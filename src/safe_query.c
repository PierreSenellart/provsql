/**
 * @file safe_query.c
 * @brief Hierarchical-CQ rewriter for the @c provsql.boolean_provenance GUC.
 *
 * Opt-in pre-pass invoked by @c process_query in @c provsql.c.  Rewrites
 * SELECT-FROM-WHERE conjunctive queries with a hierarchical structure
 * (every shared variable's atom-set is either fully covered or fits
 * into a multi-level inner-group decomposition) into a form whose
 * provenance circuit is read-once.  The result lets the linear-time
 * @c BooleanCircuit::independentEvaluation method handle queries that
 * would otherwise fall through to the dDNNF / tree-decomposition /
 * external-knowledge-compiler pipeline.
 *
 * Entry point: @c try_safe_query_rewrite (see @c safe_query.h).
 *
 * The bulk of the file is detector + rewriter helpers.  All non-API
 * symbols are @c static.
 */
#include "postgres.h"
#include "fmgr.h"
#include "pg_config.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#if PG_VERSION_NUM >= 120000
#include "optimizer/optimizer.h"
#else
#include "optimizer/clauses.h"          /* contain_volatile_functions */
#endif
#include "parser/parse_oper.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "compatibility.h"
#include "provsql_mmap.h"
#include "provsql_utils.h"
#include "safe_query.h"

extern int provsql_verbose;             /* declared in provsql.c */

/* -------------------------------------------------------------------------
 * Safe-query optimisation (provsql.boolean_provenance)
 *
 * Slot for the hierarchical-CQ rewriter.  When the GUC
 * provsql.boolean_provenance is on, the planner-hook calls
 * try_safe_query_rewrite() between the AGG-DISTINCT rewrite and
 * get_provenance_attributes; if it returns a non-NULL Query, that
 * Query is fed back into process_query() from the top, exactly the
 * same recursion pattern as rewrite_agg_distinct().
 *
 * The first pass (is_safe_query_candidate) is a cheap shape /
 * metadata gate; if it accepts, the second pass
 * (find_hierarchical_root_atoms) builds the variable-equivalence
 * relation and decides whether the query has a root variable.  When
 * both accept, rewrite_hierarchical_cq emits the wrapped Query.
 * ------------------------------------------------------------------------- */

/**
 * @brief Walk a Query and reject anything outside the safe-query scope.
 *
 * Accepts only:
 *  - self-join-free conjunctive queries
 *  - no aggregation, window functions, DISTINCT ON, LIMIT/OFFSET,
 *    set operations (UCQ -- TODO), sublinks
 *  - an outer @c GROUP @c BY or top-level @c DISTINCT.  Without one,
 *    the per-atom @c SELECT @c DISTINCT wraps would shrink the user-
 *    visible row count, so the rewrite would change the result set.
 *  - all base relations have a provenance metadata entry, none are
 *    OPAQUE.  BID atom block-key validation is deferred to the
 *    rewriter (we cannot check it without knowing the root variable).
 *
 * @return @c true iff @p q is a candidate for the safe-query rewrite.
 */
static bool is_safe_query_candidate(const constants_t *constants, Query *q) {
  ListCell *lc, *lc2;
  List *seen_relids = NIL;

  if (q->setOperations != NULL)
    return false;                       /* TODO: top-level UCQ */
  if (q->hasAggs || q->hasWindowFuncs)
    return false;
  if (q->limitCount != NULL || q->limitOffset != NULL)
    return false;
  if (q->groupingSets != NIL)
    return false;
  if (q->hasDistinctOn)
    return false;
  if (q->hasSubLinks)
    return false;
  if (q->rtable == NIL)
    return false;                       /* FROM-less; nothing to rewrite */
  /* The per-atom @c SELECT @c DISTINCT wraps collapse duplicate
   * source tuples on their projection slots; without an outer
   * @c GROUP @c BY or top-level @c DISTINCT the user would observe a
   * shrunken row count compared to the unrewritten query.  Require
   * one of them so the rewrite is row-count-preserving in the user's
   * eye.  Both are encoded as @c SortGroupClause lists; either is
   * enough -- @c transform_distinct_into_group_by promotes the
   * outer @c DISTINCT to a @c GROUP @c BY downstream of us. */
  if (q->groupClause == NIL && q->distinctClause == NIL)
    return false;

  /* All FROM entries must be base relations referenced via plain
   * RangeTblRef (no JoinExpr, no RTE_SUBQUERY / RTE_VALUES / ...).
   * The fromlist check ensures we are looking at a flat join. */
  foreach (lc, q->jointree->fromlist) {
    Node *n = (Node *) lfirst(lc);
    if (!IsA(n, RangeTblRef))
      return false;
  }

  foreach (lc, q->rtable) {
    RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
    ProvenanceTableInfo info;

    if (rte->rtekind != RTE_RELATION)
      return false;
    /* Self-join-free: no two RTEs may share a relid. */
    foreach (lc2, seen_relids) {
      if (lfirst_oid(lc2) == rte->relid)
        return false;
    }
    seen_relids = lappend_oid(seen_relids, rte->relid);

    /* Metadata gate.  Tracked relations must not be OPAQUE (unknown
     * correlations).  Non-tracked relations are accepted: they
     * contribute deterministic, probability-1 tuples, which behave
     * as if every row carried a @c gate_one() leaf -- read-once
     * factoring is unaffected.  TID and BID tracked relations are
     * both fine; the BID block-key alignment check happens in the
     * rewriter once the separator is known. */
    if (provsql_lookup_table_info(rte->relid, &info)
        && info.kind == PROVSQL_TABLE_OPAQUE)
      return false;
  }

  list_free(seen_relids);
  return true;
}

/**
 * @brief One projected column of an atom's wrapping subquery.
 *
 * @c base_attno is the column of the base relation that supplies this
 * slot.  @c class_id is the variable-equivalence-class representative
 * index from the union-find; only shared classes (those touching at
 * least two atoms) ever appear as slots, of which the root class is
 * one.
 *
 * The output column number of the slot inside the inner @c SELECT
 * @c DISTINCT is the 1-based position of the slot in its atom's
 * @c proj_slots list; the root-class slot is always first, so its
 * output attno is 1.
 */
typedef struct safe_proj_slot {
  AttrNumber  base_attno;
  int         class_id;
  AttrNumber  outer_attno;  ///< 1-based column in the inner sub-Query's targetList (or per-atom DISTINCT wrap for outer-wrap atoms).  Matches the slot's position in the atom's proj_slots for outer-wrap atoms, for first-member grouped atoms, and for shared slots on non-first-member grouped atoms.  Differs for singleton head Vars on non-first-members: those get the next position in the group's unified inner targetList after all earlier members' slots.
} safe_proj_slot;

/**
 * @brief Per-atom rewrite metadata discovered by the hierarchy detector.
 *
 * @c rtindex is the 1-based index into @c q->rtable that matches @c Var.varno.
 * @c proj_slots is the ordered list of @c safe_proj_slot * to project
 * out of this atom's inner @c SELECT @c DISTINCT.  The root-class slot
 * is always first.  Additional shared classes touching this atom
 * (column pushdown) follow in ascending class-repr order.
 *
 * @c pushed_quals is the list of WHERE conjuncts that reference only
 * this atom (single-atom Vars only) and were extracted from the outer
 * query before the hierarchy analysis ran.  They are AND-injected into
 * the inner subquery's WHERE after a @c varno remap from
 * @c rtindex to @c 1, so the atom-local predicates evaluate before the
 * @c DISTINCT and the offending single-atom Vars never reach the outer
 * scope.
 */
typedef struct safe_rewrite_atom {
  Index       rtindex;
  List       *proj_slots;
  List       *pushed_quals;
  int         group_id;        ///< -1 for atoms wrapped directly at the outer (one @c SELECT @c DISTINCT subquery per atom); >= 0 indexes into the rewrite's groups list and means the atom is a member of an inner sub-Query built around a partial-coverage shared class.
  Index       outer_rtindex;   ///< Assigned by the rewriter: this atom's slot in the rebuilt outer rtable.  Grouped atoms all share their group's outer_rtindex.
  Index       inner_rtindex;   ///< Assigned by the rewriter for grouped atoms only: position inside the inner sub-Query's rtable (1-based).  0 for outer-wrap atoms.
  AttrNumber  root_anchor_attno; ///< For grouped atoms: base @c attno of the root-class binding column inside this atom.  Used by the outer Var remap to recognise root-class references that should resolve to the inner sub-Query's single output column.
} safe_rewrite_atom;

/**
 * @brief Descriptor for an inner sub-Query introduced when one or more
 *        shared classes have partial coverage.
 *
 * Every member atom shares the same partial-coverage set; the group is
 * folded into a single @c RTE_SUBQUERY at the outer level whose
 * @c targetList is the fully-covered-class bindings (root first, then
 * other fully-covered classes in ascending repr order) plus the
 * implicit @c provsql column, and whose @c groupClause aggregates the
 * partial-coverage variables away.  The hierarchical-CQ rewriter fires
 * again when @c process_query re-enters on the inner sub-Query, so the
 * per-atom @c SELECT @c DISTINCT wraps materialise inside.
 */
typedef struct safe_inner_group {
  int         group_id;
  List       *member_atoms;        ///< List of safe_rewrite_atom *, in original-rtindex order
  List       *inner_quals;         ///< List of Node *: cross-atom conjuncts whose vars all reference group members (original varnos; the rewriter remaps to inner varnos at build time)
  Index       outer_rtindex;       ///< Assigned by the rewriter: position of the inner sub-Query RTE in the outer rtable
} safe_inner_group;

/** @brief Walker context for @c safe_collect_vars_walker. */
typedef struct safe_collect_vars_ctx {
  List *vars;  ///< Deduplicated list of distinct base-level Var nodes
} safe_collect_vars_ctx;

/**
 * @brief Tree walker that collects every distinct base-level Var node.
 *
 * "Base level" means @c varlevelsup == 0; outer references are ignored.
 * Vars are deduplicated by @c (varno, varattno).
 */
static bool safe_collect_vars_walker(Node *node, safe_collect_vars_ctx *ctx) {
  ListCell *lc;
  if (node == NULL)
    return false;
  if (IsA(node, Var)) {
    Var *v = (Var *) node;
    if (v->varlevelsup != 0)
      return false;
    foreach (lc, ctx->vars) {
      Var *existing = (Var *) lfirst(lc);
      if (existing->varno == v->varno && existing->varattno == v->varattno)
        return false;
    }
    ctx->vars = lappend(ctx->vars, v);
    return false;
  }
  return expression_tree_walker(node, safe_collect_vars_walker, ctx);
}

/**
 * @brief Find the position of a Var inside the @c vars list (matched on
 *        @c (varno, varattno)).  Returns -1 if not present.
 */
static int safe_var_index(List *vars, Index varno, AttrNumber varattno) {
  ListCell *lc;
  int i = 0;
  foreach (lc, vars) {
    Var *v = (Var *) lfirst(lc);
    if (v->varno == varno && v->varattno == varattno)
      return i;
    i++;
  }
  return -1;
}

/**
 * @brief Recognise a top-level WHERE conjunct that equates two base Vars.
 *
 * Matches @c OpExpr nodes whose operator is the canonical equality for
 * the two operand types and whose operands strip down to base-level
 * @c Var nodes (possibly through @c RelabelType wrappers, which carry
 * binary-coercion casts).  Returns @c true and fills @p *l, @p *r when
 * matched.
 */
static bool safe_is_var_equality(Expr *qual, Var **l, Var **r) {
  OpExpr *op;
  Node   *ln, *rn;
  Var    *lv, *rv;
  Oid     expected;

  if (!IsA(qual, OpExpr))
    return false;
  op = (OpExpr *) qual;
  if (list_length(op->args) != 2)
    return false;
  ln = (Node *) linitial(op->args);
  rn = (Node *) lsecond(op->args);
  while (IsA(ln, RelabelType))
    ln = (Node *) ((RelabelType *) ln)->arg;
  while (IsA(rn, RelabelType))
    rn = (Node *) ((RelabelType *) rn)->arg;
  if (!IsA(ln, Var) || !IsA(rn, Var))
    return false;
  lv = (Var *) ln;
  rv = (Var *) rn;
  if (lv->varlevelsup != 0 || rv->varlevelsup != 0)
    return false;
  expected = find_equality_operator(lv->vartype, rv->vartype);
  if (expected == InvalidOid || op->opno != expected)
    return false;
  *l = lv;
  *r = rv;
  return true;
}

