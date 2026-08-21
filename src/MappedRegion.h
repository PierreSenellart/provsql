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
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "provsql_config.h"

#ifndef PROVSQL_INPROCESS_STORE
#include <sys/mman.h>
#endif

/**
 * @brief Push a file's dirty data pages to stable storage.
 *
 * @c fdatasync where the platform has it; macOS does not declare it,
 * and its @c fsync only reaches the drive's cache, so there the
 * @c F_FULLFSYNC fcntl (what PostgreSQL itself issues) is the durable
 * barrier, with @c fsync as the fallback on file systems that reject it.
 * @return 0 on success, -1 with @c errno set otherwise.
 */
static inline int provsql_fdatasync(int fd) {
#if defined(__APPLE__)
  if(fcntl(fd, F_FULLFSYNC) == 0)
    return 0;
  return fsync(fd);
#else
  return fdatasync(fd);
#endif
}

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

/**
 * @brief Force the backing file's contents to stable storage.
 *
 * @c sync() pushes the region's bytes into the file; this pushes the
 * file's dirty pages out of the kernel's cache, which is what a crash of
 * the machine (as opposed to a crash of PostgreSQL) can otherwise lose.
 * @c fdatasync on the descriptor rather than @c msync on the mapping:
 * it flushes the file's dirty pages whoever dirtied them, and does not
 * walk the mapping.
 */
void flush() {
  if(read_only_ || fd_ == -1)
    return;
#ifdef PROVSQL_INPROCESS_STORE
  sync();
#endif
  if(provsql_fdatasync(fd_) && errno != EINVAL)
    throw std::runtime_error(strerror(errno));
}

/**
 * @brief Replace the backing file, atomically, with @p length bytes of
 *        @p data, and remap onto the result.
 *
 * Used to rewrite a region whose new contents cannot be derived from the
 * old ones in place -- rehashing the UUID table.  The bytes go to a
 * sibling file, are forced to disk, and then @c rename(2) puts them in
 * place in one step, so a crash at any point leaves either the complete
 * old file or the complete new one.
 *
 * @param path  Path of the backing file (the same one @c openFile opened).
 */
void replaceContents(const char *path, const void *data, std::size_t length) {
  if(read_only_)
    throw std::runtime_error("ProvSQL mmap: cannot replace a read-only region");
#ifdef PROVSQL_INPROCESS_STORE
  /* Single process, no shared mapping: swap the heap buffer and let the
     next sync() write it back.  Emscripten has no directory fsync and no
     crash window to protect against here. */
  void *p = realloc(base_, length);
  if(!p)
    throw std::runtime_error("ProvSQL: out of memory replacing region");
  base_ = p;
  memcpy(base_, data, length);
  length_ = length;
  sync();
  (void) path;
#else
  std::string tmp = std::string(path) + ".new";
  int tfd = open(tmp.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600); // flawfinder: ignore
  if(tfd == -1)
    throw std::runtime_error(strerror(errno));
  const char *p = static_cast<const char *>(data);
  std::size_t left = length;
  while(left > 0) {
    ssize_t w = write(tfd, p, left);
    if(w <= 0) {
      int e = errno;
      ::close(tfd);
      unlink(tmp.c_str());
      throw std::runtime_error(strerror(e));
    }
    left -= static_cast<std::size_t>(w);
    p += w;
  }
  if(provsql_fdatasync(tfd) || rename(tmp.c_str(), path)) {
    int e = errno;
    ::close(tfd);
    unlink(tmp.c_str());
    throw std::runtime_error(strerror(e));
  }
  ::close(tfd);
  syncDirectoryOf(path);

  if(base_ && ::munmap(base_, length_))
    throw std::runtime_error(strerror(errno));
  base_ = nullptr;
  if(fd_ != -1)
    ::close(fd_);
  fd_ = open(path, O_RDWR); // flawfinder: ignore
  if(fd_ == -1)
    throw std::runtime_error(strerror(errno));
  base_ = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if(base_ == MAP_FAILED)
    throw std::runtime_error(strerror(errno));
  length_ = length;
#endif
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

private:
#ifndef PROVSQL_INPROCESS_STORE
/** @brief Force the directory entry of @p path to disk, so the rename
 *  that created it survives a crash of the machine. */
static void syncDirectoryOf(const char *path) {
  std::string dir(path);
  auto slash = dir.rfind('/');
  dir = (slash == std::string::npos) ? "." : dir.substr(0, slash);
  int dfd = open(dir.c_str(), O_RDONLY); // flawfinder: ignore
  if(dfd == -1)
    return;
  fsync(dfd);
  ::close(dfd);
}
#endif
};

#endif /* MAPPED_REGION_H */
