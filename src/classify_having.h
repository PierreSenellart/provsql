/**
 * @file classify_having.h
 * @brief Query-time HAVING-trichotomy classifier (read-only NOTICE surface).
 *
 * When @c provsql.classify_having is on, @c provsql_planner runs this
 * classifier on every top-level @c SELECT carrying a @c HAVING aggregate
 * comparison.  For each comparison @c alpha(y) @c theta @c k it emits a
 * @c NOTICE labelling the pair under the Ré-Suciu HAVING trichotomy
 * (:cite:`DBLP:journals/vldb/ReS09`) as one of safe (exact PTIME),
 * apx-safe (exact \#P-hard but an FPRAS exists), hazardous (no FPRAS), or
 * open -- combining the static @c (alpha, theta) overlay with the
 * skeleton-safety axis from @c safe_query_skeleton_is_hierarchical.
 *
 * Read-only and purely diagnostic: it never rewrites the query and does
 * not change evaluation.  See the "HAVING Query Complexity" section of
 * @c doc/source/dev/probability-evaluation.rst for the verdict tables.
 */
#ifndef CLASSIFY_HAVING_H
#define CLASSIFY_HAVING_H

#include "postgres.h"
#include "nodes/parsenodes.h"

#include "provsql_utils.h"

/** @brief GUC: emit a HAVING-trichotomy NOTICE on every top-level SELECT. */
extern bool provsql_classify_having;

/**
 * @brief Classify the HAVING aggregate comparisons of @p q and emit one
 *        @c NOTICE per comparison.
 *
 * No-op when @p q has no @c HAVING clause.  Read-only; @p q is not
 * modified.  Skeleton safety is computed once per query via
 * @c safe_query_skeleton_is_hierarchical.
 *
 * @param q          Parsed top-level @c Query.
 * @param constants  Cached extension OIDs (from @c get_constants).
 */
extern void provsql_emit_having_classification(Query *q, const constants_t *constants);

#endif /* CLASSIFY_HAVING_H */
