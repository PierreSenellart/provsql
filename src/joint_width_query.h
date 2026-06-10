/**
 * @file joint_width_query.h
 * @brief Planner-time recognition of unsafe UCQs for the joint-width
 *        compiler.
 *
 * When @c provsql.provenance is @c 'boolean' and the safe-query rewriter
 * has declined (i.e. the query is an *unsafe* conjunctive query -- H0,
 * Hk, the @c \#P-hard cases the Dalvi-Suciu dichotomy rules out from
 * lifted inference), this recogniser extracts the UCQ structure from the
 * query's abstract syntax and builds the JSON descriptor consumed by
 * @c provsql.ucq_joint_provenance(): the relations, how their columns map
 * to query variables, and the atoms.  The descriptor drives the
 * joint-width compiler, whose certified d-D the standard evaluators
 * (@c probability_evaluate, @c shapley) then exploit -- so a recognised
 * query is answered exactly through the one evaluation pipeline.
 */
#ifndef JOINT_WIDTH_QUERY_H
#define JOINT_WIDTH_QUERY_H

#include "postgres.h"
#include "nodes/parsenodes.h"

#include "provsql_utils.h"

/**
 * @brief Build the joint-width descriptor for a recognised UCQ.
 *
 * Recognises a pure-join conjunctive query over provenance-tracked base
 * relations: a flat @c FROM list of @c RTE_RELATION entries (each
 * tracked), a @c WHERE that is a conjunction of @c Var @c = @c Var
 * equalities, no aggregates / @c GROUP @c BY / @c HAVING / sublinks.  The
 * variable structure is the column equivalence relation induced by the
 * equalities; every base column is a query variable (existential or
 * output).
 *
 * A @c UNION [@c ALL] of such conjunctive queries (the body of an
 * aggregated subquery) is recognised as a genuine multi-disjunct UCQ: one
 * disjunct per arm, relations merged across the arms, Boolean existence
 * only (per-answer UNION heads decline).
 *
 * @param constants        Cached extension OIDs.
 * @param q                The parsed query (read-only).
 * @param all_existential  Output: set to @c true iff no variable of the
 *                         UCQ is exposed in the target list (the query
 *                         computes the Boolean *existence* of the UCQ --
 *                         the @c \#P-hard case the substitution targets);
 *                         @c false when output/grouped variables remain
 *                         (a per-answer query).
 * @param head_var_idx     Output (may be @c NULL): when the query is
 *                         per-answer and every exposed target column is a
 *                         bare integer @c Var over a tracked atom, the
 *                         @c List of the head variables' query-variable
 *                         indices (@c Integer nodes), in output order,
 *                         deduplicated; @c NIL if the heads cannot be
 *                         cleanly extracted (the per-answer substitution
 *                         then declines).
 * @param head_exprs       Output (may be @c NULL): parallel @c List of the
 *                         target @c Var @c Expr exposing each head variable
 *                         (the per-group head value at execution).
 * @return A palloc'd JSON descriptor string (the shape
 *         @c ucq_joint_provenance expects), or @c NULL when @p q is
 *         outside the recogniser's scope.
 */
extern char *provsql_joint_width_descriptor(const constants_t *constants,
                                            Query *q, bool *all_existential,
                                            List **head_var_idx,
                                            List **head_exprs);

#endif /* JOINT_WIDTH_QUERY_H */