/**
 * @brief Walk @p quals as a tree of @c AND, collecting equality conjuncts.
 *
 * Returns the matched @c (Var *, Var *) pairs as a flat list, with each
 * pair contributed as two consecutive list entries (left, right).
 * Non-equality conjuncts are silently ignored (they do not contribute
 * to the variable equivalence relation but do not prevent the rewrite
 * either; the detector will fall back to bail if no root variable is
 * found).  OR / NOT are not traversed -- they are not equijoins, and
 * accepting them here would be unsound.
 */
static void safe_collect_equalities(Node *quals, List **out) {
  if (quals == NULL)
    return;
  if (IsA(quals, List)) {
    ListCell *lc;
    foreach (lc, (List *) quals)
      safe_collect_equalities((Node *) lfirst(lc), out);
    return;
  }
  if (IsA(quals, BoolExpr)) {
    BoolExpr *be = (BoolExpr *) quals;
    if (be->boolop == AND_EXPR) {
      ListCell *lc;
      foreach (lc, be->args)
        safe_collect_equalities((Node *) lfirst(lc), out);
    }
    return;
  }
  {
    Var *l, *r;
    if (safe_is_var_equality((Expr *) quals, &l, &r)) {
      *out = lappend(*out, l);
      *out = lappend(*out, r);
    }
  }
}

/** @brief Walker context for @c safe_collect_varnos_walker. */
typedef struct safe_collect_varnos_ctx {
  Bitmapset *varnos;  ///< Set of @c varno values seen in base-level Vars
} safe_collect_varnos_ctx;

/**
 * @brief Collect the distinct base-level @c varno values referenced by an
 *        expression sub-tree.
 *
 * Used by @c safe_split_quals to decide whether a WHERE conjunct is
 * "atom-local" (single varno ⇒ pushable) or "cross-atom" (≥ 2 varnos
 * ⇒ stays in the residual).  Outer Vars (@c varlevelsup > 0) are
 * ignored; the safe-query candidate gate has already rejected
 * sublinks, so they cannot legitimately appear here.
 */
static bool safe_collect_varnos_walker(Node *node,
                                       safe_collect_varnos_ctx *ctx) {
  if (node == NULL)
    return false;
  if (IsA(node, Var)) {
    Var *v = (Var *) node;
    if (v->varlevelsup == 0 && (int) v->varno >= 1)
      ctx->varnos = bms_add_member(ctx->varnos, (int) v->varno);
    return false;
  }
  return expression_tree_walker(node, safe_collect_varnos_walker, ctx);
}

/**
 * @brief Flatten the top-level @c AND tree of a WHERE clause into a flat
 *        list of conjunct @c Node *.
 *
 * Mirrors @c safe_collect_equalities' AND-tree recursion: a top-level
 * @c List is treated as an implicit AND (as @c FromExpr->quals can be),
 * a @c BoolExpr with @c AND_EXPR has its @c args recursed into, and any
 * other node is treated as a single leaf conjunct (including OR /
 * NOT / NULL-tests / equalities / arbitrary @c OpExpr).  The resulting
 * list shares pointers with the original tree; callers must
 * @c copyObject before mutating.
 */
static void safe_flatten_and(Node *n, List **out) {
  if (n == NULL)
    return;
  if (IsA(n, List)) {
    ListCell *lc;
    foreach (lc, (List *) n)
      safe_flatten_and((Node *) lfirst(lc), out);
    return;
  }
  if (IsA(n, BoolExpr) && ((BoolExpr *) n)->boolop == AND_EXPR) {
    ListCell *lc;
    foreach (lc, ((BoolExpr *) n)->args)
      safe_flatten_and((Node *) lfirst(lc), out);
    return;
  }
  *out = lappend(*out, n);
}

/**
 * @brief Partition top-level WHERE conjuncts into atom-local quals
 *        (pushed into the matching atom's inner wrap) and the residual
 *        cross-atom quals (which stay in the outer query).
 *
 * @p per_atom_out is an array of length @p natoms (caller-allocated,
 * zero-initialised) of @c List *.  @p out_residual receives the
 * residual conjunction, rebuilt as a single @c Node *: @c NULL when no
 * residual conjuncts remain, the bare conjunct when exactly one, or a
 * fresh @c BoolExpr(@c AND_EXPR) otherwise.
 *
 * A conjunct is pushed when its base-level Vars all reference the same
 * @c varno and the conjunct contains no volatile function calls.
 * Volatile predicates are kept in the outer scope because the inner
 * @c SELECT @c DISTINCT can collapse the evaluation count -- we
 * deliberately avoid changing the number of times such functions run.
 *
 * The pushed conjuncts are still shared with the original
 * @c q->jointree->quals tree; @c safe_build_inner_wrap @c copyObject's
 * them before performing the @c varno remap.
 */
static void safe_split_quals(Node *quals, int natoms,
                             List **per_atom_out, Node **out_residual) {
  List     *conjuncts = NIL;
  List     *residual  = NIL;
  ListCell *lc;
  int       i;

  for (i = 0; i < natoms; i++)
    per_atom_out[i] = NIL;

  safe_flatten_and(quals, &conjuncts);

  foreach (lc, conjuncts) {
    Node *qual = (Node *) lfirst(lc);
    safe_collect_varnos_ctx vctx = { NULL };
    int nmembers;

    if (contain_volatile_functions(qual)) {
      residual = lappend(residual, qual);
      continue;
    }

    safe_collect_varnos_walker(qual, &vctx);
    nmembers = bms_num_members(vctx.varnos);
    if (nmembers == 1) {
      int v = bms_singleton_member(vctx.varnos);
      if (v >= 1 && v <= natoms) {
        per_atom_out[v - 1] = lappend(per_atom_out[v - 1], qual);
        bms_free(vctx.varnos);
        continue;
      }
    }
    bms_free(vctx.varnos);
    residual = lappend(residual, qual);
  }

  if (residual == NIL)
    *out_residual = NULL;
  else if (list_length(residual) == 1)
    *out_residual = (Node *) linitial(residual);
  else
    *out_residual = (Node *) makeBoolExpr(AND_EXPR, residual, -1);
}

/**
 * @brief Partition the cross-atom residual into per-group conjuncts and a
 *        new outer residual.
 *
 * For every top-level @c AND conjunct of @p residual:
 *
 *  - if every base-level @c Var it references points at an atom in some
 *    inner group's member set, the conjunct moves into that group's
 *    @c inner_quals (in original-varno space; the rewriter remaps to
 *    inner varnos when it builds the sub-Query);
 *
 *  - otherwise the conjunct stays in the rebuilt outer residual.
 *
 * Volatile conjuncts always stay in the outer residual: collapsing the
 * row count inside a sub-Query with an aggregating @c GROUP @c BY would
 * change how many times the volatile function runs.
 */
static void safe_partition_residual(Node *residual, List *atoms, List *groups,
                                    Node **outer_residual_out) {
  List     *conjuncts = NIL;
  List     *outer_residual = NIL;
  ListCell *lc;

  if (residual == NULL) {
    *outer_residual_out = NULL;
    return;
  }

  safe_flatten_and(residual, &conjuncts);

  foreach (lc, conjuncts) {
    Node *qual = (Node *) lfirst(lc);
    safe_collect_varnos_ctx vctx = { NULL };
    int v;
    int target_group = -1;
    bool stays_outer = false;

    if (contain_volatile_functions(qual)) {
      outer_residual = lappend(outer_residual, qual);
      continue;
    }

    safe_collect_varnos_walker(qual, &vctx);
    v = -1;
    while ((v = bms_next_member(vctx.varnos, v)) >= 0) {
      int g;
      if (v < 1 || v > list_length(atoms)) {
        stays_outer = true;
        break;
      }
      g = ((safe_rewrite_atom *) list_nth(atoms, v - 1))->group_id;
      if (g < 0) {
        stays_outer = true;
        break;
      }
      if (target_group < 0)
        target_group = g;
      else if (target_group != g) {
        stays_outer = true;
        break;
      }
    }
    bms_free(vctx.varnos);

    if (stays_outer || target_group < 0)
      outer_residual = lappend(outer_residual, qual);
    else {
      safe_inner_group *gr =
          (safe_inner_group *) list_nth(groups, target_group);
      gr->inner_quals = lappend(gr->inner_quals, qual);
    }
  }

  if (outer_residual == NIL)
    *outer_residual_out = NULL;
  else if (list_length(outer_residual) == 1)
    *outer_residual_out = (Node *) linitial(outer_residual);
  else
    *outer_residual_out = (Node *) makeBoolExpr(AND_EXPR, outer_residual, -1);
}

/** @brief Mutator context for @c safe_pushed_remap_mutator. */
typedef struct safe_pushed_remap_ctx {
  Index outer_rtindex;  ///< varno in the outer scope to rewrite to 1
} safe_pushed_remap_ctx;

/**
 * @brief Rewrite @c Var.varno from the outer atom rtindex to @c 1, the
 *        sole RTE of the inner wrap subquery.
 *
 * Applied to each pushed conjunct before it is AND-injected into the
 * inner @c Query's @c jointree->quals.  @c varattno is preserved
 * (the inner subquery's RTE is a fresh clone of the same base
 * relation).
 */
static Node *safe_pushed_remap_mutator(Node *node,
                                       safe_pushed_remap_ctx *ctx) {
  if (node == NULL)
    return NULL;
  if (IsA(node, Var)) {
    Var *v = (Var *) node;
    if (v->varlevelsup == 0 && v->varno == ctx->outer_rtindex) {
      Var *nv = (Var *) copyObject(v);
      nv->varno = 1;
      return (Node *) nv;
    }
    return node;
  }
  return expression_tree_mutator(node, safe_pushed_remap_mutator, (void *) ctx);
}

/**
 * @brief Run the hierarchy detector on @p q, returning per-atom rewrite info.
 *
 * Builds the variable equivalence relation induced by WHERE-clause
 * @c Var = @c Var equalities, identifies a "root variable" (a class
 * whose member Vars touch every base RTE), and decides how each
 * remaining shared class is materialised.  Fully-covered non-root
 * classes become extra projection slots in the per-atom inner wrap.
 * Partial-coverage shared classes -- those touching at least two but
 * not all atoms -- trigger the multi-level path: the affected atoms
 * are bundled into an inner sub-Query whose @c GROUP @c BY folds the
 * partial-coverage variables before the outer join with the remaining
 * atoms.  Returns a list of @c safe_rewrite_atom * (one per
 * @c q->rtable entry, in @c rtable order) plus, via @p groups_out,
 * the list of @c safe_inner_group * the rewriter must build.
 *
 * Returns @c NIL when the query is not in the currently-supported
 * shape:
 *
 *  - fewer than two atoms (single-relation query needs no rewrite);
 *  - no root variable (multi-component CQ);
 *  - an atom whose root binding spans more than one column (the
 *    rewrite would have to push an intra-atom equality into the
 *    inner subquery -- TODO);
 *  - at least one partial-coverage class exists but every atom is
 *    touched by some partial-coverage class.  Bundling all atoms
 *    into one inner group would re-enter the rewriter with the same
 *    shape, so we defer disjoint-partial cases such as
 *    @c A(x,y),B(x,y),C(x,z),D(x,z) where atoms(y) and atoms(z) are
 *    disjoint;
 *  - any Var in @c q->targetList or the residual @p quals that
 *    belongs to a single-atom class (head-only projection is still
 *    out of scope; atom-local WHERE conjuncts are handled upstream
 *    by @c safe_split_quals and never reach this function).
 *
 * The shape gate in @c is_safe_query_candidate has already enforced
 * self-join-free, no aggs / windows / sublinks etc.; this function
 * only adds the hierarchy-specific checks.
 *
 * @param constants   Cached extension OIDs (unused here; reserved for
 *                    future class/type lookups).
 * @param q           Input Query; the detector only @em reads it.
 * @param quals       Residual WHERE quals (post-split): the cross-atom
 *                    conjunction that the union-find must reason
 *                    about.  Single-atom conjuncts have been
 *                    extracted upstream and stored separately per
 *                    atom.
 * @param groups_out  Out: list of @c safe_inner_group * produced when
 *                    the partial-coverage path fires; @c NIL when the
 *                    rewriter only needs single-level outer wraps.
 */
