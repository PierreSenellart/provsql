/**
 * @file MMappedVector.h
 * @brief Append-only vector template backed by a memory-mapped file.
 *
 * @c MMappedVector<T> provides a @c std::vector-like interface over a
 * memory-mapped file, enabling the provenance circuit data structures
 * (@c GateInformation, child UUID lists, extra string data) to survive
 * PostgreSQL restarts and be shared across processes.
 *
 * Design constraints:
 * - **Append-only**: elements are added with @c add(); existing elements
 *   can be updated in-place via @c operator[]() but cannot be removed.
 * - **Automatic growth**: when capacity is exhausted the backing file is
 *   grown by a factor of two and remapped.
 * - @c T must be a trivially copyable type so that it can be stored
 *   directly in the memory-mapped region.
 *
 * The implementation is in @c MMappedVector.hpp (included from this header).
 */
#ifndef MMAPPED_VECTOR_H
#define MMAPPED_VECTOR_H

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" {
#include "provsql_utils.h"
}

/**
 * @brief Append-only, mmap-backed vector of elements of type @c T.
 *
 * @tparam T  The element type.  Must be trivially copyable.
 */
template <typename T>
class MMappedVector {
/**
 * @brief On-disk layout stored at the start of the backing file.
 *
 * @c d is a flexible array member holding the actual elements.
 */
struct data_t {
  uint64_t magic;      ///< File-type identifier
  uint16_t version;    ///< Format version (currently 1)
  uint16_t elem_size;  ///< sizeof(T) at write time
  uint32_t _reserved;  ///< Padding to 16-byte boundary, must be 0
  unsigned long nb_elements;  ///< Number of elements currently stored
  unsigned long capacity;     ///< Maximum elements before the next grow
  T d[];                      ///< Flexible array of elements
};

int fd;        ///< File descriptor of the backing mmap file
data_t *data;  ///< Pointer to the memory-mapped data header

/** @brief Initial number of element slots allocated. */
static constexpr unsigned STARTING_CAPACITY=(1u << 16);

/**
 * @brief Map @p length bytes from the backing file.
 * @param length     Number of bytes to map.
 * @param read_only  If @c true, map read-only.
 */
void mmap(size_t length, bool read_only);
/** @brief Double the backing file and remap. */
void grow();
/**
 * @brief Unused overload kept for interface compatibility.
 * @param u  Ignored UUID parameter.
 * @param i  Ignored integer parameter.
 */
void set(pg_uuid_t u, unsigned long i);

public:
/**
 * @brief Open (or create) the mmap-backed vector.
 *
 * @param filename   Path to the backing file (created with
 *                   @c STARTING_CAPACITY slots if absent).
 * @param read_only  If @c true, map the file read-only.
 * @param magic      Expected magic value for format validation.
 */
MMappedVector(const char *filename, bool read_only, uint64_t magic);
/** @brief Sync and unmap the file. */
~MMappedVector();

/**
 * @brief Read-only element access by index.
 * @param k  Zero-based element index.
 * @return   Const reference to element @p k.
 */
const T &operator[](unsigned long k) const;

/**
 * @brief Read-write element access by index.
 * @param k  Zero-based element index.
 * @return   Reference to element @p k.
 */
T &operator[](unsigned long k);

/**
 * @brief Append an element to the end of the vector.
 *
 * Grows the backing file if the current capacity is exhausted.
 *
 * @param value  Element to append.
 */
void add(const T& value);

/**
 * @brief Return the number of elements currently stored.
 * @return Element count.
 */
inline unsigned long nbElements() const {
  return data->nb_elements;
}

/** @brief Flush dirty pages to the backing file with @c msync(). */
void sync();
};

 #endif /* MMAPPED_VECTOR_H */
