/**
 * @file MMappedUUIDHashTable.cpp
 * @brief Open-addressing hash table over a memory-mapped file: implementation.
 *
 * Implements all methods of @c MMappedUUIDHashTable declared in
 * @c MMappedUUIDHashTable.h:
 * - @c MMappedUUIDHashTable(): open/create the backing file and map it.
 * - @c ~MMappedUUIDHashTable(): sync and unmap.
 * - @c add(): insert a UUID and assign the next sequential integer.
 * - @c operator[](): look up an integer by UUID.
 * - @c sync(): flush the backing region (@c MappedRegion::sync()).
 *
 * Internal helpers:
 * - @c grow(): double the table size and rehash.
 * - @c find(): locate the slot index for a UUID (or @c NOTHING if absent).
 * - @c set(): write a key-value pair into the table.
 */
#include "MMappedUUIDHashTable.h"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/mman.h>

MMappedUUIDHashTable::MMappedUUIDHashTable(const char *filename, bool read_only, uint64_t magic_value)
{
  path_ = filename;
  read_only_ = read_only;
  auto size = region.openFile(filename, read_only);
  bool empty = (size == 0);

  if(empty) {
    size = table_t::sizeForLogSize(STARTING_LOG_SIZE);
    region.resizeFile(size);
  }

  region.map(size);
  table = reinterpret_cast<table_t *>(region.base());

  if(empty) {
    table->magic     = magic_value;
    table->version   = 1;
    table->elem_size = static_cast<uint16_t>(sizeof(value_t));
    table->flags     = 0;
    table->log_size = table_t::logSizeForSize(size);
    table->nb_elements = 0;
    table->next_value = 0;
    for(unsigned long i=0; i<table->capacity(); ++i) {
      table->t[i].value = NOTHING;
    }
  } else {
    if(table->magic != magic_value)
      throw std::runtime_error("ProvSQL mmap: wrong file type (magic mismatch)");
    if(table->version != 1)
      throw std::runtime_error("ProvSQL mmap: unsupported format version "
                               + std::to_string(table->version));
    if(table->elem_size != sizeof(value_t))
      throw std::runtime_error("ProvSQL mmap: element size mismatch (recompile required)");
    unclean_ = (table->flags & FLAG_DIRTY) != 0;
  }

  /* Mark the file open for writing; the destructor clears it.  Found still
     set on open, it says the previous writer died mid-write, which
     provsql.check_store() reports. */
  if(!read_only)
    table->flags |= FLAG_DIRTY;
}

/* Rehashing rebuilds the whole table, so it cannot be done in place: a
   process killed part-way through an in-place rehash leaves the mapping
   of every not-yet-reinserted token gone, and each of those tokens then
   reads back as a fresh input -- silently, since an unknown token is a
   valid input gate.  Instead the new table is built in memory and handed
   to MappedRegion::replaceContents, which writes it to a sibling file,
   forces it to disk, and renames it into place: a crash at any point
   leaves either the complete old table or the complete new one. */
void MMappedUUIDHashTable::grow()
{
  const unsigned new_log_size = table->log_size + 1;
  const std::size_t new_size = table_t::sizeForLogSize(new_log_size);
  const unsigned long new_capacity = 1ul << new_log_size;

  std::vector<char> buf(new_size);
  table_t *nt = reinterpret_cast<table_t *>(buf.data());
  nt->magic       = table->magic;
  nt->version     = table->version;
  nt->elem_size   = table->elem_size;
  nt->flags       = table->flags;
  nt->log_size    = new_log_size;
  nt->nb_elements = table->nb_elements;
  nt->next_value  = table->next_value;
  for(unsigned long i=0; i<new_capacity; ++i)
    nt->t[i].value = NOTHING;

  for(unsigned long i=0; i<table->capacity(); ++i) {
    if(table->t[i].value == NOTHING)
      continue;
    const value_t &e = table->t[i];
    unsigned long k =
      (*reinterpret_cast<const unsigned long *>(&e.uuid)) % new_capacity;
    while(nt->t[k].value != NOTHING)
      k = (k + 1) % new_capacity;
    nt->t[k] = e;
  }

  region.replaceContents(path_.c_str(), buf.data(), new_size);
  table = reinterpret_cast<table_t *>(region.base());
}

MMappedUUIDHashTable::~MMappedUUIDHashTable()
{
  if(table && !read_only_)
    table->flags &= ~FLAG_DIRTY;
  region.close();
}

unsigned long MMappedUUIDHashTable::find(pg_uuid_t u) const
{
  auto k = hash(u);
  while(table->t[k].value != NOTHING &&
        std::memcmp(&table->t[k].uuid, &u, sizeof(pg_uuid_t))) {
    k = (k+1) % table->capacity();
  }

  return k;
}

unsigned long MMappedUUIDHashTable::operator[](pg_uuid_t u) const
{
  auto k = find(u);

  return table->t[k].value;
}

std::pair<unsigned long,bool> MMappedUUIDHashTable::add(pg_uuid_t u)
{
  auto k = find(u);
  if(table->t[k].value != NOTHING)
    return std::make_pair(table->t[k].value, false);
  return publish(u, table->next_value);
}

std::pair<unsigned long,bool> MMappedUUIDHashTable::publish(pg_uuid_t u,
                                                            unsigned long value)
{
  auto k = find(u);

  if(table->t[k].value != NOTHING)
    return std::make_pair(table->t[k].value, false);

  if(table->nb_elements >= MAXIMUM_LOAD_FACTOR * table->capacity())
    grow();
  k = find(u);

  ++table->nb_elements;
  table->t[k].uuid = u;
  if(value + 1 > table->next_value)
    table->next_value = value + 1;
  /* The value store publishes the entry: it is the last write, and an
     aligned 8-byte store, so a reader sees NOTHING or the whole thing. */
  table->t[k].value = value;
  return std::make_pair(value, true);
}

void MMappedUUIDHashTable::sync()
{
  region.sync();
}

void MMappedUUIDHashTable::flush()
{
  region.flush();
}