static List *find_hierarchical_root_atoms(const constants_t *constants,
                                          Query *q, Node *quals,
                                          List **groups_out) {
  safe_collect_vars_ctx vctx = { NIL };
  List   *eq_pairs = NIL;
  ListCell *lc;
  Var   **vars_arr;
  int    *cls;
  int     nvars;
  int     natoms = list_length(q->rtable);
  int    *class_atom_count;
  int    *class_atom_anchor_attno;  /* per atom, per class: any one attno */
  int     root_class = -1;
  List   *atoms_out = NIL;
  int    *atom_group = NULL;        /* per-atom group id: -1 = outer-wrap; 0 = inner */
  bool   *in_targetlist = NULL;     /* per-var: appears somewhere in q->targetList */
  int    *first_member_of_group = NULL; /* per-group: smallest atom index in the group */
  int    *group_singleton_counter = NULL; /* per-group: running outer_attno counter for singleton head Vars on non-first-members */
  bool    have_partial_class = false;
  int     partial_first = -1;       /* repr of the first partial-coverage class seen */
  int     i, j;
  Index   varno;
  bool    ok;

  *groups_out = NIL;

  if (natoms < 2)
    return NIL;

  /* Collect every distinct base-level Var occurring anywhere in the
   * residual query (target list and the residual WHERE quals,
   * i.e. cross-atom conjuncts that survive @c safe_split_quals).
   * Each becomes a node in the union-find. */
  expression_tree_walker((Node *) q->targetList,
                         safe_collect_vars_walker, &vctx);
  if (quals)
    expression_tree_walker(quals, safe_collect_vars_walker, &vctx);

  nvars = list_length(vctx.vars);
  if (nvars == 0)
    return NIL;

  vars_arr = palloc(nvars * sizeof(Var *));
  cls = palloc(nvars * sizeof(int));
  i = 0;
  foreach (lc, vctx.vars) {
    vars_arr[i] = (Var *) lfirst(lc);
    cls[i] = i;
    i++;
  }

  /* Union equality-related Vars.  We walk only the residual quals
   * (atom-local conjuncts were already split off); a top-level @c AND
   * is decomposed conjunct-by-conjunct, but @c OR / @c NOT subtrees
   * are never traversed because they would weaken, not strengthen,
   * the equivalence relation. */
  if (quals)
    safe_collect_equalities(quals, &eq_pairs);

  for (lc = list_head(eq_pairs); lc != NULL; lc = my_lnext(eq_pairs, lc)) {
    Var *lv, *rv;
    int  li, ri, ci, cj, k;
    lv = (Var *) lfirst(lc);
    lc = my_lnext(eq_pairs, lc);
    rv = (Var *) lfirst(lc);
    li = safe_var_index(vctx.vars, lv->varno, lv->varattno);
    ri = safe_var_index(vctx.vars, rv->varno, rv->varattno);
    if (li < 0 || ri < 0)
      continue;
    ci = cls[li];
    cj = cls[ri];
    if (ci == cj)
      continue;
    for (k = 0; k < nvars; k++)
      if (cls[k] == cj)
        cls[k] = ci;
  }

  /* For each class, count how many distinct atoms (varno values) it
   * touches.  A class touching all `natoms` is a root variable. */
  class_atom_count = palloc0(nvars * sizeof(int));
  class_atom_anchor_attno =
      palloc0((size_t) nvars * (size_t) natoms * sizeof(int));

#define ANCHOR(c, atom_idx) class_atom_anchor_attno[(c) * natoms + (atom_idx)]

  for (i = 0; i < nvars; i++) {
    int c = cls[i];
    int atom_idx;
    varno = vars_arr[i]->varno;
    if (varno < 1 || (int) varno > natoms)
      continue;                       /* shouldn't happen */
    atom_idx = (int) varno - 1;
    if (ANCHOR(c, atom_idx) == 0) {
      class_atom_count[c]++;
      ANCHOR(c, atom_idx) = vars_arr[i]->varattno;
    } else if (ANCHOR(c, atom_idx) != vars_arr[i]->varattno) {
      /* Same class binds two columns of the same atom: the current
       * rewriter does not push the implied intra-atom equality into
       * the inner subquery, so mark this class unusable.  Count >
       * natoms is impossible otherwise, so we use this as a sentinel. */
      class_atom_count[c] = natoms + 1;
    }
  }

  /* Single-atom head Vars: walk @c q->targetList once to mark every
   * @c vars_arr index that appears in the user's projection.  Used
   * below to allow body-only Vars (singleton classes, @c count == 1)
   * to reach the outer scope as an extra @c proj_slot on their atom's
   * wrap. */
  in_targetlist = palloc0(nvars * sizeof(bool));
  {
    safe_collect_vars_ctx tlist_ctx = { NIL };
    ListCell *tlc;
    expression_tree_walker((Node *) q->targetList,
                           safe_collect_vars_walker, &tlist_ctx);
    foreach (tlc, tlist_ctx.vars) {
      Var *v = (Var *) lfirst(tlc);
      int idx = safe_var_index(vctx.vars, v->varno, v->varattno);
      if (idx >= 0)
        in_targetlist[idx] = true;
    }
  }

  /* Root class: a class touching every atom (count == natoms).
   * Pick the lowest repr index when multiple candidates exist, for
   * deterministic rewriter output. */
  for (i = 0; i < nvars; i++) {
    if (cls[i] != i)
      continue;
    if (class_atom_count[i] == natoms) {
      root_class = i;
      break;
    }
  }
  if (root_class < 0)
    goto bail;

  /* Multi-level handling: any atom touched by at least one partial-
   * coverage shared class (count >= 2 but < natoms) goes into some
   * inner sub-Query.  Two grouping strategies, decided per-query:
   *
   *  - @em One @em big @em inner @em group: when at least one atom
   *    has @em empty partial-coverage signature (no partial-coverage
   *    class touches it), bundle every atom with non-empty signature
   *    into one inner sub-Query and let the recursive call (via
   *    @c process_query / Choice A) peel further partial-coverage
   *    classes inside.  The empty-signature atoms become outer
   *    wraps; the recursion is guaranteed to make progress at each
   *    level.
   *
   *  - @em Disjoint @em multi-group: when every atom carries a non-
   *    empty signature, the one-big-group approach would re-enter the
   *    same shape, so we partition atoms by their @em exact
   *    signature and build one inner sub-Query per distinct
   *    signature.  This only works when partial-coverage classes are
   *    cleanly partitioned: every class @c c must touch atoms that
   *    all share the same signature.  Otherwise @c c "bridges"
   *    multiple groups and the outer would need an extra join column;
   *    we defer that case.
   */
  atom_group = palloc(natoms * sizeof(int));
  for (j = 0; j < natoms; j++)
    atom_group[j] = -1;

  {
    Bitmapset **sig = palloc0(natoms * sizeof(Bitmapset *));
    bool has_outer_atom = true;

    for (i = 0; i < nvars; i++) {
      int c = cls[i];
      if (c != i)
        continue;
      if (class_atom_count[c] < 2)
        continue;                       /* single-atom class checked below */
      if (class_atom_count[c] > natoms)
        continue;                       /* sentinel; handled below per-Var */
      if (class_atom_count[c] == natoms)
        continue;                       /* fully-covered: extra outer slot */
      have_partial_class = true;
      if (partial_first < 0)
        partial_first = c;
      for (j = 0; j < natoms; j++) {
        if (ANCHOR(c, j) != 0)
          sig[j] = bms_add_member(sig[j], c);
      }
    }

    if (have_partial_class) {
      has_outer_atom = false;
      for (j = 0; j < natoms; j++) {
        if (bms_is_empty(sig[j])) {
          has_outer_atom = true;
          break;
        }
      }
    }

    if (have_partial_class && has_outer_atom) {
      /* One-big-inner-group: atoms with any partial-coverage class go
       * into group 0; empty-signature atoms stay as outer wraps. */
      for (j = 0; j < natoms; j++)
        atom_group[j] = bms_is_empty(sig[j]) ? -1 : 0;
    } else if (have_partial_class) {
      /* Disjoint multi-group: bridge check + distinct-signature
       * partitioning.  A partial-coverage class @c c bridges if the
       * atoms it touches do not all share the same signature; that
       * happens whenever a class spans more than one group, and the
       * outer would need an extra join column. */
      bool bridge_exists = false;
      Bitmapset **group_sigs = NULL;
      int ngroups = 0;
      for (i = 0; i < nvars && !bridge_exists; i++) {
        Bitmapset *first_sig = NULL;
        int c = cls[i];
        if (c != i)
          continue;
        if (class_atom_count[c] < 2 || class_atom_count[c] >= natoms)
          continue;
        for (j = 0; j < natoms; j++) {
          if (ANCHOR(c, j) == 0)
            continue;
          if (first_sig == NULL)
            first_sig = sig[j];
          else if (!bms_equal(first_sig, sig[j])) {
            bridge_exists = true;
            break;
          }
        }
      }
      if (bridge_exists) {
        if (provsql_verbose >= 30)
          provsql_notice("safe-query rewriter: a partial-coverage shared "
                         "class spans atoms with different signatures "
                         "(bridge case) -- multi-group rewrite with bridge "
                         "join columns is deferred");
        for (j = 0; j < natoms; j++) bms_free(sig[j]);
        pfree(sig);
        goto bail;
      }
      /* Assign group_id by signature equality, in order of first
       * appearance to keep the rewriter output deterministic. */
      group_sigs = palloc0(natoms * sizeof(Bitmapset *));
      for (j = 0; j < natoms; j++) {
        bool found = false;
        int g;
        for (g = 0; g < ngroups; g++) {
          if (bms_equal(group_sigs[g], sig[j])) {
            atom_group[j] = g;
            found = true;
            break;
          }
        }
        if (!found) {
          atom_group[j] = ngroups;
          group_sigs[ngroups] = sig[j];
          ngroups++;
        }
      }
      pfree(group_sigs);
    }

    for (j = 0; j < natoms; j++) bms_free(sig[j]);
    pfree(sig);
  }

  /* For each group, identify the @em first member (smallest
   * original-rtindex atom belonging to the group).  Head Vars on
   * grouped atoms are only allowed on the first member: the inner
   * sub-Query's @c targetList is built from @c first_member->proj_slots,
   * so a head Var added to a non-first-member atom's @c proj_slots
   * would not actually surface in the inner output.  Tracking this
   * here lets the per-Var check and proj_slots build below act
   * uniformly. */
  {
    int g, ngroups_local = 0;
    for (j = 0; j < natoms; j++)
      if (atom_group[j] >= 0 && atom_group[j] + 1 > ngroups_local)
        ngroups_local = atom_group[j] + 1;
    first_member_of_group = palloc(natoms * sizeof(int));
    group_singleton_counter = palloc(natoms * sizeof(int));
    for (g = 0; g < ngroups_local; g++) {
      first_member_of_group[g] = -1;
      group_singleton_counter[g] = 0;
    }
    for (j = 0; j < natoms; j++) {
      int g_loc = atom_group[j];
      if (g_loc >= 0 && first_member_of_group[g_loc] < 0)
        first_member_of_group[g_loc] = j;
    }
  }

  ok = true;

  /* Every Var anywhere in the query must belong to a class that
   * either touches every atom (slot at the outer level) or sits
   * inside the inner-group its atom belongs to.  A Var whose class
   * touches an atom subset that doesn't match any outer or inner
   * slot would leak into the outer scope with no wrap to host it. */
  for (i = 0; i < nvars; i++) {
    int c = cls[i];
    int atom_idx = (int) vars_arr[i]->varno - 1;
    if (class_atom_count[c] == natoms)
      continue;
    if (class_atom_count[c] >= 2 && class_atom_count[c] < natoms
        && atom_group[atom_idx] >= 0)
      continue;
    /* Single-atom head Var: only this atom's wrap binds the column,
     * so the wrap must expose it as an extra projection slot.
     * Outer-wrap atoms expose it in their own DISTINCT wrap; grouped
     * atoms add the slot to the inner sub-Query's targetList -- on
     * first_member at the natural next position, on non-first-members
     * at the per-group running counter after all earlier members'
     * slots (see the proj_slots build below). */
    if (class_atom_count[c] == 1 && in_targetlist[i])
      continue;
    if (provsql_verbose >= 30)
      provsql_notice("safe-query rewriter: Var (varno=%u, varattno=%d) "
                     "belongs to a class that does not match any outer or "
                     "inner-group slot -- rewrite scope does not yet cover "
                     "this case",
                     (unsigned) vars_arr[i]->varno,
                     (int) vars_arr[i]->varattno);
    ok = false;
    break;
  }
  if (!ok)
    goto bail;

  /* Build proj_slots per atom.  Every atom -- outer-wrap @em or
   * grouped -- gets the same slot layout: the root class first
   * (output position 1), then every other fully-covered shared class
   * (count == natoms) touching this atom in ascending repr order.
   * Outer-wrap atoms use the slot list directly inside their per-atom
   * @c SELECT @c DISTINCT.  Grouped atoms reuse the slot list for
   * two purposes: the first member's slot order determines the inner
   * sub-Query's @c targetList and @c groupClause (the inner exposes
   * one output column per fully-covered class), and every member's
   * slot list is consulted by the outer Var remap to map a base
   * @c attno to the matching output column of the group's
   * @c RTE_SUBQUERY. */
  for (j = 0; j < natoms; j++) {
    safe_rewrite_atom *sa = palloc(sizeof(safe_rewrite_atom));
    safe_proj_slot *root_slot;

    sa->rtindex      = (Index) (j + 1);
    sa->proj_slots   = NIL;
    sa->pushed_quals = NIL;
    sa->group_id     = atom_group[j];
    sa->outer_rtindex = 0;
    sa->inner_rtindex = 0;
    sa->root_anchor_attno = (AttrNumber) ANCHOR(root_class, j);
    if (sa->root_anchor_attno == 0)
      goto bail;                          /* impossible if root truly covers all */

    root_slot = palloc(sizeof(safe_proj_slot));
    root_slot->base_attno  = sa->root_anchor_attno;
    root_slot->class_id    = root_class;
    root_slot->outer_attno = 1;
    sa->proj_slots = lappend(sa->proj_slots, root_slot);
    for (i = 0; i < nvars; i++) {
      safe_proj_slot *slot;
      if (cls[i] != i || i == root_class)
        continue;
      if (class_atom_count[i] != natoms)
        continue;                         /* partial-coverage handled via groups */
      if (ANCHOR(i, j) == 0)
        continue;
      slot = palloc(sizeof(safe_proj_slot));
      slot->base_attno  = (AttrNumber) ANCHOR(i, j);
      slot->class_id    = i;
      slot->outer_attno = (AttrNumber) (list_length(sa->proj_slots) + 1);
      sa->proj_slots = lappend(sa->proj_slots, slot);
    }
    /* Single-atom head Vars: expose every body-only Var (singleton
     * class) that appears in the user's targetList as an extra slot.
     * For outer-wrap atoms the slot lives in the per-atom DISTINCT
     * wrap, and @c outer_attno is the natural position in the
     * atom's @c proj_slots.  For grouped atoms the slot goes into
     * the inner sub-Query's @c targetList:
     *   - on first_member, at the natural next position;
     *   - on non-first-members, at the position handed out by the
     *     group's running counter @c group_singleton_counter, which
     *     picks up after first_member's last slot. */
    for (i = 0; i < nvars; i++) {
      safe_proj_slot *slot;
      ListCell *exlc;
      bool already_have = false;
      bool is_first_member;
      if (!in_targetlist[i])
        continue;
      if (class_atom_count[cls[i]] != 1)
        continue;
      if ((int) vars_arr[i]->varno - 1 != j)
        continue;
      foreach (exlc, sa->proj_slots) {
        safe_proj_slot *ex = (safe_proj_slot *) lfirst(exlc);
        if (ex->base_attno == vars_arr[i]->varattno) {
          already_have = true;
          break;
        }
      }
      if (already_have)
        continue;
      slot = palloc(sizeof(safe_proj_slot));
      slot->base_attno = vars_arr[i]->varattno;
      slot->class_id   = cls[i];
      is_first_member = (sa->group_id >= 0
                         && first_member_of_group[sa->group_id] == j);
      if (sa->group_id < 0 || is_first_member) {
        slot->outer_attno =
            (AttrNumber) (list_length(sa->proj_slots) + 1);
      } else {
        group_singleton_counter[sa->group_id]++;
        slot->outer_attno =
            (AttrNumber) group_singleton_counter[sa->group_id];
      }
      sa->proj_slots = lappend(sa->proj_slots, slot);
    }
    /* For first_member of a group: after its singletons are added,
     * initialise the group's running counter so non-first-members
     * pick up just past first_member's last slot. */
    if (sa->group_id >= 0
        && first_member_of_group[sa->group_id] == j) {
      group_singleton_counter[sa->group_id] =
          list_length(sa->proj_slots);
    }

    /* BID alignment: when the atom is BID-tracked, every block_key
     * column must appear among the projection slots.  Otherwise the
     * wrap's @c SELECT @c DISTINCT could collapse rows from the same
     * block under different projected values into multiple output
     * rows, replicating the block's @c gate_mulinput in the final
     * circuit and breaking the read-once property.  An empty
     * @c block_key (whole table is one block) is even more
     * restrictive: rows that should stay together can be split by
     * any slot the wrap projects.  We bail there too rather than
     * risk an unsound rewrite. */
    {
      RangeTblEntry *rte =
          (RangeTblEntry *) list_nth(q->rtable, j);
      ProvenanceTableInfo info;
      if (provsql_lookup_table_info(rte->relid, &info)
          && info.kind == PROVSQL_TABLE_BID) {
        if (info.block_key_n == 0) {
          if (provsql_verbose >= 30)
            provsql_notice("safe-query rewriter: BID atom (varno=%d) "
                           "has an empty block_key (whole table is one "
                           "block); the wrap's DISTINCT could split the "
                           "block across multiple output rows, deferred",
                           j + 1);
          goto bail;
        } else {
          int k;
          for (k = 0; k < info.block_key_n; k++) {
            AttrNumber bk = info.block_key[k];
            ListCell *slc;
            bool found = false;
            foreach (slc, sa->proj_slots) {
              safe_proj_slot *slot = (safe_proj_slot *) lfirst(slc);
              if (slot->base_attno == bk) {
                found = true;
                break;
              }
            }
            if (!found) {
              if (provsql_verbose >= 30)
                provsql_notice("safe-query rewriter: BID atom (varno=%d) "
                               "has block_key column attno=%d outside the "
                               "projection slots; the wrap would split a "
                               "block, deferred",
                               j + 1, (int) bk);
              goto bail;
            }
          }
        }
      }
    }

    atoms_out = lappend(atoms_out, sa);
  }

  /* If we discovered an inner group, materialise it now.  Member
   * atoms are listed in their original rtindex order; @c inner_quals
   * is filled later by @c try_safe_query_rewrite as it partitions the
   * residual conjuncts.  All grouped atoms share the same
   * @c outer_rtindex, which the rewriter assigns when it walks the
   * outer rtable. */
  if (have_partial_class) {
    int max_gid = -1;
    int g;
    safe_inner_group **arr;
    ListCell *alc;
    foreach (alc, atoms_out) {
      safe_rewrite_atom *sa = (safe_rewrite_atom *) lfirst(alc);
      if (sa->group_id > max_gid)
        max_gid = sa->group_id;
    }
    arr = palloc0((max_gid + 1) * sizeof(safe_inner_group *));
    for (g = 0; g <= max_gid; g++) {
      arr[g] = palloc(sizeof(safe_inner_group));
      arr[g]->group_id = g;
      arr[g]->member_atoms = NIL;
      arr[g]->inner_quals = NIL;
      arr[g]->outer_rtindex = 0;
    }
    foreach (alc, atoms_out) {
      safe_rewrite_atom *sa = (safe_rewrite_atom *) lfirst(alc);
      if (sa->group_id >= 0)
        arr[sa->group_id]->member_atoms =
            lappend(arr[sa->group_id]->member_atoms, sa);
    }
    for (g = 0; g <= max_gid; g++)
      *groups_out = lappend(*groups_out, arr[g]);
    pfree(arr);
  }

#undef ANCHOR

  pfree(class_atom_count);
  pfree(class_atom_anchor_attno);
  pfree(vars_arr);
  pfree(cls);
  pfree(atom_group);
  if (in_targetlist)
    pfree(in_targetlist);
  if (first_member_of_group)
    pfree(first_member_of_group);
  if (group_singleton_counter)
    pfree(group_singleton_counter);
  (void) constants;
  return atoms_out;

bail:
  pfree(class_atom_count);
  pfree(class_atom_anchor_attno);
  pfree(vars_arr);
  pfree(cls);
  if (atom_group)
    pfree(atom_group);
  if (in_targetlist)
    pfree(in_targetlist);
  if (first_member_of_group)
    pfree(first_member_of_group);
  if (group_singleton_counter)
    pfree(group_singleton_counter);
  (void) constants;
  *groups_out = NIL;
  return NIL;
}

