#ifndef MMAPPED_VECTOR_HPP
#define MMAPPED_VECTOR_HPP

#include "MMappedVector.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/mman.h>

template <typename T>
MMappedVector<T>::MMappedVector(const char *filename, bool read_only)
{
  fd=open(filename, O_CREAT|(read_only?O_RDONLY:O_RDWR), 0600); // flawfinder: ignore
  if(fd==-1)
    throw std::runtime_error(strerror(errno));

  auto length = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

  bool empty=false;

  if(length==0) {
    empty=true;
    length=offsetof(data_t, d)+sizeof(T)*STARTING_CAPACITY;
    if(ftruncate(fd, length))
      throw std::runtime_error(strerror(errno));
  }

  mmap(length, read_only);

  if(empty) {
    data->capacity = STARTING_CAPACITY;
    data->nb_elements = 0;
  }
}

template <typename T>
void MMappedVector<T>::mmap(size_t length, bool read_only)
{
  data = reinterpret_cast<data_t *>(::mmap(
                                      NULL,
                                      length,
                                      PROT_READ|(read_only?0:PROT_WRITE),
                                      MAP_SHARED,
                                      fd,
                                      0));
  if(data == MAP_FAILED)
    throw std::runtime_error(strerror(errno));
}

template <typename T>
void MMappedVector<T>::grow()
{
  sync();
  auto new_capacity = data->capacity*2;
  munmap(data, offsetof(data_t,d)+sizeof(T)*data->capacity);

  auto new_length = offsetof(data_t,d)+sizeof(T)*new_capacity;
  if(ftruncate(fd, new_length))
    throw std::runtime_error(strerror(errno));
  mmap(new_length, false);

  data->capacity = new_capacity;
}

template <typename T>
MMappedVector<T>::~MMappedVector()
{
  munmap(data, offsetof(data_t,d)+sizeof(T)*data->capacity);
  close(fd);
}

template <typename T>
inline const T &MMappedVector<T>::operator[](unsigned long k) const
{
  return data->d[k];
}

template <typename T>
inline T &MMappedVector<T>::operator[](unsigned long k)
{
  return data->d[k];
}

template <typename T>
void MMappedVector<T>::add(const T &value)
{
  if(data->nb_elements == data->capacity)
    grow();

  data->d[data->nb_elements++] = value;
}

template <typename T>
void MMappedVector<T>::sync()
{
  msync(data, offsetof(data_t,d)+sizeof(T)*data->capacity, MS_SYNC);
}

#endif /* MMAPPED_VECTOR_HPP */
