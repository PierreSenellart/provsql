/**
 * @file MMappedUUIDHashTable.h
 * @brief Open-addressing hash table mapping UUIDs to integers, backed by an mmap file.
 *
 * @c MMappedUUIDHashTable provides a persistent hash table that maps
 * 128-bit UUID keys to sequential unsigned-long integers (used as gate
 * indices into the @c MMappedVector of @c GateInformation).  The table
 * is stored in a memory-mapped file so that it survives PostgreSQL
 * restarts and is accessible by multiple processes.
 *
 * Design constraints:
 * - **Append-only**: elements can be added but never removed.
 * - **Open addressing**: collisions are resolved by linear probing.
 * - **Trivial hash**: the first 8 bytes of the UUID are reinterpreted as
 *   a 64-bit integer and taken modulo the table capacity.  UUIDs are
 *   generated uniformly at random (version 4), so this is effectively
 *   uniform.
 * - **Automatic growth**: when the load factor exceeds @c MAXIMUM_LOAD_FACTOR
 *   the table is doubled in size and rehashed.
 *
 * Access to the table from multiple processes is serialised via the
 * ProvSQL LWLock in @c provsqlSharedState.
 */
#ifndef MMAPPED_UUID_HASH_TABLE_H
#define MMAPPED_UUID_HASH_TABLE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "MappedRegion.h"

extern "C" {
#include "provsql_utils.h"
}

/**
 * @brief Persistent open-addressing hash table mapping UUIDs to integers.
 */
class MMappedUUIDHashTable
{
/** @brief One slot in the hash table: a UUID key and its associated integer value. */
struct value_t {
  pg_uuid_t uuid;       ///< Key
  unsigned long value;  ///< Associated integer (gate index), or 0 if slot is empty
};

/**
 * @brief On-disk layout of the hash table stored in the mmap file.
 *
 * The header fields are followed by a flexible array of @c value_t slots.
 */
struct table_t {
  /**
   * @brief Compute the file size required for a table with @c 2^ls slots.
   * @param ls  Log2 of the desired slot count.
   * @return    Required file size in bytes.
   */
  static constexpr std::size_t sizeForLogSize(unsigned ls) {
    return offsetof(table_t, t) + (1 << ls)*sizeof(value_t);
  }
  /**
   * @brief Compute the log2 of the slot count from the file size.
   * @param size  File size in bytes.
   * @return      Log2 of the number of slots that fit in @p size.
   */
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
  /**
   * @brief Maximum number of slots in the table (@c 2^log_size).
   * @return Current capacity (number of available hash-table slots).
   */
  constexpr unsigned long capacity() {
    return 1u << log_size;
  }

