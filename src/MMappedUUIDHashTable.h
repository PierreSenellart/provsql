#ifndef MMAPPED_UUID_HASH_TABLE_H
#define MMAPPED_UUID_HASH_TABLE_H

#include <cstddef>
#include <utility>

extern "C" {
#include "provsql_utils.h"
}

/*
 * Open-addressing hash table meant to map UUIDs to integers, stored in
 * an mmaped file; can only grow, impossible to remove elements; we use
 * the fact that keys are UUIDs to have a trivial hash function
 */
class MMappedUUIDHashTable
{
struct value_t {
  pg_uuid_t uuid;
  unsigned long value;
};

struct table_t {
  static constexpr std::size_t sizeForLogSize(unsigned ls) {
    return offsetof(table_t, t) + (1 << ls)*sizeof(value_t);
  }
  static constexpr unsigned logSizeForSize(std::size_t size) {
    size -= offsetof(table_t, t);
    size /= sizeof(value_t);
    size >>= 1;
    unsigned log_size=0;
    while(size) {
      size >>= 1;
      ++log_size;
    }
    return log_size;
  }
  constexpr unsigned long capacity() {
    return 1u << log_size;
  }

  unsigned log_size;
  unsigned long nb_elements;
  unsigned long next_value;
  value_t t[];
};

int fd;
table_t *table;

static constexpr unsigned STARTING_LOG_SIZE=16;
static constexpr double MAXIMUM_LOAD_FACTOR=.5;

inline unsigned long hash(pg_uuid_t u) const {
  return *reinterpret_cast<unsigned long*>(&u) % (1 << table->log_size);
};

unsigned long find(pg_uuid_t u) const;
void mmap(size_t length, bool read_only);
void grow();
void set(pg_uuid_t u, unsigned long i);

public:
static constexpr unsigned long NOTHING=static_cast<unsigned long>(-1);
MMappedUUIDHashTable(const char *filename, bool read_only);
~MMappedUUIDHashTable();

unsigned long operator[](pg_uuid_t u) const;
std::pair<unsigned long,bool> add(pg_uuid_t u);
inline unsigned long nbElements() const {
  return table->nb_elements;
}
void sync();
};

 #endif /* MMAPPED_UUID_HASH_TABLE_H */
