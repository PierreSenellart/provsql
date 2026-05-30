/**
 * @file MappedRegion.h
 * @brief File-backed memory region with two interchangeable backends.
 *
 * A @c MappedRegion owns a backing file and a base pointer to @c length()
 * bytes of it, with @c map() / @c remap() / @c sync() / @c close().  It is
 * the single storage primitive under @c MMappedVector and
 * @c MMappedUUIDHashTable.
 *
 * Multi-process build: the region is a shared (@c MAP_SHARED) @c mmap of
 * the file.  The kernel keeps the mapping coherent across the backends and
 * the worker and flushes dirty pages, so a backend's writes are visible to
 * the others through the same file.
 *
 * Single-process build (@c PROVSQL_INPROCESS_STORE): the region is a heap
 * buffer loaded from the file on @c map() and written back explicitly on
 * @c sync() / @c close().  Emscripten does not support @c MAP_SHARED
 * write-back, and with a single process a shared mapping has no purpose;
 * the file still lives under @c $PGDATA, so PGlite persists it.  Write-back
 * timing is the caller's responsibility (the store registers an
 * @c on_proc_exit hook so a backend flushes before it exits).
 */
#ifndef MAPPED_REGION_H
#define MAPPED_REGION_H

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <unistd.h>

#include "provsql_config.h"

#ifndef PROVSQL_INPROCESS_STORE
#include <sys/mman.h>
#endif

class MappedRegion {
int fd_ = -1;            ///< Backing file descriptor
void *base_ = nullptr;   ///< Base of the mapped region / heap buffer
std::size_t length_ = 0; ///< Current region length in bytes
bool read_only_ = false; ///< Opened read-only (no write-back)

public:
MappedRegion() = default;
MappedRegion(const MappedRegion &) = delete;
MappedRegion &operator=(const MappedRegion &) = delete;

/**
 * @brief Open (creating if absent) the backing file.
 * @return The file's current size in bytes (0 if newly created).
 */
std::size_t openFile(const char *filename, bool read_only) {
  read_only_ = read_only;
  fd_ = open(filename, O_CREAT | (read_only ? O_RDONLY : O_RDWR), 0600); // flawfinder: ignore
  if(fd_ == -1)
    throw std::runtime_error(strerror(errno));
  auto size = lseek(fd_, 0, SEEK_END);
  lseek(fd_, 0, SEEK_SET);
  return static_cast<std::size_t>(size);
}

/** @brief Set the backing file's size.
 *
 * The shared-mmap backend must pre-size the file (mmap maps file-backed
 * pages).  The heap-buffer backend does not: it allocates the buffer and
 * @c sync() extends the file with @c pwrite.  Crucially, leaving the file
 * unsized until the first @c sync() means a fresh file that is never
 * synced (e.g. a backend that aborts before write-back) stays empty on
 * disk and is re-initialised cleanly on reopen, rather than persisting as
 * a full-size, never-written file whose zero header fails magic
 * validation. */
void resizeFile(std::size_t length) {
#ifdef PROVSQL_INPROCESS_STORE
  (void) length;
#else
  if(ftruncate(fd_, length))
    throw std::runtime_error(strerror(errno));
#endif
}

/** @brief Establish the initial region of @p length bytes over the file. */
void map(std::size_t length) {
#ifdef PROVSQL_INPROCESS_STORE
  base_ = malloc(length);
  if(!base_)
    throw std::runtime_error("ProvSQL: out of memory mapping region");
  ssize_t r = pread(fd_, base_, length, 0); // flawfinder: ignore
  if(r < 0)
    throw std::runtime_error(strerror(errno));
  if(static_cast<std::size_t>(r) < length)
    memset(static_cast<char *>(base_) + r, 0, length - static_cast<std::size_t>(r));
#else
  base_ = ::mmap(nullptr, length, PROT_READ | (read_only_ ? 0 : PROT_WRITE),
                 MAP_SHARED, fd_, 0);
  if(base_ == MAP_FAILED)
    throw std::runtime_error(strerror(errno));
#endif
  length_ = length;
}

/** @brief Grow the region to @p new_length, preserving existing content. */
void remap(std::size_t new_length) {
#ifdef PROVSQL_INPROCESS_STORE
  resizeFile(new_length);
  void *p = realloc(base_, new_length);
  if(!p)
    throw std::runtime_error("ProvSQL: out of memory growing region");
  base_ = p;
  if(new_length > length_)
    memset(static_cast<char *>(base_) + length_, 0, new_length - length_);
#else
  if(::munmap(base_, length_))
    throw std::runtime_error(strerror(errno));
  resizeFile(new_length);
  base_ = ::mmap(nullptr, new_length, PROT_READ | (read_only_ ? 0 : PROT_WRITE),
                 MAP_SHARED, fd_, 0);
  if(base_ == MAP_FAILED)
    throw std::runtime_error(strerror(errno));
#endif
  length_ = new_length;
}

/** @brief Flush the region to the backing file (no-op when read-only). */
void sync() {
  if(read_only_ || !base_)
    return;
#ifdef PROVSQL_INPROCESS_STORE
  if(pwrite(fd_, base_, length_, 0) < 0) // flawfinder: ignore
    throw std::runtime_error(strerror(errno));
#else
  msync(base_, length_, MS_SYNC);
#endif
}

/** @brief Write back (if writable) and release the region and file. */
void close() {
  if(base_) {
#ifdef PROVSQL_INPROCESS_STORE
    sync();
    free(base_);
#else
    ::munmap(base_, length_);
#endif
    base_ = nullptr;
  }
  if(fd_ != -1) {
    ::close(fd_);
    fd_ = -1;
  }
}

void *base() const { return base_; }
std::size_t length() const { return length_; }
};

#endif /* MAPPED_REGION_H */