  uint64_t magic;      ///< File-type identifier
  uint16_t version;    ///< Format version (currently 1)
  uint16_t elem_size;  ///< sizeof(value_t) at write time
  uint32_t flags;      ///< Bit 0: opened for writing and not closed since
  unsigned log_size;          ///< log2 of the number of slots
  unsigned long nb_elements;  ///< Current number of stored key-value pairs
  unsigned long next_value;   ///< Next integer value to assign to a new UUID
  value_t t[];                ///< Flexible array of hash-table slots
};

MappedRegion region;  ///< Backing storage (shared mmap, or heap buffer)
table_t *table;       ///< Typed view of @c region.base()
std::string path_;    ///< Backing file, for the atomic rehash in @c grow()
bool read_only_ = false;///< Mapped read-only: the header must not be written
bool unclean_ = false;///< The previous run left the dirty bit set

/** @brief Initial log2 capacity (65 536 slots). */
static constexpr unsigned STARTING_LOG_SIZE=16;
/** @brief Rehash when this fraction of slots is occupied. */
static constexpr double MAXIMUM_LOAD_FACTOR=.5;

/**
 * @brief Compute the starting slot index for UUID @p u.
 *
 * Reinterprets the first 8 bytes of @p u as a 64-bit integer and takes
 * it modulo the current capacity.
 * @param u  UUID to hash.
 * @return   Slot index in [0, capacity).
 */
inline unsigned long hash(pg_uuid_t u) const {
  return *reinterpret_cast<unsigned long*>(&u) % (1 << table->log_size);
};

/**
 * @brief Find the slot index of @p u, or @c NOTHING if absent.
 * @param u  UUID to look up.
 * @return   Slot index, or @c NOTHING if @p u is not in the table.
 */
unsigned long find(pg_uuid_t u) const;
/** @brief Double the table capacity and rehash all existing entries. */
void grow();

public:
/** @brief Sentinel returned by @c operator[]() when the UUID is not present. */
static constexpr unsigned long NOTHING=static_cast<unsigned long>(-1);

/**
 * @brief Open (or create) the mmap-backed hash table.
 *
 * @param filename   Path to the backing file (created if absent).
 * @param read_only  If @c true, map the file read-only (no new entries
 *                   can be inserted).
 * @param magic      Expected magic value for format validation.
 */
MMappedUUIDHashTable(const char *filename, bool read_only, uint64_t magic);
/** @brief Sync and unmap the file. */
~MMappedUUIDHashTable();

/**
 * @brief Bit of @c table_t::flags set while the file is open for writing.
 *
 * Cleared on a clean close, so a file found with it still set was left
 * behind by a process that died (or by an immediate shutdown).  Reported
 * by @c uncleanShutdown; the header layout is unchanged, since the bit
 * lives in what used to be reserved padding.
 */
static constexpr uint32_t FLAG_DIRTY = 1u;

/** @brief Whether this file still had @c FLAG_DIRTY set when it was
 *  opened, i.e. whoever wrote it last did not close it. */
inline bool uncleanShutdown() const {
  return unclean_;
}

/**
 * @brief Look up the integer index for UUID @p u.
 *
 * @param u  The UUID to look up.
 * @return   The associated integer, or @c NOTHING if @p u is absent.
 */
unsigned long operator[](pg_uuid_t u) const;

/**
 * @brief Insert UUID @p u, assigning it the next available integer.
 *
 * If @p u is already present the existing value is returned without
 * modification.
 *
 * @param u  UUID to insert.
 * @return   A pair @c {value, inserted} where @c inserted is @c true if
 *           a new entry was created.
 */
std::pair<unsigned long,bool> add(pg_uuid_t u);

/**
 * @brief Insert UUID @p u with a caller-chosen value.
 *
 * Lets the caller build the record @p value indexes *before* the key that
 * points at it becomes visible, so that a process killed between the two
 * leaves an unreferenced record rather than a mapping entry pointing past
 * the end of the record vector -- which would shift every later gate for
 * the rest of the store's life.  The slot's @c value field is written
 * last, and an aligned 8-byte store is atomic on the supported platforms,
 * so a reader sees either @c NOTHING or the complete entry.
 *
 * @param u      UUID to insert.
 * @param value  Value to associate with it.
 * @return       A pair @c {value, inserted}; @c inserted is @c false (and
 *               the existing value returned) when @p u was already there.
 */
std::pair<unsigned long,bool> publish(pg_uuid_t u, unsigned long value);

/** @brief The value the next @c add would assign.  Kept equal to the
 *  number of gate records by @c MMappedCircuit. */
inline unsigned long nextValue() const {
  return table->next_value;
}

/** @brief The number of slots the table currently has. */
inline unsigned long capacity() const {
  return table->capacity();
}

/** @brief The @p k-th slot's value, or @c NOTHING when the slot is empty.
 *  For consistency checks and for the clean-up's mark phase. */
inline unsigned long slotValue(unsigned long k) const {
  return table->t[k].value;
}

/** @brief The @p k-th slot's key; only meaningful when @c slotValue(k)
 *  is not @c NOTHING. */
inline pg_uuid_t slotKey(unsigned long k) const {
  return table->t[k].uuid;
}

/**
 * @brief Return the number of UUID→integer pairs currently stored.
 * @return Element count.
 */
inline unsigned long nbElements() const {
  return table->nb_elements;
}

/**
 * @brief Flush the backing region to its file (@c MappedRegion::sync()).
 */
void sync();

/** @brief Force the backing file to stable storage
 *  (@c MappedRegion::flush()). */
void flush();
};

 #endif /* MMAPPED_UUID_HASH_TABLE_H */
