/**
 * @file qual_classify.h
 * @brief Predicate-tree classification helpers shared by the query
 *        rewriters (the safe-query rewrite and the joint-width UCQ
 *        recogniser).
 *
 * These are pure, side-effect-free analyses of a @c WHERE / @c ON qual
 * tree of base-relation @c Var nodes: flatten an @c AND tree, recognise
 * @c Var=Var equijoins and @c Var=Const selections, collect the base Vars
 * / varnos a sub-tree touches, and split a conjunction into the
 * single-relation selections (pushable into a relation scan) and the
 * cross-relation residual (joins).  They encode the same notion of
 * "structure vs. pre-filter" both rewriters rely on: an equijoin
 * identifies variables (structure); a single-relation predicate is a
 * selection (a pre-filter); everything else is residual.
 *
 * They carry no safety-specific logic, so both the safe-query and
 * joint-width rewriters share them.
 */
#ifndef QUAL_CLASSIFY_H
#define QUAL_CLASSIFY_H

#include "postgres.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/bitmapset.h"

/** @brief Walker context for @c qc_collect_vars_walker. */
typedef struct qc_vars_ctx {
  List *vars;  ///< Deduplicated list of distinct base-level Var nodes
} qc_vars_ctx;

/** @brief Walker context for @c qc_collect_varnos_walker. */
typedef struct qc_varnos_ctx {
  Bitmapset *varnos;  ///< Set of @c varno values seen in base-level Vars
} qc_varnos_ctx;

/**
 * @brief Tree walker that collects every distinct base-level Var node
 *        (@c varlevelsup == 0), deduplicated by @c (varno, varattno).
 */
extern bool qc_collect_vars_walker(Node *node, qc_vars_ctx *ctx);

/**
 * @brief Position of a Var inside @p vars (matched on @c (varno,
 *        varattno)); -1 if absent.
 */
extern int qc_var_index(List *vars, Index varno, AttrNumber varattno);

/**
 * @brief Recognise a conjunct equating two base @c Vars (the canonical
 *        equality for the operand types, through @c RelabelType casts).
 *        Fills @p *l, @p *r on match.
 */
extern bool qc_is_var_eq(Expr *qual, Var **l, Var **r);

/**
 * @brief Recognise a conjunct of shape @c Var=Const (either order, through
 *        @c RelabelType casts; non-NULL literal, canonical equality).
 *        Fills @p *var, @p *konst on match.
 */
extern bool qc_is_var_const_eq(Expr *qual, Var **var, Const **konst);

/**
 * @brief Walk @p quals as an @c AND tree, appending each @c Var=Var
 *        equijoin's two Vars (left, right) to @p *out.  OR / NOT are not
 *        traversed (they are not equijoins).
 */
extern void qc_collect_equalities(Node *quals, List **out);

/**
 * @brief Collect the distinct base-level @c varno values referenced by a
 *        sub-tree (used to tell a single-relation selection from a
 *        cross-relation predicate).
 */
extern bool qc_collect_varnos_walker(Node *node, qc_varnos_ctx *ctx);

/**
 * @brief Flatten the top-level @c AND tree of a qual into a flat list of
 *        leaf conjuncts (a bare @c List is an implicit AND).  The result
 *        shares pointers with the input; @c copyObject before mutating.
 */
extern void qc_flatten_and(Node *n, List **out);

/**
 * @brief Partition top-level conjuncts into atom-local selections and the
 *        cross-atom residual.
 *
 * @p per_atom_out is a caller-allocated, zero-initialised array of length
 * @p natoms; a conjunct whose base Vars all reference a single @c varno
 * (1..natoms) and that is non-volatile lands in that atom's list, the
 * rest in @p *out_residual (rebuilt as @c NULL / the lone conjunct / a
 * fresh @c AND).  Volatile predicates stay in the residual (the inner
 * @c DISTINCT must not change their evaluation count).
 */
extern void qc_split_quals(Node *quals, int natoms,
                           List **per_atom_out, Node **out_residual);

#endif /* QUAL_CLASSIFY_H */