/**
 * @brief Mutator context for @c safe_remap_vars_mutator.
 *
 * @c atoms gives one descriptor per original RTE.  For both outer-wrap
 * atoms (@c group_id == -1) and grouped atoms (@c group_id >= 0), the
 * Var is rewritten by scanning the atom's @c proj_slots for the
 * matching @c base_attno: the new @c varno is the atom's (or its
 * group's) @c outer_rtindex, the new @c varattno is the slot's
 * 1-based position in @c proj_slots.  A Var whose @c base_attno is
 * not in any slot (i.e. the column does not belong to any fully-
 * covered shared class) triggers an error -- the wrap / inner sub-
 * Query has no matching output column for it, and the detector should
 * have rejected such a query.
 */
typedef struct safe_remap_ctx {
  List *atoms;   ///< List of safe_rewrite_atom *, one per RTE
  List *groups;  ///< List of safe_inner_group *
} safe_remap_ctx;

/**
 * @brief Rewrite Var nodes in the outer query after each base RTE has
 *        been wrapped as a DISTINCT subquery projecting one or more
 *        slot columns.
 *
 * For each base-level Var (varno, varattno), the matching atom is
 * @c atoms[varno-1].  We scan its @c proj_slots in order, looking
 * for a slot with @c base_attno == varattno, and remap the Var to
 * the 1-based output position of that slot.  A Var with no matching
 * slot indicates the detector accepted a query it shouldn't have;
 * we @c provsql_error to surface the bug rather than silently emit
 * a broken plan.
 */
