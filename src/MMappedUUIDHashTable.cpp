#include "MMappedUUIDHashTable.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/mman.h>

MMappedUUIDHashTable::MMappedUUIDHashTable(const char *filename, bool read_only)
{
  fd=open(filename, O_CREAT|(read_only?O_RDONLY:O_RDWR), 0600); // flawfinder: ignore
  if(fd==-1)
    throw std::runtime_error(strerror(errno));

  auto size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

  bool empty=false;

  if(size==0) {
    empty=true;
    size=table_t::sizeForLogSize(STARTING_LOG_SIZE);
    if(ftruncate(fd, size))
      throw std::runtime_error(strerror(errno));
  }

  mmap(size, read_only);

  if(empty) {
    table->log_size = table_t::logSizeForSize(size);
    table->nb_elements = 0;
    table->next_value = 0;
    for(unsigned i=0; i<table->capacity(); ++i) {
      table->t[i].value = NOTHING;
    }
  }
}

void MMappedUUIDHashTable::mmap(size_t length, bool read_only)
{
  table = reinterpret_cast<table_t *>(::mmap(
                                        NULL,
                                        length,
                                        PROT_READ|(read_only?0:PROT_WRITE),
                                        MAP_SHARED_VALIDATE,
                                        fd,
                                        0));
  if(table == MAP_FAILED)
    throw std::runtime_error(strerror(errno));
}

void MMappedUUIDHashTable::grow()
{
  std::vector<value_t> elements;
  elements.reserve(table->nb_elements);
  for(unsigned i=0; i<table->capacity(); ++i)
    if(table->t[i].value != NOTHING)
      elements.push_back(table->t[i]);

  auto new_log_size = table->log_size+1;
  assert(new_log_size<sizeof(unsigned)*8);
  munmap(table, table_t::sizeForLogSize(table->log_size));

  auto new_size = table_t::sizeForLogSize(new_log_size);
  if(ftruncate(fd, new_size))
    throw std::runtime_error(strerror(errno));
  mmap(new_size, false);

  table->log_size = new_log_size;
  for(unsigned i=0; i<table->capacity(); ++i) {
    table->t[i].value = NOTHING;
  }
  for(const auto &u: elements)
    set(u.uuid, u.value);
}

MMappedUUIDHashTable::~MMappedUUIDHashTable()
{
  munmap(table, table_t::sizeForLogSize(table->log_size));
  close(fd);
}

unsigned MMappedUUIDHashTable::find(pg_uuid_t u) const
{
  unsigned k = hash(u);
  while(table->t[k].value != NOTHING &&
        std::memcmp(&table->t[k].uuid, &u, sizeof(pg_uuid_t))) {
    k = (k+1) % table->capacity();
  }

  return k;
}

unsigned MMappedUUIDHashTable::operator[](pg_uuid_t u) const
{
  unsigned k = find(u);

  return table->t[k].value;
}

std::pair<unsigned,bool> MMappedUUIDHashTable::add(pg_uuid_t u)
{
  unsigned k = find(u);

  if(table->t[k].value == NOTHING) {
    if(table->nb_elements >= MAXIMUM_LOAD_FACTOR * table->capacity()) {
      grow();
    }
    k = find(u);
    ++table->nb_elements;
    table->t[k].uuid = u;
    return std::make_pair(table->t[k].value = table->next_value++, true);
  } else
    return std::make_pair(table->t[k].value, false);
}

// Only used when growing the table, so no need to check/update nb_elements
void MMappedUUIDHashTable::set(pg_uuid_t u, unsigned i)
{
  table->t[find(u)] = {u, i};
}
