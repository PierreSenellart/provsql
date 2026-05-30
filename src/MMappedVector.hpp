/**
 * @file MMappedVector.hpp
 * @brief Template implementation of @c MMappedVector<T>.
 *
 * Provides the out-of-line definitions of all @c MMappedVector<T>
 * methods declared in @c MMappedVector.h.  This file must be included
 * by any translation unit that instantiates @c MMappedVector<T> for a
 * specific @c T.
 *
 * Implemented methods:
 * - @c MMappedVector(): open/create the backing file and map it.
 * - @c ~MMappedVector(): sync and unmap.
 * - @c operator[](k) const: read element @p k.
 * - @c operator[](k): write element @p k.
 * - @c add(): append one element, growing the file if necessary.
 * - @c sync(): flush the backing region (@c MappedRegion::sync()).
 *
 * Internal helpers:
 * - @c grow(): double the capacity and remap the backing region.
 */
#ifndef MMAPPED_VECTOR_HPP
#define MMAPPED_VECTOR_HPP

#include "MMappedVector.h"

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

template <typename T>
MMappedVector<T>::MMappedVector(const char *filename, bool read_only, uint64_t magic_value)
{
  auto length = region.openFile(filename, read_only);
  bool empty = (length == 0);

  if(empty) {
    length = offsetof(data_t, d) + sizeof(T) * STARTING_CAPACITY;
    region.resizeFile(length);
  }

  region.map(length);
  data = reinterpret_cast<data_t *>(region.base());

  if(empty) {
    data->magic     = magic_value;
    data->version   = 1;
    data->elem_size = static_cast<uint16_t>(sizeof(T));
    data->_reserved = 0;
    data->capacity    = STARTING_CAPACITY;
    data->nb_elements = 0;
  } else {
    if(data->magic != magic_value)
      throw std::runtime_error("ProvSQL mmap: wrong file type (magic mismatch)");
    if(data->version != 1)
      throw std::runtime_error("ProvSQL mmap: unsupported format version "
                               + std::to_string(data->version));
    if(data->elem_size != sizeof(T))
      throw std::runtime_error("ProvSQL mmap: element size mismatch (recompile required)");
  }
}

template <typename T>
void MMappedVector<T>::grow()
{
  auto new_capacity = data->capacity*2;
  region.remap(offsetof(data_t,d)+sizeof(T)*new_capacity);
  data = reinterpret_cast<data_t *>(region.base());
  data->capacity = new_capacity;
}

template <typename T>
MMappedVector<T>::~MMappedVector()
{
  region.close();
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
  region.sync();
}

#endif /* MMAPPED_VECTOR_HPP */
