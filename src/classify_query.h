/**
 * @file classify_query.h
 * @brief Public surface of the query-time TID / BID / OPAQUE classifier.
 *
 * The classifier is invoked by @c provsql_planner on the top-level
 * @c Query when the @c provsql.classify_top_level GUC is on, and emits a
 * @c NOTICE carrying the certified kind and the set of
 * provenance-tracked base relations the query touches.  See
 * @c doc/TODO/safe-query-followups.md ("TID / BID propagation through
 * derived relations") for the broader design and follow-up slices
 * (CTAS tag inheritance, inter-relation correlation registry, view
 * descent).
 */
#ifndef CLASSIFY_QUERY_H
#define CLASSIFY_QUERY_H

#include "postgres.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"

#include "MMappedTableInfo.h"

/** @brief GUC: emit a classification @c NOTICE on every top-level SELECT. */
extern bool provsql_classify_top_level;

/**
 * @brief Result of @c provsql_classify_query.
 *
 * @c kind is the certified result-relation kind under the existing
 * @c provsql_table_kind taxonomy (TID, BID, OPAQUE).
 *
 * @c source_relids is a @c List of @c pg_class @c Oids of the
 * provenance-tracked base relations the query touches, built via
 * @c lappend_oid.  Reported even when @c kind is OPAQUE so the caller
 * can attribute the non-certifiability to specific tables.  Allocated
 * in the current memory context; callers may free with @c list_free.
 */
typedef struct ProvSQLClassification {
  provsql_table_kind kind;
  List              *source_relids;
} ProvSQLClassification;

/**
 * @brief Classify the result relation of a parsed top-level @c Query.
 *
 * Initial scope : a flat @c fromlist of @c RangeTblRefs, with no
 * @c SubLinks, no modifying @c CTEs, no set operations, and either
 * zero or one provenance-tracked base relation reached either
 * directly (@c RTE_RELATION) or through any depth of @c RTE_SUBQUERY
 * entries (view bodies after PG rewriting, and inline @c FROM
 * subqueries).  The PG 18 virtual @c RTE_GROUP is transparently
 * skipped.  When a single tracked base relation is reached, the
 * source's recorded kind is preserved.  A FROM-less query and any
 * query whose accessible base relations carry no provenance
 * metadata are reported as TID (trivially deterministic).
 * Everything else is reported as OPAQUE.
 *
 * Follow-up slices add joins of independent TIDs, UNION ALL,
 * GROUP BY refinement (especially the BID block-key check), and
 * the transitive ancestor-set computation that the future
 * correlation registry will consume.
 *
 * @param q    Parsed @c Query to classify.  Read-only; not mutated.
 * @param out  Output struct.  @c source_relids is built in the current
 *             memory context.
 */
extern void provsql_classify_query(Query *q, ProvSQLClassification *out);

/**
 * @brief Render the result of @c provsql_classify_query as a @c NOTICE.
 *
 * Formats :
 * @verbatim
 *   ProvSQL: query result is TID (sources: schema.t1, schema.t2)
 *   ProvSQL: query result is OPAQUE (sources: schema.t1)
 *   ProvSQL: query result is TID (no provenance-tracked sources)
 * @endverbatim
 */
extern void provsql_classify_emit_notice(const ProvSQLClassification *c);

#endif /* CLASSIFY_QUERY_H */