static Node *safe_remap_vars_mutator(Node *node, safe_remap_ctx *ctx) {
  if (node == NULL)
    return NULL;
  if (IsA(node, Var)) {
    Var *v = (Var *) node;
    if (v->varlevelsup == 0
        && v->varno >= 1 && (int) v->varno <= list_length(ctx->atoms)) {
      safe_rewrite_atom *sa =
          (safe_rewrite_atom *) list_nth(ctx->atoms, (int) v->varno - 1);
      if (sa->group_id >= 0) {
        safe_inner_group *gr =
            (safe_inner_group *) list_nth(ctx->groups, sa->group_id);
        ListCell *lc;
        foreach (lc, sa->proj_slots) {
          safe_proj_slot *slot = (safe_proj_slot *) lfirst(lc);
          if (slot->base_attno == v->varattno) {
            Var *vv = (Var *) copyObject(v);
            vv->varno = gr->outer_rtindex;
#if PG_VERSION_NUM >= 130000
            vv->varnosyn = gr->outer_rtindex;
#endif
            vv->varattno = slot->outer_attno;
#if PG_VERSION_NUM >= 130000
            vv->varattnosyn = slot->outer_attno;
#endif
            return (Node *) vv;
          }
        }
        provsql_error("safe-query rewriter: Var (varno=%u, varattno=%d) "
                      "references a grouped atom column with no slot in "
                      "the inner sub-Query's targetList -- the column is "
                      "not in any fully-covered shared class",
                      (unsigned) v->varno, (int) v->varattno);
      } else {
        ListCell *lc;
        foreach (lc, sa->proj_slots) {
          safe_proj_slot *slot = (safe_proj_slot *) lfirst(lc);
          if (slot->base_attno == v->varattno) {
            Var *vv = (Var *) copyObject(v);
            vv->varno = sa->outer_rtindex;
#if PG_VERSION_NUM >= 130000
            vv->varnosyn = sa->outer_rtindex;
#endif
            vv->varattno = slot->outer_attno;
#if PG_VERSION_NUM >= 130000
            vv->varattnosyn = slot->outer_attno;
#endif
            return (Node *) vv;
          }
        }
        provsql_error("safe-query rewriter: Var (varno=%u, varattno=%d) "
                      "has no projection slot in atom %u",
                      (unsigned) v->varno, (int) v->varattno,
                      (unsigned) v->varno);
      }
    }
    return (Node *) v;
  }
  return expression_tree_mutator(node, safe_remap_vars_mutator, (void *) ctx);
}

/**
 * @brief Build the inner @c Query that projects every slot in
 *        @p proj_slots of @p base_rte under @c SELECT @c DISTINCT.
 *
 * One @c TargetEntry and one @c SortGroupClause are emitted per slot,
 * in @p proj_slots order; the root-class slot is conventionally first,
 * so it always ends up at output attno 1.  The recursive
 * @c process_query call on this @c Query will discover the @c provsql
 * column on @p base_rte and append it to the inner target list, so the
 * wrapping outer query gets the slot columns at attno @c 1..N and the
 * @c provsql column at attno @c N+1.
 */
static Query *safe_build_inner_wrap(Query *outer_src,
                                    RangeTblEntry *base_rte,
                                    List *proj_slots,
                                    Index outer_rtindex,
                                    List *pushed_quals) {
  Query           *inner = makeNode(Query);
  RangeTblRef     *rtr   = makeNode(RangeTblRef);
  FromExpr        *jt    = makeNode(FromExpr);
  RangeTblEntry   *inner_rte;
  ListCell        *lc;
  int              slot_idx = 0;

  inner_rte = copyObject(base_rte);

  inner->commandType   = CMD_SELECT;
  inner->canSetTag     = false;
  inner->rtable        = list_make1(inner_rte);
#if PG_VERSION_NUM >= 160000
  /* The cloned RTE's perminfoindex pointed into the OUTER query's
   * rteperminfos list; reattach the matching RTEPermissionInfo to
   * the inner query so the planner finds it under inner->rteperminfos
   * (otherwise list_nth_node on an empty list segfaults during
   * post-processing). */
  if (base_rte->perminfoindex != 0
      && outer_src && outer_src->rteperminfos != NIL
      && (int) base_rte->perminfoindex <= list_length(outer_src->rteperminfos)) {
    RTEPermissionInfo *rpi = list_nth_node(RTEPermissionInfo,
                                           outer_src->rteperminfos,
                                           base_rte->perminfoindex - 1);
    inner->rteperminfos = list_make1(copyObject(rpi));
    inner_rte->perminfoindex = 1;
  } else {
    inner->rteperminfos = NIL;
    inner_rte->perminfoindex = 0;
  }
#endif
  rtr->rtindex         = 1;
  jt->fromlist         = list_make1(rtr);
  jt->quals            = NULL;
  inner->jointree      = jt;

  inner->targetList     = NIL;
  inner->distinctClause = NIL;
  inner->hasDistinctOn  = false;

  foreach (lc, proj_slots) {
    safe_proj_slot   *slot = (safe_proj_slot *) lfirst(lc);
    HeapTuple         atttup;
    Form_pg_attribute attform;
    Oid               atttypid;
    int32             atttypmod;
    Oid               attcollation;
    Var              *v;
    TargetEntry      *te = makeNode(TargetEntry);
    SortGroupClause  *sgc = makeNode(SortGroupClause);

    atttup = SearchSysCache2(ATTNUM,
                             ObjectIdGetDatum(base_rte->relid),
                             Int16GetDatum(slot->base_attno));
    if (!HeapTupleIsValid(atttup))
      provsql_error("safe-query rewriter: cannot resolve attno %d of "
                    "relation %u",
                    (int) slot->base_attno, (unsigned) base_rte->relid);
    attform      = (Form_pg_attribute) GETSTRUCT(atttup);
    atttypid     = attform->atttypid;
    atttypmod    = attform->atttypmod;
    attcollation = attform->attcollation;
    ReleaseSysCache(atttup);

    slot_idx++;
    v = makeVar(1, slot->base_attno, atttypid, atttypmod, attcollation, 0);
    te->expr            = (Expr *) v;
    te->resno           = (AttrNumber) slot_idx;
    te->resname         = psprintf("provsql_slot_%d", slot_idx);
    te->ressortgroupref = (Index) slot_idx;
    te->resorigtbl      = base_rte->relid;
    te->resorigcol      = slot->base_attno;
    te->resjunk         = false;
    inner->targetList = lappend(inner->targetList, te);

    sgc->tleSortGroupRef = (Index) slot_idx;
    get_sort_group_operators(atttypid, true, true, false,
                             &sgc->sortop, &sgc->eqop, NULL, &sgc->hashable);
    sgc->nulls_first     = false;
    inner->distinctClause = lappend(inner->distinctClause, sgc);
  }

  /* Inject the pushed-down atom-local quals.  Each is @c copyObject'd
   * (so the outer query's residual tree is untouched), then its base-
   * level @c Var.varno is rewritten from the outer atom's rtindex to
   * @c 1 -- the only RTE in the inner subquery.  Single conjunct goes
   * in directly; multiple conjuncts are AND-bundled. */
  if (pushed_quals != NIL) {
    safe_pushed_remap_ctx rctx;
    List     *remapped = NIL;
    ListCell *qlc;
    rctx.outer_rtindex = outer_rtindex;
    foreach (qlc, pushed_quals) {
      Node *q = (Node *) copyObject(lfirst(qlc));
      q = safe_pushed_remap_mutator(q, &rctx);
      remapped = lappend(remapped, q);
    }
    if (list_length(remapped) == 1)
      inner->jointree->quals = (Node *) linitial(remapped);
    else
      inner->jointree->quals = (Node *) makeBoolExpr(AND_EXPR, remapped, -1);
  }

  return inner;
}

/** @brief Mutator context for @c safe_inner_varno_remap_mutator. */
typedef struct safe_inner_varno_remap_ctx {
  int *orig_to_inner;  ///< 1-indexed array: orig rtindex -> inner rtindex (0 if not in group)
  int  natoms;
} safe_inner_varno_remap_ctx;

/**
 * @brief Rewrite base-level @c Var.varno from the outer atom rtindex to
 *        the corresponding inner-sub-Query rtindex.
 *
 * Applied to each conjunct that the partition pass routed into an inner
 * group (and to every pushed-down atom-local qual of every grouped
 * atom) before injection into the inner sub-Query's
 * @c jointree->quals.  @c varattno is unchanged -- the inner
 * sub-Query's RTEs are fresh clones of the same base relations, so the
 * base attribute numbers carry over.
 */
static Node *safe_inner_varno_remap_mutator(Node *node,
                                            safe_inner_varno_remap_ctx *ctx) {
  if (node == NULL)
    return NULL;
  if (IsA(node, Var)) {
    Var *v = (Var *) node;
    if (v->varlevelsup == 0
        && v->varno >= 1 && (int) v->varno <= ctx->natoms) {
      int newno = ctx->orig_to_inner[v->varno];
      if (newno > 0) {
        Var *nv = (Var *) copyObject(v);
        nv->varno = (Index) newno;
#if PG_VERSION_NUM >= 130000
        nv->varnosyn = (Index) newno;
#endif
        return (Node *) nv;
      }
    }
    return node;
  }
  return expression_tree_mutator(node,
                                 safe_inner_varno_remap_mutator,
                                 (void *) ctx);
}

/**
 * @brief Build the inner sub-Query that aggregates a group of
 *        partial-coverage atoms over their non-root shared variables.
 *
 * The sub-Query's @c rtable contains a clone of each member atom's
 * @c RangeTblEntry, in original-rtindex order.  Its @c WHERE is the AND
 * of @c gr->inner_quals (cross-atom conjuncts within the group) and
 * every member atom's @c pushed_quals; each conjunct's @c Var.varno is
 * remapped from the outer atom rtindex to the inner rtindex via
 * @c safe_inner_varno_remap_mutator.  The @c targetList exposes a single
 * column carrying the root-class binding of the first member; the
 * @c groupClause aggregates the per-group provenance over the inner
 * shared variables.  When @c process_query re-enters on this sub-Query,
 * the hierarchical-CQ rewriter fires again and wraps each member atom
 * with its own @c SELECT @c DISTINCT.
 */
