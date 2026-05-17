/**
 * @file classify_query.h
 * @brief Public surface of the query-time TID / BID / OPAQUE classifier.
 *
 * The classifier is invoked by @c provsql_planner on the top-level
 * @c Query when the @c provsql.classify_top_level GUC is on, and emits a
 * @c NOTICE carrying the certified kind and the set of
 * provenance-tracked base relations the query touches.  Independent-
 * TID join inference, BID block-key preservation under projection
 * and @c GROUP @c BY, transitive ancestor-set computation through
 * the per-relation registry, view descent, and ANSI @c INNER /
 * @c CROSS @c JOIN handling all live in this same file's helpers
 * and the matching propagation pre-passes in
 * @c src/safe_query.c.
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
 * Scope :
 *
 *  - Single-source classification : a flat @c fromlist of
 *    @c RangeTblRefs, with no kind-altering features (@c SubLinks,
 *    modifying @c CTEs, @c cteList, @c DISTINCT, @c GROUP @c BY,
 *    @c HAVING, aggregates, window functions, set-returning
 *    functions in the target list).  Zero or one provenance-tracked
 *    base relation reached either directly (@c RTE_RELATION) or
 *    through any depth of @c RTE_SUBQUERY entries (view bodies
 *    after PG rewriting, inline @c FROM subqueries).  The PG 18
 *    virtual @c RTE_GROUP is skipped transparently.  When a single
 *    tracked base relation is reached, the source's recorded kind
 *    is preserved verbatim.  @c ORDER @c BY, @c LIMIT, @c OFFSET
 *    do not change row lineages and are therefore transparent.
 *  - @c UNION @c ALL specialisation : a fully-UNION-ALL tree of
 *    subquery legs each of which classifies as TID over a base-
 *    relid set that is disjoint from every other leg's promotes
 *    to TID with the cumulative source list.
 *  - Zero tracked sources : trivially deterministic, reported as
 *    TID with an empty source list.
 *  - Everything else is reported as OPAQUE.
 *
 *  - Multi-source TID promotion : @c n_meta @c >= @c 2 promotes
 *    to TID when every classifier-reported source is TID and the
 *    registered ancestor sets are pairwise disjoint.
 *  - BID projection preservation : the single-source BID branch
 *    walks the outer target list (transitively through any depth
 *    of @c RTE_SUBQUERY TLE projection) and downgrades to OPAQUE
 *    when any block-key column is dropped.
 *  - BID @c GROUP @c BY block-key promotion : a pre-dispatch
 *    special case promotes @c SELECT @c k @c FROM @c bid_t
 *    @c GROUP @c BY @c k to TID (each output row is one block).
 *  - ANSI @c INNER / @c CROSS @c JOIN : the shape gate accepts
 *    @c JoinExpr fromlist entries when @c jointype @c ==
 *    @c JOIN_INNER, recursing into both arms.
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
 *   ProvSQL: query result is BID (sources: schema.t1)
 *   ProvSQL: query result is TID (no provenance-tracked sources)
 *   ProvSQL: query result is OPAQUE
 * @endverbatim
 *
 * The OPAQUE form omits the parenthetical because the rtable
 * walk only reaches syntactically visible sources : when the
 * shape gate trips on a sublink, set operation, GROUP BY, ...
 * the list would be partial and falsely suggest completeness.
 */
extern void provsql_classify_emit_notice(const ProvSQLClassification *c);

#endif /* CLASSIFY_QUERY_H */
