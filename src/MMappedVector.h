#ifndef MMAPPED_VECTOR_H
#define MMAPPED_VECTOR_H

#include <cstddef>
#include <vector>

extern "C" {
#include "provsql_utils.h"
}

/*
 * Vector template, stored in an mmaped file; can only grow, impossible
 * to remove elements
 */
template <typename T>
class MMappedVector {
struct data_t {
  unsigned long nb_elements;
  unsigned long capacity;
  T d[];
};

int fd;
data_t *data;

static constexpr unsigned STARTING_CAPACITY=(1u << 16);

void mmap(size_t length, bool read_only);
void grow();
void set(pg_uuid_t u, unsigned long i);

public:
MMappedVector(const char *filename, bool read_only);
~MMappedVector();

const T &operator[](unsigned long k) const;
T &operator[](unsigned long k);
void add(const T& value);
inline unsigned long nbElements() const {
  return data->nb_elements;
}
void sync();
};

 #endif /* MMAPPED_VECTOR_H */