static Query *safe_build_group_subquery(Query *outer_src,
                                        safe_inner_group *gr,
                                        List *atoms) {
  Query           *inner = makeNode(Query);
  FromExpr        *jt    = makeNode(FromExpr);
  safe_rewrite_atom *first_member;
  RangeTblEntry   *first_rte;
  HeapTuple        atttup;
  Form_pg_attribute attform;
  Oid              atttypid;
  int32            atttypmod;
  Oid              attcollation;
  ListCell        *lc;
  int              inner_idx = 0;
  safe_inner_varno_remap_ctx rctx;
  int              natoms = list_length(atoms);

  inner->commandType   = CMD_SELECT;
  inner->canSetTag     = false;
  inner->rtable        = NIL;
  inner->jointree      = jt;
  jt->fromlist         = NIL;
  jt->quals            = NULL;
#if PG_VERSION_NUM >= 160000
  inner->rteperminfos  = NIL;
#endif

  rctx.orig_to_inner = palloc0((natoms + 1) * sizeof(int));
  rctx.natoms        = natoms;

  /* Clone each member atom's RTE into the inner rtable and record its
   * inner rtindex.  Order follows the @c member_atoms list, which is
   * itself in original-rtindex order, so the inner rtindex matches the
   * member's natural reading order. */
  foreach (lc, gr->member_atoms) {
    safe_rewrite_atom *sa = (safe_rewrite_atom *) lfirst(lc);
    RangeTblEntry *src_rte =
        (RangeTblEntry *) list_nth(outer_src->rtable, (int) sa->rtindex - 1);
    RangeTblEntry *cloned = (RangeTblEntry *) copyObject(src_rte);
    RangeTblRef   *rtr    = makeNode(RangeTblRef);

    inner_idx++;
    sa->inner_rtindex = (Index) inner_idx;
    rctx.orig_to_inner[sa->rtindex] = inner_idx;

#if PG_VERSION_NUM >= 160000
    if (cloned->perminfoindex != 0
        && outer_src->rteperminfos != NIL
        && (int) cloned->perminfoindex
           <= list_length(outer_src->rteperminfos)) {
      RTEPermissionInfo *rpi = list_nth_node(RTEPermissionInfo,
                                             outer_src->rteperminfos,
                                             cloned->perminfoindex - 1);
      inner->rteperminfos =
          lappend(inner->rteperminfos, copyObject(rpi));
      cloned->perminfoindex = (Index) list_length(inner->rteperminfos);
    } else {
      cloned->perminfoindex = 0;
    }
#endif

    inner->rtable = lappend(inner->rtable, cloned);
    rtr->rtindex  = inner_idx;
    jt->fromlist  = lappend(jt->fromlist, rtr);
  }

  /* Assemble the inner WHERE: cross-atom conjuncts the partition pass
   * routed here, plus each member atom's pushed atom-local quals
   * (the atom-local pre-pass will re-extract them when the rewriter
   * re-enters on the inner sub-Query, but the conjuncts must travel
   * along with their atoms so the re-entry's @c safe_split_quals sees
   * them). */
  {
    List *all_quals = NIL;
    foreach (lc, gr->inner_quals)
      all_quals = lappend(all_quals,
                          copyObject((Node *) lfirst(lc)));
    foreach (lc, gr->member_atoms) {
      safe_rewrite_atom *sa = (safe_rewrite_atom *) lfirst(lc);
      ListCell *qlc;
      foreach (qlc, sa->pushed_quals)
        all_quals = lappend(all_quals,
                            copyObject((Node *) lfirst(qlc)));
    }
    {
      ListCell *qlc;
      List *remapped = NIL;
      foreach (qlc, all_quals) {
        Node *qq = (Node *) lfirst(qlc);
        qq = safe_inner_varno_remap_mutator(qq, &rctx);
        remapped = lappend(remapped, qq);
      }
      if (remapped == NIL)
        jt->quals = NULL;
      else if (list_length(remapped) == 1)
        jt->quals = (Node *) linitial(remapped);
      else
        jt->quals = (Node *) makeBoolExpr(AND_EXPR, remapped, -1);
    }
  }

  /* targetList: one TargetEntry per fully-covered shared class, in the
   * order of the first member's @c proj_slots (root first, then
   * other fully-covered classes by ascending repr).  All members of
   * the group agree on each fully-covered class's value inside the
   * group, so picking the first member's columns is correct.  The
   * @c groupClause has a matching @c SortGroupClause per slot. */
  first_member = (safe_rewrite_atom *) linitial(gr->member_atoms);
  first_rte    = (RangeTblEntry *)
      list_nth(outer_src->rtable, (int) first_member->rtindex - 1);

  inner->targetList = NIL;
  inner->groupClause = NIL;
  /* Emit one TargetEntry per slot in @c outer_attno order, covering
   * first_member's slots first, then each non-first-member's
   * singleton-only slots in member-list order.  Slots with the same
   * @c outer_attno across members (shared root + fully-covered
   * classes) are emitted once, attached to the first member that
   * owns them.  We track which @c outer_attno values have already
   * been emitted via a Bitmapset. */
  {
    Bitmapset *emitted = NULL;
    ListCell  *mlc;
    foreach (mlc, gr->member_atoms) {
      safe_rewrite_atom *m = (safe_rewrite_atom *) lfirst(mlc);
      RangeTblEntry *m_rte = (RangeTblEntry *)
          list_nth(outer_src->rtable, (int) m->rtindex - 1);
      ListCell *slot_lc;
      foreach (slot_lc, m->proj_slots) {
        safe_proj_slot  *slot = (safe_proj_slot *) lfirst(slot_lc);
        TargetEntry    *te;
        SortGroupClause *sgc;
        Var            *cv;
        if (bms_is_member((int) slot->outer_attno, emitted))
          continue;
        emitted = bms_add_member(emitted, (int) slot->outer_attno);

        atttup = SearchSysCache2(ATTNUM,
                                 ObjectIdGetDatum(m_rte->relid),
                                 Int16GetDatum(slot->base_attno));
        if (!HeapTupleIsValid(atttup))
          provsql_error("safe-query rewriter: cannot resolve attno %d of "
                        "relation %u in inner sub-Query",
                        (int) slot->base_attno, (unsigned) m_rte->relid);
        attform      = (Form_pg_attribute) GETSTRUCT(atttup);
        atttypid     = attform->atttypid;
        atttypmod    = attform->atttypmod;
        attcollation = attform->attcollation;
        ReleaseSysCache(atttup);

        te  = makeNode(TargetEntry);
        sgc = makeNode(SortGroupClause);
        cv = makeVar((Index) m->inner_rtindex,
                     slot->base_attno,
                     atttypid, atttypmod, attcollation, 0);
        te->expr            = (Expr *) cv;
        te->resno           = slot->outer_attno;
        te->resname         = psprintf("provsql_slot_%d",
                                       (int) slot->outer_attno);
        te->ressortgroupref = (Index) slot->outer_attno;
        te->resorigtbl      = m_rte->relid;
        te->resorigcol      = slot->base_attno;
        te->resjunk         = false;
        inner->targetList = lappend(inner->targetList, te);

        sgc->tleSortGroupRef = (Index) slot->outer_attno;
        get_sort_group_operators(atttypid, true, true, false,
                                 &sgc->sortop, &sgc->eqop, NULL,
                                 &sgc->hashable);
        sgc->nulls_first     = false;
        inner->groupClause = lappend(inner->groupClause, sgc);
      }
    }
    bms_free(emitted);
  }

  (void) first_member; (void) first_rte;
  pfree(rctx.orig_to_inner);
  return inner;
}

/**
 * @brief Apply the (multi-level when needed) hierarchical-CQ rewrite.
 *
 * Walks the outer rtable in original order.  Each atom is replaced by
 * an @c RTE_SUBQUERY.  Atoms with @c group_id @c == @c -1 get a direct
 * outer wrap (@c SELECT @c DISTINCT on their projection slots).  The
 * first atom of each inner group emits the group's sub-Query
 * (@c safe_build_group_subquery), and subsequent group members are
 * skipped from the outer rtable -- they live inside the inner
 * sub-Query.  The outer rtable therefore has one entry per outer-wrap
 * atom plus one entry per inner group, generally fewer than the
 * original.
 *
 * The remap pass then rewrites every base Var in the outer
 * @c targetList and residual WHERE.  Both outer-wrap and grouped
 * Vars resolve by scanning the atom's @c proj_slots for the matching
 * @c base_attno: the new @c varno is the atom's (or its group's)
 * @c outer_rtindex, and the new @c varattno is the slot's 1-based
 * position in @c proj_slots (which matches the output column of the
 * outer wrap or of the inner sub-Query's @c targetList).
 */
static Query *rewrite_hierarchical_cq(const constants_t *constants,
                                      Query *q, List *atoms, List *groups,
                                      Node *residual) {
  Query        *outer = copyObject(q);
  safe_remap_ctx mctx;
  List         *new_rtable = NIL;
#if PG_VERSION_NUM >= 160000
  List         *new_rteperminfos = NIL;
#endif
  List         *new_fromlist = NIL;
  ListCell     *lc;
  int           j;
  int           outer_pos = 0;
  int           ngroups = list_length(groups);
  bool         *group_emitted = NULL;
  int           total_atoms_in_groups = 0;
  int           ninner = 0;

  (void) constants;

  if (ngroups > 0) {
    group_emitted = palloc0(ngroups * sizeof(bool));
    foreach (lc, groups) {
      safe_inner_group *gr = (safe_inner_group *) lfirst(lc);
      total_atoms_in_groups += list_length(gr->member_atoms);
    }
  }

  /* Replace the outer WHERE with the residual (atom-local conjuncts
   * were extracted upstream; conjuncts wholly inside a group were
   * routed into that group's inner_quals before this function runs).
   * A fresh @c copyObject keeps the outer tree independent. */
  if (outer->jointree)
    outer->jointree->quals =
        residual ? (Node *) copyObject(residual) : NULL;

  /* Walk original rtable in order; emit either a direct per-atom
   * outer wrap or, the first time we hit a group member, the group's
   * inner sub-Query RTE. */
  j = 0;
  foreach (lc, outer->rtable) {
    RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
    safe_rewrite_atom *sa = (safe_rewrite_atom *) list_nth(atoms, j);
    RangeTblRef *rtr;

    if (sa->group_id < 0) {
      Query *inner = safe_build_inner_wrap(outer, rte, sa->proj_slots,
                                           sa->rtindex, sa->pushed_quals);
      RangeTblEntry *new_rte = makeNode(RangeTblEntry);
      Alias *eref = makeNode(Alias);
      ListCell *slot_lc;
      int slot_idx = 0;

      eref->aliasname = rte->eref && rte->eref->aliasname
                        ? pstrdup(rte->eref->aliasname)
                        : pstrdup("provsql_wrap");
      eref->colnames = NIL;
      foreach (slot_lc, sa->proj_slots) {
        (void) lfirst(slot_lc);
        slot_idx++;
        eref->colnames = lappend(eref->colnames,
                                 makeString(psprintf("provsql_slot_%d",
                                                     slot_idx)));
      }

      new_rte->rtekind  = RTE_SUBQUERY;
      new_rte->subquery = inner;
      new_rte->alias    = NULL;
      new_rte->eref     = eref;
      new_rte->inFromCl = rte->inFromCl;
      new_rte->lateral  = false;
#if PG_VERSION_NUM < 160000
      new_rte->requiredPerms = 0;
#endif

      outer_pos++;
      sa->outer_rtindex = (Index) outer_pos;
      new_rtable = lappend(new_rtable, new_rte);
      rtr = makeNode(RangeTblRef);
      rtr->rtindex = outer_pos;
      new_fromlist = lappend(new_fromlist, rtr);
    } else {
      int g = sa->group_id;
      safe_inner_group *gr = (safe_inner_group *) list_nth(groups, g);

      if (!group_emitted[g]) {
        Query *inner = safe_build_group_subquery(outer, gr, atoms);
        RangeTblEntry *new_rte = makeNode(RangeTblEntry);
        Alias *eref = makeNode(Alias);
        int slot_idx = 0;
        int total_inner_cols = inner->targetList != NIL
                               ? list_length(inner->targetList) : 0;

        eref->aliasname = pstrdup("provsql_group");
        eref->colnames = NIL;
        for (slot_idx = 1; slot_idx <= total_inner_cols; slot_idx++) {
          eref->colnames = lappend(eref->colnames,
                                   makeString(psprintf("provsql_slot_%d",
                                                       slot_idx)));
        }

        new_rte->rtekind  = RTE_SUBQUERY;
        new_rte->subquery = inner;
        new_rte->alias    = NULL;
        new_rte->eref     = eref;
        new_rte->inFromCl = rte->inFromCl;
        new_rte->lateral  = false;
#if PG_VERSION_NUM < 160000
        new_rte->requiredPerms = 0;
#endif

        outer_pos++;
        gr->outer_rtindex = (Index) outer_pos;
        new_rtable = lappend(new_rtable, new_rte);
        rtr = makeNode(RangeTblRef);
        rtr->rtindex = outer_pos;
        new_fromlist = lappend(new_fromlist, rtr);
        group_emitted[g] = true;
        ninner++;
      }
      sa->outer_rtindex = gr->outer_rtindex;
    }
    j++;
  }

  outer->rtable = new_rtable;
  if (outer->jointree)
    outer->jointree->fromlist = new_fromlist;
#if PG_VERSION_NUM >= 160000
  /* The rebuilt rtable is a fresh list of RTE_SUBQUERY entries; none
   * of them reference @c outer->rteperminfos, so clear it.  The inner
   * sub-Queries carry their own @c rteperminfos. */
  outer->rteperminfos = new_rteperminfos;
#endif

  /* Remap outer Vars: outer-wrap atoms resolve to their slot's column
   * of their atom's wrapping subquery; grouped atoms resolve to the
   * group's inner sub-Query at output column 1. */
  mctx.atoms = atoms;
  mctx.groups = groups;
  outer->targetList = (List *)
      safe_remap_vars_mutator((Node *) outer->targetList, &mctx);
  if (outer->jointree && outer->jointree->quals)
    outer->jointree->quals =
        safe_remap_vars_mutator(outer->jointree->quals, &mctx);

  if (group_emitted)
    pfree(group_emitted);

  if (provsql_verbose >= 30) {
    StringInfoData buf;
    int total_slots   = 0;
    int total_pushed  = 0;
    int has_col_push  = 0;
    foreach (lc, atoms) {
      safe_rewrite_atom *sa = (safe_rewrite_atom *) lfirst(lc);
      total_slots  += list_length(sa->proj_slots);
      total_pushed += list_length(sa->pushed_quals);
      if (list_length(sa->proj_slots) > 1)
        has_col_push = 1;
    }
    initStringInfo(&buf);
    appendStringInfo(&buf,
                     "safe-query rewrite fired: wrapped %d atoms with "
                     "SELECT DISTINCT on %d total slot(s)",
                     list_length(atoms) - total_atoms_in_groups,
                     total_slots);
    if (ninner > 0) {
      appendStringInfo(&buf,
                       ", folded %d atom%s into %d inner sub-Quer%s",
                       total_atoms_in_groups,
                       total_atoms_in_groups == 1 ? "" : "s",
                       ninner, ninner == 1 ? "y" : "ies");
    }
    if (has_col_push || total_pushed > 0) {
      const char *sep = " (";
      if (has_col_push) {
        appendStringInfoString(&buf, sep);
        appendStringInfoString(&buf, "column pushdown");
        sep = "; ";
      }
      if (total_pushed > 0) {
        appendStringInfoString(&buf, sep);
        appendStringInfo(&buf, "%d atom-local qual%s pushed",
                         total_pushed, total_pushed == 1 ? "" : "s");
      }
      appendStringInfoChar(&buf, ')');
    }
    provsql_notice("%s", buf.data);
    pfree(buf.data);
  }

  return outer;
}

