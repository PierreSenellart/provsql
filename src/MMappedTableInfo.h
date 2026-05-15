/**
 * @file MMappedTableInfo.h
 * @brief Per-table provenance metadata persisted alongside the circuit store.
 *
 * @c ProvenanceTableInfo records, one per relation tracked by ProvSQL,
 * are stored in a fifth mmap-backed file (@c provsql_table_info.mmap)
 * inside each database's @c $PGDATA/base/&lt;db_oid&gt;/ directory.
 * They feed the safe-query optimisation: the planner-time hierarchy
 * detector needs to know whether each base relation is TID
 * (independent leaves; default after @c add_provenance) or BID
 * (block-correlated leaves; produced by @c repair_key) before it can
 * decide whether a query is safe to rewrite into read-once form.
 *
 * The file uses the same 16-byte header convention as every other
 * ProvSQL mmap file (magic / version / elem_size / _reserved); records
 * are fixed-stride so we can back it with @c MMappedVector directly,
 * without introducing a second variable-length region.
 *
 * @warning ON-DISK ABI: the layout of @c ProvenanceTableInfo is
 * serialised verbatim into @c provsql_table_info.mmap.  Adding,
 * removing, or resizing a field requires bumping the mmap file format
 * version and providing a migration path, exactly as for
 * @c GateInformation in @c MMappedCircuit.h.
 */
#ifndef MMAPPED_TABLE_INFO_H
#define MMAPPED_TABLE_INFO_H

#include <cstdint>

extern "C" {
#include "postgres.h"
#include "access/attnum.h"
}

/**
 * @brief Cap on the number of block-key columns recorded per relation.
 *
 * BID tables (produced by @c repair_key) can have multi-column keys.
 * We store the column numbers inline in a fixed-size array so each
 * record is fixed-stride and @c MMappedVector can back the file
 * directly.  Sixteen is generous in practice — provenance-tracked
 * tables rarely use composite keys wider than a handful of columns.
 * @c repair_key raises a clear error if a wider key is requested.
 */
#define PROVSQL_TABLE_INFO_MAX_BLOCK_KEY 16

/**
 * @brief Per-relation metadata for the safe-query optimisation.
 *
 * One record per provenance-tracked relation.  @c relid is the
 * @c pg_class OID of the relation and acts as the primary key during
 * linear lookup; @c tid is @c true iff provenance tokens for the
 * relation are independent input leaves (the post-@c add_provenance
 * default).  @c repair_key flips @c tid to @c false and records the
 * block-key columns in @c block_key[0..block_key_n-1] so the hierarchy
 * detector can confirm the separator variable's binding columns are a
 * superset of the block key.
 */
typedef struct ProvenanceTableInfo {
  Oid relid;                                         ///< pg_class OID of the relation (primary key)
  bool tid;                                          ///< @c true if leaves are independent (TID), @c false if BID
  uint16_t block_key_n;                              ///< Number of valid entries in @c block_key (0 for TID)
  AttrNumber block_key[PROVSQL_TABLE_INFO_MAX_BLOCK_KEY]; ///< Block-key column numbers
} ProvenanceTableInfo;

#endif /* MMAPPED_TABLE_INFO_H */
