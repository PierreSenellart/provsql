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

#ifdef __cplusplus
#include <cstdint>
#else
#include <stdint.h>
#endif

#include "postgres.h"
#include "access/attnum.h"

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
 * @brief How the provenance leaves of a tracked relation are correlated.
 *
 * Three cases need distinguishing for the safe-query rewriter:
 *
 *  - @c PROVSQL_TABLE_TID -- independent input leaves; the
 *    post-@c add_provenance default.  Each row's provenance token is a
 *    fresh @c gate_input with its own probability.
 *  - @c PROVSQL_TABLE_BID -- block-correlated leaves produced by
 *    @c repair_key.  Rows sharing the same value of @c block_key are
 *    mutually exclusive (they originate from a single block
 *    @c gate_input via @c gate_mulinput children).  An empty
 *    @c block_key means the whole table is one block.
 *  - @c PROVSQL_TABLE_OPAQUE -- correlations are unknown.  Used for
 *    relations whose provenance is derived from a tracked source via
 *    @c CREATE @c TABLE @c AS @c SELECT, @c INSERT @c INTO @c SELECT,
 *    or @c UPDATE under @c provsql.update_provenance.  The safe-query
 *    rewriter must bail on these.
 *
 * Stored as @c uint8_t in @c ProvenanceTableInfo so the on-disk size
 * matches the previous @c bool field exactly.
 *
 * @warning ON-DISK ABI: these integer values are persisted in
 * @c provsql_table_info.mmap.  Do not reorder or renumber existing
 * members; new kinds must be appended.
 */
typedef enum provsql_table_kind {
  PROVSQL_TABLE_TID    = 0,
  PROVSQL_TABLE_BID    = 1,
  PROVSQL_TABLE_OPAQUE = 2
} provsql_table_kind;

/**
 * @brief Per-relation metadata for the safe-query optimisation.
 *
 * One record per provenance-tracked relation.  @c relid is the
 * @c pg_class OID of the relation and acts as the primary key during
 * linear lookup.  @c kind discriminates between TID, BID, and OPAQUE
 * (see @c provsql_table_kind).  For BID, @c block_key[0..block_key_n-1]
 * lists the column numbers whose tuples partition the table into
 * mutually-exclusive blocks; an empty key means the whole table is
 * one block.  @c block_key is left empty for TID and OPAQUE.
 */
typedef struct ProvenanceTableInfo {
  Oid relid;                                         ///< pg_class OID of the relation (primary key)
  uint8_t kind;                                      ///< One of @c provsql_table_kind
  uint16_t block_key_n;                              ///< Number of valid entries in @c block_key
  AttrNumber block_key[PROVSQL_TABLE_INFO_MAX_BLOCK_KEY]; ///< Block-key column numbers
} ProvenanceTableInfo;

#endif /* MMAPPED_TABLE_INFO_H */