/**
 * @brief Compute atom-level connected components.
 *
 * Two atoms belong to the same component iff there is a chain of
 * equality conjuncts in @p quals that connects one of their Vars to
 * one of the other's Vars.  Uses a quick atom-level union-find driven
 * by the equality pairs already extracted by
 * @c safe_collect_equalities, then compacts representatives into
 * 0-based component ids written into @p atom_to_comp.
 *
 * @return Number of distinct components.
 */
static int compute_atom_components(Query *q, Node *quals, int *atom_to_comp) {
  int       natoms = list_length(q->rtable);
  int      *dsu    = palloc(natoms * sizeof(int));
  List     *eq_pairs = NIL;
  int      *root_to_comp;
  int       ncomp = 0;
  int       j;
  ListCell *lc;

  for (j = 0; j < natoms; j++)
    dsu[j] = j;

  safe_collect_equalities(quals, &eq_pairs);
  for (lc = list_head(eq_pairs); lc != NULL; lc = my_lnext(eq_pairs, lc)) {
    Var *lv, *rv;
    int  la, ra;
    lv = (Var *) lfirst(lc);
    lc = my_lnext(eq_pairs, lc);
    rv = (Var *) lfirst(lc);
    la = (int) lv->varno - 1;
    ra = (int) rv->varno - 1;
    if (la < 0 || la >= natoms || ra < 0 || ra >= natoms)
      continue;
    while (dsu[la] != la) la = dsu[la];
    while (dsu[ra] != ra) ra = dsu[ra];
    if (la != ra)
      dsu[la] = ra;
  }

  root_to_comp = palloc(natoms * sizeof(int));
  for (j = 0; j < natoms; j++) {
    int r = j;
    int k;
    bool found = false;
    while (dsu[r] != r) r = dsu[r];
    dsu[j] = r;
    for (k = 0; k < ncomp; k++) {
      if (root_to_comp[k] == r) {
        atom_to_comp[j] = k;
        found = true;
        break;
      }
    }
    if (!found) {
      root_to_comp[ncomp] = r;
      atom_to_comp[j] = ncomp++;
    }
  }
  pfree(root_to_comp);
  pfree(dsu);
  return ncomp;
}

/** @brief Mutator context for @c safe_outer_te_remap_mutator. */
typedef struct safe_outer_te_remap_ctx {
  int    *atom_to_comp;            ///< per-atom component id
  int    *atom_to_inner_attno;     ///< per-atom column position in its component's inner targetList (1-based; 0 = not exposed)
  Index  *comp_to_outer_rtindex;   ///< per-component outer-rtable position (1-based)
} safe_outer_te_remap_ctx;

/**
 * @brief Rewrite Vars in the outer targetList for the multi-component
 *        rewrite.
 *
 * Each base-level Var(varno=v, varattno=a) in the user's targetList is
 * looked up in @c atom_to_inner_attno[v-1] to find which output column
 * of the matching component's inner sub-Query carries the Var, then
 * rewritten to point at that component's @c RTE_SUBQUERY in the outer
 * rtable.  A Var whose @c atom_to_inner_attno entry is 0 (i.e. the
 * detector did not pick this column for its inner sub-Query)
 * indicates a bug or a query the caller should have refused; we
 * @c provsql_error to surface it.
 */
static Node *safe_outer_te_remap_mutator(Node *node,
                                         safe_outer_te_remap_ctx *ctx) {
  if (node == NULL)
    return NULL;
  if (IsA(node, Var)) {
    Var *v = (Var *) node;
    if (v->varlevelsup == 0 && v->varno >= 1) {
      int atom_idx = (int) v->varno - 1;
      int comp     = ctx->atom_to_comp[atom_idx];
      AttrNumber inner_attno =
          (AttrNumber) ctx->atom_to_inner_attno[atom_idx];
      Index outer_rtindex = ctx->comp_to_outer_rtindex[comp];
      Var *vv;
      if (inner_attno == 0)
        provsql_error("safe-query multi-component rewriter: Var "
                      "(varno=%u, varattno=%d) has no exposed column "
                      "in its component's inner sub-Query",
                      (unsigned) v->varno, (int) v->varattno);
      vv = (Var *) copyObject(v);
      vv->varno     = outer_rtindex;
      vv->varattno  = inner_attno;
#if PG_VERSION_NUM >= 130000
      vv->varnosyn    = outer_rtindex;
      vv->varattnosyn = inner_attno;
#endif
      return (Node *) vv;
    }
    return node;
  }
  return expression_tree_mutator(node, safe_outer_te_remap_mutator,
                                 (void *) ctx);
}

/**
 * @brief Apply the multi-component rewrite.
 *
 * Assumes @p atom_to_comp partitions the @c q->rtable atoms into
 * @p ncomp connected components (@p ncomp >= 2) and that every
 * @c TargetEntry in @c q->targetList has all its Vars in a single
 * component.  Builds one inner @c Query per component, each carrying:
 *  - the component's atoms as @c RTE_RELATION clones,
 *  - the cross-atom WHERE conjuncts and atom-local pushed quals
 *    confined to those atoms,
 *  - the slice of @c q->targetList referencing this component's
 *    atoms (fresh @c ressortgroupref) plus matching @c groupClause,
 * and assembles an outer @c Query whose @c rtable is the list of
 * inner sub-Queries.  Each output row's provenance is the
 * @c gate_times of the per-component provsqls; Choice A re-entry
 * lets the single-component rewriter handle each component
 * independently.
 *
 * Returns @c NULL to fall through when any component has no Var-
 * carrying @c TargetEntry to anchor its inner sub-Query (the all-
 * constant case, e.g. @c SELECT @c DISTINCT @c 1 @c FROM @c A,B,
 * is deferred).
 */
