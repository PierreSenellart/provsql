/**
 * @file safe_query.h
 * @brief Public surface of the safe-query (hierarchical-CQ) rewriter.
 *
 * The implementation lives in @c safe_query.c.  ProvSQL's main planner
 * hook (@c process_query in @c provsql.c) calls @c try_safe_query_rewrite
 * between the AGG-DISTINCT rewrite and @c get_provenance_attributes,
 * but only when the @c provsql.boolean_provenance GUC is on.  Returning
 * a non-NULL @c Query feeds the rewriter's output back through
 * @c process_query from the top; returning @c NULL makes the caller
 * fall through to the existing pipeline.
 */
#ifndef SAFE_QUERY_H
#define SAFE_QUERY_H

#include "postgres.h"
#include "nodes/parsenodes.h"

#include "provsql_utils.h"

/** @brief GUC: opt-in safe-query optimisation, declared in @c provsql.c. */
extern bool provsql_boolean_provenance;

/**
 * @brief Top-level entry point for the hierarchical-CQ rewriter.
 *
 * Runs the shape gate, then the hierarchy detector.  When both accept,
 * applies the rewrite and returns the rewritten @c Query (the caller
 * is expected to re-enter @c process_query on the result).  Returns
 * @c NULL when @p q is outside the safe-query scope.
 *
 * @param constants  Cached extension OIDs (from @c get_constants).
 * @param q          Input @c Query, modified in place by side-effect-free
 *                   helpers but @em not consumed; the rewriter
 *                   @c copyObject's it before mutating.
 * @return A fresh rewritten @c Query, or @c NULL to fall through.
 */
extern Query *try_safe_query_rewrite(const constants_t *constants, Query *q);

/**
 * @brief Read-only test: is the conjunctive skeleton @c sk(Q) of an
 *        aggregate / @c HAVING query hierarchical (Dalvi-Suciu safe)?
 *
 * Reuses the hierarchical detector and the read-only normalisation
 * pre-passes of @c try_safe_query_rewrite on a deep copy whose head is
 * the group-by / aggregated variables and whose aggregate, @c HAVING
 * predicate and @c GROUP @c BY are stripped.  Performs no rewrite and
 * does not modify @p q.  Returns @c true only when a hierarchical
 * structure is positively certified (a single base relation is trivially
 * safe; disconnected multi-component skeletons and uninspectable shapes
 * are conservatively @c false).  This is the skeleton-safety axis of the
 * Ré-Suciu HAVING trichotomy, consumed by the HAVING classifier.
 *
 * @param constants  Cached extension OIDs (from @c get_constants).
 * @param orig       The aggregate / @c HAVING @c Query.  Not modified.
 * @return @c true iff @c sk(Q) is certified hierarchical.
 */
extern bool safe_query_skeleton_is_hierarchical(const constants_t *constants,
                                                Query *orig);

/**
 * @brief Per-atom marker spec for the inversion-free path.
 *
 * One entry per range-table atom (indexed by @c relid-1).  When @c valid, the
 * planner wraps that atom's provenance token in
 * @c annotate(prov, inversion_free_key(root_col, sec_col, factor)): a
 * per-input order key built from the tuple's root- and secondary-class column
 * values and its factor (@c SAFE_CERT_GUARD_FACTOR for the shared self-join
 * guard, else a per-factor id).
 */
typedef struct InvFreeMarker {
  bool       valid;
  AttrNumber root_col;   /* 1-based root-class column */
  AttrNumber sec_col;    /* 1-based secondary-class column */
  int        factor;     /* SAFE_CERT_GUARD_FACTOR, or a factor id */
} InvFreeMarker;

/**
 * @brief Per-query marker context for the inversion-free path, threaded through
 *        the recursive query rewrite to reach base inputs nested in subqueries.
 *
 * The detector runs on a *flattened* copy of the lineage query (single-base SPJ
 * subqueries / views inlined to their base relation in place, so range-table
 * positions are preserved).  The resulting per-atom markers are organised into
 * a context tree mirroring the *original* (un-flattened) query: for range-table
 * position @c i (0-based), @c markers[i] is the order marker for a base relation
 * sitting directly at @c i, while @c sub[i] is the child context for a flattened
 * subquery at @c i (whose own base atom carries the marker).  Exactly one of
 * @c markers[i].valid and @c sub[i]!=NULL holds for a certified atom.  A query
 * rewritten with a non-NULL context applies @c markers to its own base prov
 * Vars and passes @c sub[i] down when recursing into the subquery at @c i.
 */
typedef struct InvFreeMarkerCtx InvFreeMarkerCtx;
struct InvFreeMarkerCtx {
  int                natoms;    /* this query's range-table length */
  InvFreeMarker     *markers;   /* size natoms: direct base-atom markers */
  InvFreeMarkerCtx **sub;       /* size natoms: child ctx per flattened subquery, or NULL */
};

/**
 * @brief Inversion-free analysis of the lineage query @p q.
 *
 * Runs the detector on @p q itself (the query whose provenance lineage is being
 * built), so the certificate and the per-atom marker specs align with the
 * lineage by construction, independent of any read-once pre-pass.  On success
 * sets @p cert_out to a palloc'd @c C-prefixed serialised @c SafeCert recipe
 * (for the per-row root) and, when the marker model applies, @p markers_out to
 * a palloc'd array of @c *natoms_out @c InvFreeMarker (one per atom); otherwise
 * leaves them @c NULL.
 *
 * @return @c true if @p q is certified inversion-free (cert produced).
 */
extern bool inversion_free_analyze(const constants_t *constants, Query *q,
                                   char **cert_out, InvFreeMarker **markers_out,
                                   int *natoms_out);

/**
 * @brief PG 18 helper: strip the synthetic @c RTE_GROUP entry from
 *        @p q in place, resolving every grouped @c Var back to its
 *        base-table expression.  Defined in @c provsql.c.
 *
 * Idempotent: a no-op on earlier PostgreSQL versions and when
 * @c q->hasGroupRTE is already false.  The safe-query rewriter
 * applies it before the union-find pass so the range table is flat.
 */
extern void strip_group_rte_pg18(Query *q);

#endif /* SAFE_QUERY_H */