static Query *rewrite_multi_component(const constants_t *constants,
                                      Query *q,
                                      Node *residual,
                                      List **per_atom_quals,
                                      int *atom_to_comp,
                                      int ncomp) {
  Query        *outer;
  int           natoms = list_length(q->rtable);
  Query       **inner_queries;
  List        **inner_quals;       /* per-component list of Node* */
  List        **inner_tlists;      /* per-component list of TargetEntry* (orig) */
  int          *comp_inner_idx;    /* per-component running rtindex counter */
  int          *atom_to_inner_idx; /* per-atom 1-based rtindex inside its component */
  int          *atom_to_inner_attno; /* per-atom 1-based attno of its first targetList exposure */
  Index        *comp_outer_rtindex;
  int           k, j;
  ListCell     *lc;
  List         *conjuncts = NIL;
  List         *outer_resid = NIL;

  (void) constants;

  /* Allocate per-component scratch. */
  inner_quals        = palloc0(ncomp * sizeof(List *));
  inner_tlists       = palloc0(ncomp * sizeof(List *));
  comp_inner_idx     = palloc0(ncomp * sizeof(int));
  atom_to_inner_idx  = palloc0(natoms * sizeof(int));
  atom_to_inner_attno = palloc0(natoms * sizeof(int));
  comp_outer_rtindex = palloc0(ncomp * sizeof(Index));

  /* Assign per-component inner rtindexes in original-rtindex order. */
  for (j = 0; j < natoms; j++) {
    int c = atom_to_comp[j];
    comp_inner_idx[c]++;
    atom_to_inner_idx[j] = comp_inner_idx[c];
  }

  /* Partition the user's targetList by component.  Reject any TE
   * whose Vars span more than one component (impossible for a truly
   * disconnected CQ -- belt-and-braces).  Reject the all-constant
   * case (a TE with no Vars at all) by returning NULL; we defer
   * that. */
  foreach (lc, q->targetList) {
    TargetEntry *te = (TargetEntry *) lfirst(lc);
    safe_collect_varnos_ctx vctx = { NULL };
    int v;
    int chosen = -1;
    safe_collect_varnos_walker((Node *) te->expr, &vctx);
    if (bms_is_empty(vctx.varnos)) {
      /* No atom Vars: a constant-only or @c provenance()-only TE
       * (the latter is rewritten downstream).  It stays at the outer
       * level; nothing to push into any inner sub-Query. */
      bms_free(vctx.varnos);
      continue;
    }
    v = -1;
    while ((v = bms_next_member(vctx.varnos, v)) >= 0) {
      int c;
      if (v < 1 || v > natoms) {
        bms_free(vctx.varnos);
        return NULL;
      }
      c = atom_to_comp[v - 1];
      if (chosen < 0)
        chosen = c;
      else if (chosen != c) {
        bms_free(vctx.varnos);
        return NULL;                /* cross-component TE; not disconnected */
      }
    }
    bms_free(vctx.varnos);
    inner_tlists[chosen] = lappend(inner_tlists[chosen], te);
  }

  /* A component with no user-Var TargetEntry still needs an anchor
   * inside its inner sub-Query: without something in the targetList,
   * the inner has no column to group on and PostgreSQL won't accept
   * the subquery.  Synthesise a @c Const(1) for those components.
   * The outer doesn't reference these anchors (no user TE points at
   * them); they only exist to fold the inner to one row per per-
   * component grouping (here, one row total since there are no
   * Vars to group by). */
  for (k = 0; k < ncomp; k++) {
    if (inner_tlists[k] == NIL) {
      TargetEntry *anchor = makeNode(TargetEntry);
      anchor->expr = (Expr *) makeConst(INT4OID, -1, InvalidOid,
                                        sizeof(int32),
                                        Int32GetDatum(1), false, true);
      anchor->resno           = 1;
      anchor->resname         = pstrdup("provsql_anchor");
      anchor->ressortgroupref = 1;
      anchor->resjunk         = false;
      inner_tlists[k] = list_make1(anchor);
    }
  }

  /* Partition the residual cross-atom conjuncts by component.  A
   * conjunct whose Vars span more than one component stays at the
   * outer level (shouldn't happen for a truly disconnected CQ but be
   * defensive). */
  safe_flatten_and(residual, &conjuncts);
  foreach (lc, conjuncts) {
    Node *qual = (Node *) lfirst(lc);
    safe_collect_varnos_ctx vctx = { NULL };
    int v;
    int chosen = -1;
    bool keep_outer = false;
    safe_collect_varnos_walker(qual, &vctx);
    v = -1;
    while ((v = bms_next_member(vctx.varnos, v)) >= 0) {
      int c;
      if (v < 1 || v > natoms) {
        keep_outer = true;
        break;
      }
      c = atom_to_comp[v - 1];
      if (chosen < 0)
        chosen = c;
      else if (chosen != c) {
        keep_outer = true;
        break;
      }
    }
    bms_free(vctx.varnos);
    if (keep_outer || chosen < 0)
      outer_resid = lappend(outer_resid, qual);
    else
      inner_quals[chosen] = lappend(inner_quals[chosen], qual);
  }

  /* Build one inner Query per component. */
  inner_queries = palloc0(ncomp * sizeof(Query *));
  for (k = 0; k < ncomp; k++) {
    Query     *inner = makeNode(Query);
    FromExpr  *jt    = makeNode(FromExpr);
    int        inner_attno = 0;
    int        inner_sgr   = 0;
    int       *orig_to_inner;

    inner->commandType = CMD_SELECT;
    inner->canSetTag   = false;
    inner->rtable      = NIL;
    inner->jointree    = jt;
    jt->fromlist       = NIL;
    jt->quals          = NULL;
#if PG_VERSION_NUM >= 160000
    inner->rteperminfos = NIL;
#endif

    orig_to_inner = palloc0((natoms + 1) * sizeof(int));

    /* Clone the component's atoms into the inner rtable. */
    for (j = 0; j < natoms; j++) {
      RangeTblEntry *src_rte, *cloned;
      RangeTblRef   *rtr;
      int inner_rtindex;
      if (atom_to_comp[j] != k)
        continue;
      src_rte = (RangeTblEntry *) list_nth(q->rtable, j);
      cloned  = (RangeTblEntry *) copyObject(src_rte);
#if PG_VERSION_NUM >= 160000
      if (cloned->perminfoindex != 0
          && q->rteperminfos != NIL
          && (int) cloned->perminfoindex <= list_length(q->rteperminfos)) {
        RTEPermissionInfo *rpi = list_nth_node(RTEPermissionInfo,
                                               q->rteperminfos,
                                               cloned->perminfoindex - 1);
        inner->rteperminfos =
            lappend(inner->rteperminfos, copyObject(rpi));
        cloned->perminfoindex = (Index) list_length(inner->rteperminfos);
      } else {
        cloned->perminfoindex = 0;
      }
#endif
      inner->rtable = lappend(inner->rtable, cloned);
      inner_rtindex = list_length(inner->rtable);
      orig_to_inner[j + 1] = inner_rtindex;
      rtr = makeNode(RangeTblRef);
      rtr->rtindex = inner_rtindex;
      jt->fromlist = lappend(jt->fromlist, rtr);
    }

    /* Inner WHERE: cross-atom conjuncts within this component + atom-
     * local pushed quals for this component's atoms.  Var.varno is
     * rewritten from the original rtindex to the inner rtindex via a
     * tiny inline remap. */
    {
      List *all = NIL;
      ListCell *qlc;
      foreach (qlc, inner_quals[k])
        all = lappend(all, copyObject((Node *) lfirst(qlc)));
      for (j = 0; j < natoms; j++) {
        if (atom_to_comp[j] != k)
          continue;
        foreach (qlc, per_atom_quals[j])
          all = lappend(all, copyObject((Node *) lfirst(qlc)));
      }
      if (all != NIL) {
        safe_inner_varno_remap_ctx rctx;
        List *remapped = NIL;
        rctx.orig_to_inner = orig_to_inner;
        rctx.natoms        = natoms;
        foreach (qlc, all) {
          Node *qq = (Node *) lfirst(qlc);
          qq = safe_inner_varno_remap_mutator(qq, &rctx);
          remapped = lappend(remapped, qq);
        }
        if (list_length(remapped) == 1)
          jt->quals = (Node *) linitial(remapped);
        else
          jt->quals = (Node *) makeBoolExpr(AND_EXPR, remapped, -1);
      }
    }

    /* Inner targetList: clone the user's TEs that landed in this
     * component, remap their Vars to the inner rtindexes, assign
     * fresh resno + ressortgroupref, and synthesise a matching
     * groupClause that GROUPs BY every slot. */
    inner->targetList = NIL;
    inner->groupClause = NIL;
    {
      ListCell *tlc;
      foreach (tlc, inner_tlists[k]) {
        TargetEntry *src_te = (TargetEntry *) lfirst(tlc);
        TargetEntry *new_te = (TargetEntry *) copyObject(src_te);
        safe_inner_varno_remap_ctx rctx;
        SortGroupClause *sgc = makeNode(SortGroupClause);
        Oid             expr_type;
        rctx.orig_to_inner = orig_to_inner;
        rctx.natoms        = natoms;
        new_te->expr = (Expr *) safe_inner_varno_remap_mutator(
            (Node *) new_te->expr, &rctx);
        inner_attno++;
        inner_sgr++;
        new_te->resno           = (AttrNumber) inner_attno;
        new_te->ressortgroupref = (Index) inner_sgr;
        new_te->resjunk         = false;
        /* Track exposure for outer Var remap.  Each user TE keeps
         * the first atom-Var encountered; for our restricted scope
         * (every TE has Vars in a single component, and each Var of
         * a given (varno, varattno) ends up at one inner column) this
         * gives the right mapping. */
        {
          safe_collect_varnos_ctx vctx = { NULL };
          int v;
          safe_collect_varnos_walker((Node *) src_te->expr, &vctx);
          v = -1;
          while ((v = bms_next_member(vctx.varnos, v)) >= 0) {
            if (v >= 1 && v <= natoms && atom_to_comp[v - 1] == k)
              atom_to_inner_attno[v - 1] = inner_attno;
          }
          bms_free(vctx.varnos);
        }
        inner->targetList = lappend(inner->targetList, new_te);

        expr_type = exprType((Node *) new_te->expr);
        sgc->tleSortGroupRef = (Index) inner_sgr;
        get_sort_group_operators(expr_type, true, true, false,
                                 &sgc->sortop, &sgc->eqop, NULL,
                                 &sgc->hashable);
        sgc->nulls_first = false;
        inner->groupClause = lappend(inner->groupClause, sgc);
      }
    }

    inner_queries[k] = inner;
    pfree(orig_to_inner);
  }

  /* Build the outer Query: rtable is one RTE_SUBQUERY per
   * component; jointree.fromlist holds N RangeTblRefs; targetList /
   * groupClause / distinctClause / etc. are copied from the user's
   * Query with Vars remapped to the matching component's inner
   * output column. */
  outer = copyObject(q);
  outer->rtable = NIL;
  outer->jointree->fromlist = NIL;
  outer->jointree->quals = (outer_resid == NIL) ? NULL
                          : (list_length(outer_resid) == 1
                             ? (Node *) linitial(outer_resid)
                             : (Node *) makeBoolExpr(AND_EXPR,
                                                     outer_resid, -1));
#if PG_VERSION_NUM >= 160000
  outer->rteperminfos = NIL;
#endif
  for (k = 0; k < ncomp; k++) {
    RangeTblEntry *new_rte = makeNode(RangeTblEntry);
    Alias *eref = makeNode(Alias);
    ListCell *tlc;
    int slot_idx = 0;

    eref->aliasname = psprintf("provsql_component_%d", k + 1);
    eref->colnames = NIL;
    foreach (tlc, inner_queries[k]->targetList) {
      TargetEntry *ite = (TargetEntry *) lfirst(tlc);
      slot_idx++;
      eref->colnames = lappend(eref->colnames,
                               makeString(ite->resname
                                          ? pstrdup(ite->resname)
                                          : psprintf("col_%d", slot_idx)));
    }

    new_rte->rtekind  = RTE_SUBQUERY;
    new_rte->subquery = inner_queries[k];
    new_rte->alias    = NULL;
    new_rte->eref     = eref;
    new_rte->inFromCl = true;
    new_rte->lateral  = false;
#if PG_VERSION_NUM < 160000
    new_rte->requiredPerms = 0;
#endif

    outer->rtable = lappend(outer->rtable, new_rte);
    comp_outer_rtindex[k] = (Index) list_length(outer->rtable);
    {
      RangeTblRef *rtr = makeNode(RangeTblRef);
      rtr->rtindex = comp_outer_rtindex[k];
      outer->jointree->fromlist = lappend(outer->jointree->fromlist, rtr);
    }
  }

  /* Remap Vars in the outer targetList and jointree.quals. */
  {
    safe_outer_te_remap_ctx tctx;
    tctx.atom_to_comp        = atom_to_comp;
    tctx.atom_to_inner_attno = atom_to_inner_attno;
    tctx.comp_to_outer_rtindex = comp_outer_rtindex;
    outer->targetList = (List *) safe_outer_te_remap_mutator(
        (Node *) outer->targetList, &tctx);
    if (outer->jointree->quals)
      outer->jointree->quals =
          safe_outer_te_remap_mutator(outer->jointree->quals, &tctx);
  }

  if (provsql_verbose >= 30)
    provsql_notice("safe-query multi-component rewrite fired: split %d "
                   "atoms into %d disconnected component%s",
                   natoms, ncomp, ncomp == 1 ? "" : "s");

  pfree(inner_queries);
  pfree(inner_quals);
  pfree(inner_tlists);
  pfree(comp_inner_idx);
  pfree(atom_to_inner_idx);
  pfree(atom_to_inner_attno);
  pfree(comp_outer_rtindex);
  return outer;
}

/**
 * @brief Top-level entry point for the safe-query rewrite.
 *
 * Runs the shape gate then the hierarchy detector.  If both accept,
 * applies the single-level rewrite and returns the rewritten Query;
 * the caller (@c process_query) feeds it back from the top so that
 * inner subqueries are themselves re-considered (multi-level
 * recursion via Choice A).  Returns @c NULL to fall through to the
 * existing pipeline.
 */
Query *try_safe_query_rewrite(const constants_t *constants, Query *q) {
  List   *atoms;
  List   *groups = NIL;
  Node   *residual = NULL;
  Node   *outer_residual = NULL;
  List  **per_atom = NULL;
  int     natoms;
  int     i;
  ListCell *lc;

#if PG_VERSION_NUM >= 180000
  /* Same trick as rewrite_agg_distinct: PG 18's RTE_GROUP virtual
   * entry derails the shape gate ("all rtable entries are
   * RTE_RELATION") and the union-find ("varno must index q->rtable")
   * before they can see the underlying base relations.  Strip it
   * here so the rest of try_safe_query_rewrite (and, on a bail, the
   * existing pipeline) see a flat range table with the grouped Vars
   * resolved back to their base-table expressions. */
  strip_group_rte_pg18(q);
#endif

  if (!is_safe_query_candidate(constants, q))
    return NULL;

  /* Atom-local pre-pass: pull out atom-local WHERE conjuncts so the
   * detector only sees Vars that participate in cross-atom structure.
   * Single-atom existential Vars hidden inside pushable predicates
   * (e.g. @c c.z @c > @c 5 in @c A(x,y),B(x,y),C(x,y,z)) thus
   * disappear from the union-find input and stop tripping the
   * "every Var in a class touching every atom" check. */
  natoms = list_length(q->rtable);
  per_atom = palloc0(natoms * sizeof(List *));
  safe_split_quals(q->jointree ? q->jointree->quals : NULL,
                   natoms, per_atom, &residual);

  /* Multi-component dispatch: when the atoms split into more than
   * one connected component (q :- A(x), B(y) with no join), the
   * single-component detector below can't find a root variable.
   * Build a Cartesian outer over one inner sub-Query per component
   * and let Choice A re-entry handle each component on its own. */
  if (natoms >= 2) {
    int *atom_to_comp = palloc(natoms * sizeof(int));
    int  ncomp = compute_atom_components(q, residual, atom_to_comp);
    if (ncomp > 1) {
      Query *rewritten = rewrite_multi_component(
          constants, q, residual, per_atom, atom_to_comp, ncomp);
      pfree(atom_to_comp);
      if (rewritten != NULL) {
        pfree(per_atom);
        return rewritten;
      }
    } else {
      pfree(atom_to_comp);
    }
  }

  atoms = find_hierarchical_root_atoms(constants, q, residual, &groups);
  if (atoms == NIL) {
    if (provsql_verbose >= 30)
      provsql_notice("safe-query candidate accepted by shape gate but no "
                     "root variable found -- falling through");
    pfree(per_atom);
    return NULL;
  }

  /* Attach per-atom pushed conjuncts to the rewrite descriptors. */
  i = 0;
  foreach (lc, atoms) {
    safe_rewrite_atom *sa = (safe_rewrite_atom *) lfirst(lc);
    sa->pushed_quals = per_atom[i];
    i++;
  }
  pfree(per_atom);

  /* With at least one inner group, partition the residual cross-atom
   * conjuncts -- those wholly inside a group move into the group's
   * inner_quals; the rest stay in the outer residual.  With no inner
   * groups, partition is a no-op (every conjunct stays outer) and the
   * rewriter does single-level outer-only wrapping. */
  if (groups != NIL)
    safe_partition_residual(residual, atoms, groups, &outer_residual);
  else
    outer_residual = residual;

  return rewrite_hierarchical_cq(constants, q, atoms, groups, outer_residual);
}
