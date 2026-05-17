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
 * The hierarchical-CQ class and its read-once decomposability are the
 * "safe queries" of Dalvi and Suciu, "The Dichotomy of Probabilistic
 * Inference for Unions of Conjunctive Queries", J. ACM 59(6), 2012
 * (doi:10.1145/2395116.2395119) ; the dichotomy theorem in that paper
 * is the theoretical foundation for the rewrite this file implements.
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
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
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
 *    sublinks, or top-level set operations.  Top-level UCQs
 *    (UNION / EXCEPT / INTERSECT) are processed branch-by-branch by
 *    the planner's recursive @c process_query, so each branch reaches
 *    this gate on its own and the outer set-operation node bails here.
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
    return false;                       /* UCQ branches handled by
                                         * recursive process_query
                                         * re-entry, not here */
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

    /* Metadata gate.
     *
     * - No provsql column on the relation: accepted as deterministic,
     *   probability-1 tuples (every row behaves as if it carried a
     *   gate_one() leaf, so read-once factoring is unaffected).
     *
     * - provsql column present but no metadata entry: refuse.  This
     *   covers CREATE TABLE AS SELECT, ALTER TABLE ADD COLUMN
     *   provsql, and ALTER TABLE RENAME ... TO provsql -- in all
     *   three the relation has a column ProvSQL would honour at
     *   evaluation time, but the column's content never passed
     *   through add_provenance / repair_key, so independence cannot
     *   be assumed.
     *
     * - provsql column present and metadata says OPAQUE: refuse
     *   (set_table_info, or a provenance_guard fire after a user-
     *   supplied INSERT / UPDATE).
     *
     * - provsql column present and metadata says TID or BID: accept.
     *   The BID block-key alignment check happens in the rewriter
     *   once the root variable is known. */
    {
      AttrNumber provsql_attno = get_attnum(rte->relid, PROVSQL_COLUMN_NAME);
      bool has_provsql_col =
        provsql_attno != InvalidAttrNumber
        && get_atttype(rte->relid, provsql_attno) == constants->OID_TYPE_UUID;
      bool has_meta = provsql_lookup_table_info(rte->relid, &info);

      if (has_provsql_col && !has_meta)
        return false;
      if (has_meta && info.kind == PROVSQL_TABLE_OPAQUE)
        return false;
    }
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
  bool        is_constant_pinned; ///< Reserved for future §1 follow-up work; currently never set (§1 constant-pinned atoms are routed through the multi-component path before this struct is built, so each atom in @c rewrite_hierarchical_cq is unconditionally a regular hierarchical-component atom).
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
 * @brief Recognise @c OpExpr nodes of shape @c Var @c = @c Const.
 *
 * Mirrors @c safe_is_var_equality but matches an equality between a
 * base-level @c Var and a planner-time @c Const literal (in either argument
 * order), again stripping @c RelabelType wrappers to see through binary-
 * coercion casts.  This is the recogniser used by the §1 constant-selection
 * elimination pass (Dalvi & Suciu 2007 §5.1, induced FD @c ∅ @c → @c R.a)
 * to flag union-find roots as pinned to a literal.  Volatile predicates
 * never reach this point: @c safe_split_quals routes them to the residual,
 * and the constant-selection scan walks both @c per_atom and the residual
 * but stops at any @c OpExpr that is not a plain @c Var/@c Const equality.
 *
 * On match, @p *var receives the base-level @c Var and @p *konst the
 * literal; on no match, returns @c false without touching them.  NULL
 * constants (@c constisnull) do not yield an FD -- equality to NULL is
 * never satisfied in standard SQL semantics, so the planner would have
 * already short-circuited the row at executor time, but a @c Var @c = @c
 * NULL conjunct is still SQL-legal and we conservatively reject it as
 * non-FD-inducing.
 */
static bool safe_is_var_const_equality(Expr *qual, Var **var, Const **konst) {
  OpExpr *op;
  Node   *ln, *rn;
  Var    *v;
  Const  *k;
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
  if (IsA(ln, Var) && IsA(rn, Const)) {
    v = (Var *) ln;
    k = (Const *) rn;
  } else if (IsA(ln, Const) && IsA(rn, Var)) {
    k = (Const *) ln;
    v = (Var *) rn;
  } else {
    return false;
  }
  if (v->varlevelsup != 0)
    return false;
  if (k->constisnull)
    return false;
  expected = find_equality_operator(v->vartype, k->consttype);
  if (expected == InvalidOid || op->opno != expected)
    return false;
  *var   = v;
  *konst = k;
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
#if PG_VERSION_NUM >= 130000
      /* PG 13+ keeps a parallel @c varnosyn / @c varattnosyn for
       * @c ruleutils.c-style query deparsing.  Updating only
       * @c varno here leaves @c varnosyn pointing at the outer atom's
       * (now-stale) rtindex; @c pg_get_querydef then dereferences a
       * Var whose syntactic-rtindex slot resolves through a different
       * RTE than the semantic one, recurses into that RTE's
       * subquery, and (because the syntactic dereference always finds
       * its way back to the same Var) stack-overflows.  Mirror the
       * semantic remap on the syntactic side. */
      nv->varnosyn = 1;
#endif
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
 *  - no root variable within the (single) connected component.
 *    Disconnected components are handled upstream in
 *    @c try_safe_query_rewrite via @c rewrite_multi_component, so
 *    this bail covers only the "connected but no variable touches
 *    every atom" case ;
 *  - an atom whose root binding spans more than one column.  The
 *    rewrite would have to push an intra-atom equality (e.g.
 *    @c A(x,x) when @c x is the root) into the inner subquery ;
 *    the current rewriter does not synthesise such an equality and
 *    bails ;
 *  - a Var in @p quals or @c q->targetList that does not fit any of
 *    the slot kinds the rewriter knows how to expose : the
 *    fully-covered class (extra outer slot), a partial-coverage
 *    class whose atom landed in an inner group (inner-group slot),
 *    or a single-atom head Var on an atom whose wrap can carry an
 *    extra projection slot.  Body-only Vars on a single-atom class
 *    and partial-coverage classes that the multi-group / bridge
 *    merger cannot route to any group fall outside this set and
 *    trigger the bail.
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
  Bitmapset *bridging_classes = NULL; /* repr indices of partial-coverage classes whose touched atoms span more than one group; the bridge variable becomes an extra slot on the first_member of each touched group, and the outer's residual WHERE re-equates the groups' columns through the standard Var remap. */
  /* §2 PK / NOT-NULL UNIQUE FD support.  @c determined_in[c*natoms+j]
   * @c == @c true means class @c c is functionally determined inside
   * RTE @c j (some key whose every column's class is anchored on @c j
   * exists in @c j's relation, and @c c is anchored on @c j by a
   * non-key column).  @c class_atom_count_fd[c] is the FD-aware
   * coverage of class @c c: the count of atoms where @c c is anchored
   * @em and @em not FD-determined.  @c fd_aware_mode triggers when no
   * single class is fully covered by the raw (non-FD) atom count but
   * the FD-aware atom-sets satisfy the textbook pairwise nested-or-
   * disjoint hierarchicality condition; in that mode the rewriter
   * uses a per-atom local anchor class (the lowest-repr class
   * anchored on the atom) instead of a global root, and each atom's
   * @c proj_slots holds one slot for every class anchored on it. */
  bool   *determined_in = NULL;
  int    *class_atom_count_fd = NULL;
  bool    fd_aware_mode = false;
  int    *atom_anchor_class = NULL; /* per atom (size natoms): repr of the class chosen as that atom's local "root" in fd_aware_mode */
  int     i, j;
  Index   varno;
  bool    ok;

  *groups_out = NIL;
  /* The §1 constant-selection elimination is handled upstream by
   * @c apply_constant_selection_fd_pass, so @p quals already has
   * the redundant within-class equijoins dropped by the time this
   * function is reached. */

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

  /* §2 -- Dalvi & Suciu 2007 §5.1 induced FDs from PRIMARY KEYs and
   * NOT-NULL UNIQUE constraints.  For each base relation in the FROM
   * list, look up its keys via the per-backend cache; for every key
   * @c K every of whose columns is anchored on the relation (i.e.
   * appears in the query as a Var of that RTE), mark every class
   * anchored on the same RTE by a non-key column as @em FD-determined
   * within that RTE.  The intuition: under the key, each non-key
   * column is a function of the key's columns, so the class
   * containing the non-key column does not contribute an independent
   * existential to the relation -- exactly the FD-aware atom-set
   * reduction §2 prescribes.
   *
   * Skipped relations:
   *
   *   - @c RTE_RELATION entries whose @c relid does not yield any
   *     PRIMARY KEY / NOT-NULL UNIQUE through
   *     @c provsql_lookup_relation_keys (no FD to apply);
   *   - non-@c RTE_RELATION entries (subqueries, joins): the
   *     candidate gate has already rejected these via the shape
   *     check at the top of @c is_safe_query_candidate. */
  determined_in =
      palloc0((size_t) nvars * (size_t) natoms * sizeof(bool));
#define DETERMINED(c, atom_idx) determined_in[(c) * natoms + (atom_idx)]

  for (j = 0; j < natoms; j++) {
    RangeTblEntry         *rte = (RangeTblEntry *) list_nth(q->rtable, j);
    ProvenanceRelationKeys keys;
    uint16                 ki;
    if (rte->rtekind != RTE_RELATION)
      continue;
    if (!provsql_lookup_relation_keys(rte->relid, &keys))
      continue;
    for (ki = 0; ki < keys.key_n; ki++) {
      const ProvenanceRelationKey *key = &keys.keys[ki];
      bool                         all_anchored = true;
      uint16                       kc;
      int                          vi;
      /* Every column of the key must be present in the query @em and
       * its class must be multi-atom -- i.e. an equijoin link binds
       * the column to a Var on some other RTE.  A key column in a
       * singleton class is a "free body existential" that ranges over
       * every value; under such a free column the FD @c K @c → @c A
       * does not reduce @c A's atom-set (a different free-column
       * value would give a different @c A, so @c A is not truly
       * determined within the RTE).  This is the composite-PK
       * soundness trap of TODO §2: "partial match (some PK columns
       * equated, others not) does not give the FD". */
      for (kc = 0; kc < key->col_n; kc++) {
        int idx = safe_var_index(vctx.vars,
                                 (Index) (j + 1),
                                 key->cols[kc]);
        if (idx < 0) {
          all_anchored = false;
          break;
        }
        if (class_atom_count[cls[idx]] < 2
            || class_atom_count[cls[idx]] > natoms) {
          all_anchored = false;
          break;
        }
      }
      if (!all_anchored)
        continue;
      /* Apply: every Var on this RTE whose attno is NOT in the key
       * has its class flagged as FD-determined within @c j. */
      for (vi = 0; vi < nvars; vi++) {
        Var *vp = vars_arr[vi];
        bool is_key_col = false;
        if (vp->varno != (Index) (j + 1))
          continue;
        for (kc = 0; kc < key->col_n; kc++) {
          if (vp->varattno == key->cols[kc]) {
            is_key_col = true;
            break;
          }
        }
        if (is_key_col)
          continue;
        DETERMINED(cls[vi], j) = true;
      }
    }
  }

  /* §3 -- deterministic-relation transparency (Gatterbauer & Suciu
   * 2015 dissociation framework; see TODO §3).  A relation that is
   * not provenance-tracked (no @c provsql column @em and no metadata
   * entry in the per-table cache) contributes probability-1 tuples:
   * dissociating tuples in a deterministic relation does not change
   * the query's probability, so the relation is structurally
   * transparent -- it filters the cross product but adds nothing to
   * atom-set membership.  We model that by marking every union-find
   * class as FD-determined within the deterministic RTE, reusing
   * the @c DETERMINED matrix the §2 PK-FD pass already populates;
   * the existing @c fd_aware_mode then drops the deterministic atom
   * from each class's @c atoms_fd and the pairwise hierarchicality
   * check accepts star-schema queries that the raw atom-count check
   * would refuse.
   *
   * Soundness guards (TODO §3 + coordination with the Part 2
   * correlation registry):
   *
   *  - @c rte->rtekind @c == @c RTE_RELATION : excluded by the
   *    candidate gate already.
   *  - @c has_provsql_col @c == @c false : the relation has no
   *    @c provsql @c uuid column at all.  A provsql column with no
   *    metadata entry, or an OPAQUE-tagged provsql column, was
   *    rejected by the candidate gate at @c is_safe_query_candidate;
   *    this branch never sees those.
   *  - @c pg_class.relkind @c == @c RELKIND_RELATION : exclude views
   *    (@c 'v' / @c 'm'), foreign tables (@c 'f'), partitioned
   *    parents (@c 'p'), composite types, etc.  A view's body might
   *    transitively reference the same probabilistic atoms as the
   *    outer query, breaking the dissociation argument; the safe
   *    rule is to refuse view descent here and revisit once the
   *    @c safe-query-followups.md correlation registry materialises.
   *  - No @c pg_inherits parent : an inheritance child shares its
   *    parent's storage in PG; tagging it transparent could overlook
   *    correlated rows in the parent.  Refuse conservatively.
   *
   * The CTAS-correlation trap (manual @c CREATE @c TABLE @c foo
   * @c AS @c SELECT @c FROM @c <tracked>) is NOT caught by these
   * guards -- the resulting @c foo has @c relkind @c = @c 'r' and no
   * provsql column.  Until the correlation registry tracks ancestry,
   * the trap remains documented in the TODO; users who deliberately
   * strip @c provsql from a CTAS take on the responsibility. */
  {
    ProvenanceTableInfo info;
    for (j = 0; j < natoms; j++) {
      RangeTblEntry *rte = (RangeTblEntry *) list_nth(q->rtable, j);
      AttrNumber     provsql_attno;
      bool           has_provsql_col;
      bool           has_meta;
      HeapTuple      class_tup;
      Form_pg_class  classform;
      bool           ok_relkind;
      if (rte->rtekind != RTE_RELATION)
        continue;
      provsql_attno = get_attnum(rte->relid, PROVSQL_COLUMN_NAME);
      has_provsql_col =
        provsql_attno != InvalidAttrNumber
        && get_atttype(rte->relid, provsql_attno) == constants->OID_TYPE_UUID;
      has_meta = provsql_lookup_table_info(rte->relid, &info);
      if (has_provsql_col || has_meta)
        continue;                       /* probabilistic / OPAQUE atom */

      class_tup =
        SearchSysCache1(RELOID, ObjectIdGetDatum(rte->relid));
      if (!HeapTupleIsValid(class_tup))
        continue;
      classform = (Form_pg_class) GETSTRUCT(class_tup);
      ok_relkind = (classform->relkind == RELKIND_RELATION);
      ReleaseSysCache(class_tup);
      if (!ok_relkind)
        continue;

      if (has_superclass(rte->relid))
        continue;                       /* inheritance child */

      /* All guards passed: mark every class FD-determined inside @c j.
       * The existing atom-set construction then excludes @c j from
       * each class's @c atoms_fd, and the pairwise hierarchicality
       * check sees the reduced sets. */
      for (i = 0; i < nvars; i++) {
        if (cls[i] != i)
          continue;
        DETERMINED(i, j) = true;
      }
    }
  }

  /* FD-aware atom counts: how many atoms does each class touch that
   * are not FD-determining the class.  Mirrors @c class_atom_count
   * but excludes the FD-pinned entries. */
  class_atom_count_fd = palloc0(nvars * sizeof(int));
  for (i = 0; i < nvars; i++) {
    int c;
    if (cls[i] != i)
      continue;
    if (class_atom_count[i] > natoms)
      continue;                       /* sentinel, leave at 0 */
    c = i;
    for (j = 0; j < natoms; j++) {
      if (ANCHOR(c, j) != 0 && !DETERMINED(c, j))
        class_atom_count_fd[c]++;
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

  /* §2 fallback: no class touches every atom under the raw count, but
   * the FD-aware atom-sets might still satisfy pairwise nested-or-
   * disjoint hierarchicality.  Concretely, we accept the textbook
   * H-query under a PK on the middle atom (Dalvi & Suciu 2007 §5.1
   * @c R(x),S(x,y),T(y) with PK on @c S.x): after the FD reduction
   * @c atoms(B) drops to @c {T}, leaving @c {R,S} and @c {T} as
   * disjoint atom-sets covering every atom.  In that case there is no
   * global root, but the rewrite still works: each atom is wrapped in
   * a flat @c SELECT @c DISTINCT exposing every class anchored on it
   * as a separate slot, and the outer's residual equijoins resolve
   * through @c safe_remap_vars_mutator on the matching slot's
   * @c base_attno.
   *
   * Conditions for entering @c fd_aware_mode:
   *
   *  1. The standard root-class check failed.
   *  2. Every atom in @c q->rtable has at least one class anchored on
   *     it (no orphan atoms -- otherwise the rewriter would have no
   *     @c provsql column to multiply into the cross product for that
   *     atom; the existing @c root_anchor_attno check enforces this
   *     under a global root, and we re-enforce it here).
   *  3. Every pair of multi-atom classes (count >= 2 under the raw
   *     count) has FD-aware atom-sets that are nested or disjoint.
   *     Singleton classes (count @c == @c 1) are tolerated -- they
   *     surface as single-atom-head Vars in the @em existing
   *     in-targetlist path further down.
   *  4. No class has the @c natoms+1 sentinel (intra-atom equalities
   *     across two columns of the same atom remain unsupported here,
   *     same as in the non-FD path).
   *  5. The query has no @em raw partial-coverage classes whose
   *     FD-aware count is still in @c [2, natoms-1].  Such classes
   *     would normally route through the multi-level / inner-group
   *     path, which is not adapted to per-atom anchors yet; the §2
   *     fd-aware mode therefore demands every multi-atom class to
   *     either cover all atoms (raw root, handled above) or to land
   *     on a disjoint pair-block via the FD reduction. */
  if (root_class < 0) {
    bool eligible = true;
    /* Sentinel and orphan-atom checks. */
    for (i = 0; i < nvars && eligible; i++) {
      if (cls[i] != i)
        continue;
      if (class_atom_count[i] > natoms) {
        eligible = false;
        break;
      }
    }
    if (eligible) {
      bool *atom_covered = palloc0(natoms * sizeof(bool));
      for (i = 0; i < nvars; i++) {
        int c = cls[i];
        Index vn = vars_arr[i]->varno;
        if (vn < 1 || (int) vn > natoms)
          continue;
        if (class_atom_count[c] > natoms)
          continue;                     /* sentinel */
        atom_covered[vn - 1] = true;
      }
      for (j = 0; j < natoms; j++) {
        if (!atom_covered[j]) {
          eligible = false;
          break;
        }
      }
      pfree(atom_covered);
    }
    if (eligible) {
      /* Pairwise nested-or-disjoint on FD-aware atom-sets, considering
       * only multi-atom classes (singletons stay as in-targetlist head
       * Vars and don't constrain pairwise hierarchicality). */
      Bitmapset **atoms_fd = palloc0(nvars * sizeof(Bitmapset *));
      int        *class_reprs = palloc(nvars * sizeof(int));
      int         nreprs = 0;
      for (i = 0; i < nvars && eligible; i++) {
        if (cls[i] != i)
          continue;
        if (class_atom_count[i] > natoms)
          continue;
        if (class_atom_count[i] < 2)
          continue;                     /* singleton; ignored here */
        for (j = 0; j < natoms; j++) {
          if (ANCHOR(i, j) != 0 && !DETERMINED(i, j))
            atoms_fd[i] = bms_add_member(atoms_fd[i], j);
        }
        class_reprs[nreprs++] = i;
      }
      for (i = 0; i < nreprs && eligible; i++) {
        int k;
        for (k = i + 1; k < nreprs && eligible; k++) {
          Bitmapset *a = atoms_fd[class_reprs[i]];
          Bitmapset *b = atoms_fd[class_reprs[k]];
          bool nested = bms_is_subset(a, b) || bms_is_subset(b, a);
          bool disjoint = !bms_overlap(a, b);
          if (!nested && !disjoint) {
            eligible = false;
            break;
          }
        }
      }
      for (i = 0; i < nvars; i++)
        if (atoms_fd[i])
          bms_free(atoms_fd[i]);
      pfree(atoms_fd);
      pfree(class_reprs);
    }
    if (eligible) {
      /* Per-atom local anchor: the lowest-repr class anchored on the
       * atom that the outer residual most naturally joins on.  Two
       * passes:
       *
       *   1. FD-aware preference -- pick a class that anchors on the
       *      atom @em and is not FD-determined there.  This is the
       *      §2 case: under PK on @c S.x, class @c {S.y, T.y} drops
       *      its @c S anchor for atom-set purposes, so @c S's local
       *      root should be the @c {R.x, S.x} class instead.
       *   2. Fallback -- atoms with every anchored class FD-determined
       *      (the §3 deterministic-relation case: every class is
       *      tagged determined inside the deterministic atom) still
       *      need a slot column for the outer's residual equijoin
       *      to resolve through.  Use the first anchored class
       *      regardless of FD status.  The DISTINCT wrap on the slot
       *      column collapses duplicate keys so each Sales token
       *      still appears once across the cross product, preserving
       *      read-once. */
      atom_anchor_class = palloc(natoms * sizeof(int));
      for (j = 0; j < natoms; j++)
        atom_anchor_class[j] = -1;
      for (i = 0; i < nvars; i++) {
        int c;
        if (cls[i] != i)
          continue;
        if (class_atom_count[i] > natoms)
          continue;
        if (class_atom_count[i] < 2)
          continue;
        c = i;
        for (j = 0; j < natoms; j++) {
          if (ANCHOR(c, j) != 0 && !DETERMINED(c, j)
              && atom_anchor_class[j] < 0)
            atom_anchor_class[j] = c;
        }
      }
      for (j = 0; j < natoms; j++) {
        if (atom_anchor_class[j] >= 0)
          continue;
        for (i = 0; i < nvars; i++) {
          if (cls[i] != i)
            continue;
          if (class_atom_count[i] < 2 || class_atom_count[i] > natoms)
            continue;
          if (ANCHOR(i, j) != 0) {
            atom_anchor_class[j] = i;
            break;
          }
        }
      }
      /* An atom with no multi-atom anchor at all (e.g. only singleton-
       * class head Vars touch it) cannot be wrapped in
       * @c fd_aware_mode -- it would need a join key from the outer's
       * residual that no shared class provides.  Bail. */
      for (j = 0; j < natoms; j++) {
        if (atom_anchor_class[j] < 0) {
          eligible = false;
          break;
        }
      }
    }
    if (eligible) {
      fd_aware_mode = true;
    } else {
      /* The @c bail block below pfrees @c determined_in,
       * @c class_atom_count_fd and @c atom_anchor_class itself; just
       * jump there. */
      goto bail;
    }
  }

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

  /* In @c fd_aware_mode every atom is an outer wrap (no inner groups);
   * the partial-coverage path below is bypassed, since the FD-reduced
   * atom-sets are by construction pairwise nested-or-disjoint and the
   * per-atom anchor in @c atom_anchor_class already encodes the
   * single-level wrap structure. */
  if (fd_aware_mode)
    goto skip_partial_coverage;

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
      /* Disjoint multi-group: partition atoms by exact signature,
       * then merge bridging-connected groups.  A partial-coverage
       * class whose touched atoms span more than one group is a
       * "bridge"; rather than threading bridge-join columns through
       * the outer, we collapse every chain of bridging-connected
       * groups into one super-group.  The recursive @c process_query
       * re-entry on the super-group's inner sub-Query then handles
       * the intra-super-group structure (the bridging class becomes
       * a fully-covered class inside the inner, and the residual
       * partial classes peel level by level).  Each super-group
       * still becomes one outer @c RTE_SUBQUERY, joined with the
       * others only on the root variable -- so the resulting
       * circuit is read-once over independent components. */
      Bitmapset **group_sigs;
      int ngroups = 0;
      int g;

      /* Assign group_id by signature equality, in order of first
       * appearance to keep the rewriter output deterministic. */
      group_sigs = palloc0(natoms * sizeof(Bitmapset *));
      for (j = 0; j < natoms; j++) {
        bool found = false;
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

      /* Identify bridging classes: partial-coverage classes whose
       * touched atoms span more than one group. */
      for (i = 0; i < nvars; i++) {
        int c = cls[i];
        Bitmapset *touched_groups = NULL;
        int jj;
        if (c != i)
          continue;
        if (class_atom_count[c] < 2 || class_atom_count[c] >= natoms)
          continue;
        for (jj = 0; jj < natoms; jj++) {
          if (ANCHOR(c, jj) != 0)
            touched_groups = bms_add_member(touched_groups,
                                            atom_group[jj]);
        }
        if (bms_num_members(touched_groups) > 1)
          bridging_classes = bms_add_member(bridging_classes, c);
        bms_free(touched_groups);
      }

      /* Merge bridging-connected groups via union-find.  After
       * merging, renumber super-groups densely starting from 0 and
       * rewrite @c atom_group accordingly. */
      if (!bms_is_empty(bridging_classes)) {
        int *parent = palloc(ngroups * sizeof(int));
        int *super = palloc(ngroups * sizeof(int));
        int next_super = 0;
        int c;

        for (g = 0; g < ngroups; g++) {
          parent[g] = g;
          super[g] = -1;
        }

        c = -1;
        while ((c = bms_next_member(bridging_classes, c)) >= 0) {
          int first_g = -1;
          int jj;
          for (jj = 0; jj < natoms; jj++) {
            int gj, ra, rb;
            if (ANCHOR(c, jj) == 0)
              continue;
            gj = atom_group[jj];
            if (first_g < 0) {
              first_g = gj;
              continue;
            }
            /* Path-compressed find. */
            ra = first_g;
            while (parent[ra] != ra) ra = parent[ra];
            rb = gj;
            while (parent[rb] != rb) rb = parent[rb];
            if (ra != rb)
              parent[rb] = ra;
          }
        }

        for (g = 0; g < ngroups; g++) {
          int r = g;
          while (parent[r] != r) r = parent[r];
          if (super[r] < 0)
            super[r] = next_super++;
          super[g] = super[r];
        }

        for (j = 0; j < natoms; j++) {
          if (atom_group[j] >= 0)
            atom_group[j] = super[atom_group[j]];
        }

        pfree(parent);
        pfree(super);
        bms_free(bridging_classes);
        bridging_classes = NULL;
      }
    }

    for (j = 0; j < natoms; j++) bms_free(sig[j]);
    pfree(sig);
  }

skip_partial_coverage:

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
   * slot would leak into the outer scope with no wrap to host it.
   *
   * In @c fd_aware_mode (§2 multi-anchor), every multi-atom class is
   * exposed as a slot on every atom it anchors -- so any Var of a
   * multi-atom class is guaranteed a matching slot in its atom's
   * @c proj_slots regardless of FD-determined status.  The check
   * simplifies to "either Var's class touches >= 2 atoms (slot built
   * below) or Var's class is a singleton with the Var in the
   * targetList (head-Var slot built below)". */
  for (i = 0; i < nvars; i++) {
    int c = cls[i];
    int atom_idx = (int) vars_arr[i]->varno - 1;
    if (fd_aware_mode) {
      if (class_atom_count[c] >= 2 && class_atom_count[c] <= natoms)
        continue;
      if (class_atom_count[c] == 1 && in_targetlist[i])
        continue;
      if (provsql_verbose >= 30)
        provsql_notice("safe-query rewriter (§2): Var (varno=%u, varattno=%d) "
                       "belongs to a class with no outer slot",
                       (unsigned) vars_arr[i]->varno,
                       (int) vars_arr[i]->varattno);
      ok = false;
      break;
    }
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
    int local_root = fd_aware_mode ? atom_anchor_class[j] : root_class;

    sa->rtindex      = (Index) (j + 1);
    sa->proj_slots   = NIL;
    sa->pushed_quals = NIL;
    sa->group_id     = atom_group[j];
    sa->outer_rtindex = 0;
    sa->inner_rtindex = 0;
    sa->is_constant_pinned = false;
    sa->root_anchor_attno = (AttrNumber) ANCHOR(local_root, j);
    if (sa->root_anchor_attno == 0)
      goto bail;                          /* impossible if root truly covers all */

    root_slot = palloc(sizeof(safe_proj_slot));
    root_slot->base_attno  = sa->root_anchor_attno;
    root_slot->class_id    = local_root;
    root_slot->outer_attno = 1;
    sa->proj_slots = lappend(sa->proj_slots, root_slot);
    for (i = 0; i < nvars; i++) {
      safe_proj_slot *slot;
      if (cls[i] != i || i == local_root)
        continue;
      if (fd_aware_mode) {
        /* §2 mode: expose every multi-atom class anchored on this
         * atom, irrespective of FD-determined status -- the slot is
         * needed for the outer's residual equijoin to resolve via
         * @c safe_remap_vars_mutator.  Singleton classes still go
         * through the head-Var path below. */
        if (class_atom_count[i] < 2 || class_atom_count[i] > natoms)
          continue;
      } else if (class_atom_count[i] != natoms) {
        continue;                         /* partial-coverage handled via groups */
      }
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

    /* Synthesize intra-group equalities for every fully-covered
     * class touching two or more atoms of the same group.  The user
     * typically writes such equalities transitively
     * (e.g. @c a.x=b.x @c AND @c a.x=c.x), and the outer's residual
     * partitioning routes each transitive conjunct to the outer
     * because its varnos span groups.  Without an explicit
     * @c b.x=c.x conjunct landing in the group's @c inner_quals,
     * the recursive @c process_query re-entry on the inner
     * sub-Query would see @c b.x and @c c.x as unrelated columns,
     * leaving @c c.x out of @c proj_slots -- the inner sub-Query
     * for @c c would then aggregate over @em every value of @c x
     * instead of the per-row @c x, and the resulting circuit would
     * over-count.  We add the missing equalities here as @c OpExpr
     * nodes in original-varno space (the existing inner-build
     * machinery remaps them to inner varnos). */
    for (g = 0; g <= max_gid; g++) {
      for (i = 0; i < nvars; i++) {
        ListCell  *mlc;
        safe_rewrite_atom *first_touching = NULL;
        AttrNumber first_attno = 0;
        Oid        first_type = InvalidOid;
        int32      first_typmod = -1;
        Oid        first_coll = InvalidOid;
        int        c = cls[i];
        if (c != i)
          continue;
        if (class_atom_count[c] != natoms)
          continue;                         /* only fully-covered */
        foreach (mlc, arr[g]->member_atoms) {
          safe_rewrite_atom *m = (safe_rewrite_atom *) lfirst(mlc);
          int           atom_idx = (int) m->rtindex - 1;
          AttrNumber    attno = ANCHOR(c, atom_idx);
          RangeTblEntry *rte;
          HeapTuple      atttup;
          Form_pg_attribute attform;
          Oid            mtype, mcoll, eqop, eqfunc;
          int32          mtypmod;
          Var           *lv, *rv;
          OpExpr        *eq;
          if (attno == 0)
            continue;
          rte = (RangeTblEntry *) list_nth(q->rtable, atom_idx);
          atttup = SearchSysCache2(ATTNUM,
                                   ObjectIdGetDatum(rte->relid),
                                   Int16GetDatum(attno));
          if (!HeapTupleIsValid(atttup))
            continue;
          attform = (Form_pg_attribute) GETSTRUCT(atttup);
          mtype   = attform->atttypid;
          mtypmod = attform->atttypmod;
          mcoll   = attform->attcollation;
          ReleaseSysCache(atttup);
          if (first_touching == NULL) {
            first_touching = m;
            first_attno    = attno;
            first_type     = mtype;
            first_typmod   = mtypmod;
            first_coll     = mcoll;
            continue;
          }
          eqop = find_equality_operator(first_type, mtype);
          if (!OidIsValid(eqop))
            continue;
          eqfunc = get_opcode(eqop);
          if (!OidIsValid(eqfunc))
            continue;
          lv = makeVar(first_touching->rtindex, first_attno,
                       first_type, first_typmod, first_coll, 0);
          rv = makeVar(m->rtindex, attno, mtype, mtypmod, mcoll, 0);
          eq = makeNode(OpExpr);
          eq->opno         = eqop;
          eq->opfuncid     = eqfunc;
          eq->opresulttype = BOOLOID;
          eq->opretset     = false;
          eq->opcollid     = InvalidOid;
          eq->inputcollid  = first_coll;
          eq->args         = list_make2(lv, rv);
          eq->location     = -1;
          arr[g]->inner_quals =
              lappend(arr[g]->inner_quals, eq);
        }
      }
    }

    for (g = 0; g <= max_gid; g++)
      *groups_out = lappend(*groups_out, arr[g]);
    pfree(arr);
  }

#undef ANCHOR
#undef DETERMINED

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
  if (determined_in)
    pfree(determined_in);
  if (class_atom_count_fd)
    pfree(class_atom_count_fd);
  if (atom_anchor_class)
    pfree(atom_anchor_class);
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
  if (determined_in)
    pfree(determined_in);
  if (class_atom_count_fd)
    pfree(class_atom_count_fd);
  if (atom_anchor_class)
    pfree(atom_anchor_class);
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
  bool  bail;    ///< Set when a Var has no slot in its atom's projection;
                 ///< the caller aborts the rewrite and falls back to the
                 ///< default pipeline rather than emitting a broken plan.
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
        /* Head/qual Var on a grouped atom that no shared-class slot
         * covers: the rewrite cannot produce a column the outer query
         * can reference, but the input SQL is still valid -- bail to
         * the default pipeline rather than raising. */
        ctx->bail = true;
        return (Node *) v;
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
        /* Same situation, outer-wrap atom: bail instead of raising. */
        ctx->bail = true;
        return (Node *) v;
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
   * group's inner sub-Query at output column 1; constant-pinned atoms
   * expose only the synthesised anchor (the per-atom @c pushed_quals
   * are already AND-injected into the inner subquery by
   * @c safe_build_inner_wrap), so a Var referencing a pinned atom
   * here would have no slot to resolve to -- the residual-cleanup
   * pass should have dropped any such Var.  If one slips through,
   * @c safe_remap_vars_mutator's pinned-atom branch raises @c bail
   * and the rewriter falls back to the regular pipeline. */
  mctx.atoms = atoms;
  mctx.groups = groups;
  mctx.bail = false;
  outer->targetList = (List *)
      safe_remap_vars_mutator((Node *) outer->targetList, &mctx);
  if (outer->jointree && outer->jointree->quals)
    outer->jointree->quals =
        safe_remap_vars_mutator(outer->jointree->quals, &mctx);

  if (group_emitted)
    pfree(group_emitted);

  /* A Var with no projection slot means the rewrite cannot honour the
   * outer query without inventing an output column for it (e.g. a
   * GROUP BY column on a grouped atom whose value is not shared
   * across the group's other members).  Bail to the regular pipeline:
   * the input SQL is still valid, the rewrite just does not apply. */
  if (mctx.bail) {
    if (provsql_verbose >= 30)
      provsql_notice("safe-query rewrite bailed: a Var has no projection "
                     "slot in its (grouped or outer-wrap) atom");
    return NULL;
  }

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
  bool    bail;                    ///< set when a Var has no exposed inner column; caller falls back to the regular pipeline
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
      if (inner_attno == 0) {
        /* Same bailout pattern as safe_remap_vars_mutator: signal the
         * caller to abandon the multi-component rewrite rather than
         * raise on a valid input the rewriter just cannot handle. */
        ctx->bail = true;
        return (Node *) v;
      }
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
    tctx.bail                = false;
    outer->targetList = (List *) safe_outer_te_remap_mutator(
        (Node *) outer->targetList, &tctx);
    if (outer->jointree->quals)
      outer->jointree->quals =
          safe_outer_te_remap_mutator(outer->jointree->quals, &tctx);
    if (tctx.bail) {
      if (provsql_verbose >= 30)
        provsql_notice("safe-query multi-component rewrite bailed: a Var "
                       "has no exposed column in its component's inner "
                       "sub-Query");
      pfree(inner_queries);
      pfree(inner_quals);
      return NULL;
    }
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
 * @brief §1 constant-selection elimination pre-pass.
 *
 * Implements Dalvi & Suciu 2007 §5.1's induced-FD construction
 * (@c ∅ @c → @c R.a from a @c R.a @c = @c c conjunct), specialised
 * to the safe-query rewriter's representation:
 *
 *  - Build a Var-level union-find from the equijoin conjuncts in
 *    @p *residual_in_out.  Every pair of Vars that share an
 *    equijoin (transitively, through the closure) lands in the same
 *    equivalence class.
 *  - Scan @p per_atom_quals[i] (atom-local conjuncts) and
 *    @p *residual_in_out (cross-atom conjuncts) for @c Var @c = @c
 *    Const matches.  Mark the matched Var's class repr as constant-
 *    pinned, recording one of the literals for propagation.
 *  - For every Var in a constant-pinned class, synthesise the
 *    corresponding @c Var @c = @c const conjunct on the Var's atom's
 *    @p per_atom_quals list (when not already present, dedup'd by
 *    @c (varno,varattno)).  After this step every atom touching the
 *    class carries the local filter, so the standard atom-local
 *    pushdown path materialises it in the wrap.
 *  - Drop top-level @c AND conjuncts of @p *residual_in_out whose
 *    every base-level Var is in a constant-pinned class.  These are
 *    the equijoin conjuncts that brought constant atoms together
 *    (e.g. @c R.x @c = @c S.x under @c S.x @c = @c 42); after
 *    propagation each side carries its own @c Var @c = @c const
 *    filter, so the original equijoin is redundant and would only
 *    prevent the rewriter from resolving columns the constant-pinned
 *    atoms' wraps no longer project.
 *
 * Effect on the rest of @c try_safe_query_rewrite: with cross-atom
 * equijoin links to constant-pinned atoms removed, those atoms
 * become their own connected components, and the existing
 * multi-component path in @c try_safe_query_rewrite handles them by
 * emitting a separate inner sub-Query per component.  The recursive
 * @c process_query re-entry then collapses each constant-pinned
 * atom to a single aggregated @c gate_plus token, while the
 * remaining atoms keep the standard single-component hierarchical
 * shape.  This is the read-once factoring §1 prescribes -- the
 * pinned atom's contribution factors out as an independent
 * @c gate_times child of the result.
 */
static void apply_constant_selection_fd_pass(Query *q, List **per_atom_quals,
                                             Node **residual_in_out) {
  int     natoms = list_length(q->rtable);
  safe_collect_vars_ctx vctx = { NIL };
  List   *eq_pairs = NIL;
  Var   **vars_arr;
  int    *cls;
  int     nvars;
  int     i;
  ListCell *lc;
  bool   *is_constant_class;
  Const **class_const_value;
  List   *all_const_conjuncts = NIL;

  if (natoms < 2)
    return;

  /* Collect distinct base-level Vars from targetList, residual,
   * and every per-atom-quals list.  All of these may carry the
   * Vars whose classes the equijoin closure will merge. */
  expression_tree_walker((Node *) q->targetList,
                         safe_collect_vars_walker, &vctx);
  if (*residual_in_out)
    expression_tree_walker(*residual_in_out,
                           safe_collect_vars_walker, &vctx);
  if (per_atom_quals != NULL) {
    int j;
    for (j = 0; j < natoms; j++) {
      ListCell *qlc;
      foreach (qlc, per_atom_quals[j])
        expression_tree_walker((Node *) lfirst(qlc),
                               safe_collect_vars_walker, &vctx);
    }
  }
  nvars = list_length(vctx.vars);
  if (nvars == 0)
    return;

  vars_arr = palloc(nvars * sizeof(Var *));
  cls      = palloc(nvars * sizeof(int));
  i = 0;
  foreach (lc, vctx.vars) {
    vars_arr[i] = (Var *) lfirst(lc);
    cls[i] = i;
    i++;
  }

  /* Union-find on residual equijoin conjuncts. */
  if (*residual_in_out)
    safe_collect_equalities(*residual_in_out, &eq_pairs);
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

  /* Scan per_atom + residual for @c Var @c = @c Const conjuncts;
   * mark the matched Var's class as constant-pinned. */
  is_constant_class = palloc0(nvars * sizeof(bool));
  class_const_value = palloc0(nvars * sizeof(Const *));
  if (per_atom_quals != NULL) {
    int j;
    for (j = 0; j < natoms; j++) {
      ListCell *qlc;
      foreach (qlc, per_atom_quals[j])
        all_const_conjuncts = lappend(all_const_conjuncts, lfirst(qlc));
    }
  }
  if (*residual_in_out)
    safe_flatten_and(*residual_in_out, &all_const_conjuncts);

  {
    ListCell *qlc;
    foreach (qlc, all_const_conjuncts) {
      Expr  *e = (Expr *) lfirst(qlc);
      Var   *v;
      Const *k;
      int    idx, root;
      if (!safe_is_var_const_equality(e, &v, &k))
        continue;
      idx = safe_var_index(vctx.vars, v->varno, v->varattno);
      if (idx < 0)
        continue;
      root = cls[idx];
      if (!is_constant_class[root]) {
        is_constant_class[root] = true;
        class_const_value[root] = k;
      }
    }
  }
  list_free(all_const_conjuncts);

  /* Propagate: for every Var in a constant-pinned class, ensure
   * @c Var @c = @c const sits in the Var's atom's pushdown list. */
  if (per_atom_quals != NULL) {
    for (i = 0; i < nvars; i++) {
      int    root = cls[i];
      Var   *vp   = vars_arr[i];
      Const *k    = class_const_value[root];
      int    atom_idx;
      bool   already = false;
      ListCell *qlc;
      OpExpr  *new_op;
      Oid      eqop;
      Var     *v_existing;
      Const   *k_existing;
      if (!is_constant_class[root] || k == NULL)
        continue;
      if (vp->varno < 1 || (int) vp->varno > natoms)
        continue;
      atom_idx = (int) vp->varno - 1;
      foreach (qlc, per_atom_quals[atom_idx]) {
        if (safe_is_var_const_equality((Expr *) lfirst(qlc),
                                       &v_existing, &k_existing)
            && v_existing->varno == vp->varno
            && v_existing->varattno == vp->varattno) {
          already = true;
          break;
        }
      }
      if (already)
        continue;
      eqop = find_equality_operator(vp->vartype, k->consttype);
      if (eqop == InvalidOid)
        continue;
      new_op = (OpExpr *) makeNode(OpExpr);
      new_op->opno         = eqop;
      new_op->opfuncid     = InvalidOid;
      new_op->opresulttype = BOOLOID;
      new_op->opretset     = false;
      new_op->opcollid     = InvalidOid;
      new_op->inputcollid  = vp->varcollid;
      new_op->args         = list_make2(copyObject(vp), copyObject(k));
      new_op->location     = -1;
      per_atom_quals[atom_idx] =
          lappend(per_atom_quals[atom_idx], new_op);
    }
  }

  /* Drop residual conjuncts whose every Var is in a constant-pinned
   * class: those equijoins are now redundant (each side carries its
   * own propagated @c Var @c = @c const filter). */
  if (*residual_in_out != NULL) {
    List     *conjuncts = NIL;
    List     *kept = NIL;
    ListCell *qlc;
    safe_flatten_and(*residual_in_out, &conjuncts);
    foreach (qlc, conjuncts) {
      Node *cj = (Node *) lfirst(qlc);
      safe_collect_vars_ctx cv = { NIL };
      ListCell *vlc;
      bool all_constant = true;
      bool any_var = false;
      expression_tree_walker(cj, safe_collect_vars_walker, &cv);
      foreach (vlc, cv.vars) {
        Var *v = (Var *) lfirst(vlc);
        int idx = safe_var_index(vctx.vars, v->varno, v->varattno);
        any_var = true;
        if (idx < 0 || !is_constant_class[cls[idx]]) {
          all_constant = false;
          break;
        }
      }
      list_free(cv.vars);
      if (any_var && all_constant)
        continue;
      kept = lappend(kept, cj);
    }
    if (kept == NIL)
      *residual_in_out = NULL;
    else if (list_length(kept) == 1)
      *residual_in_out = (Node *) linitial(kept);
    else
      *residual_in_out = (Node *) makeBoolExpr(AND_EXPR, kept, -1);
    list_free(conjuncts);
  }

  pfree(is_constant_class);
  pfree(class_const_value);
  pfree(vars_arr);
  pfree(cls);
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

  /* §1 constant-selection elimination pre-pass.  Identifies
   * union-find classes pinned to a literal by some @c Var @c =
   * @c Const conjunct, propagates the literal to every Var in the
   * class (atom-local synthesised conjuncts), and drops the redundant
   * cross-atom equijoins.  The multi-component dispatch immediately
   * below then sees constant-pinned atoms as separate components and
   * routes them through the existing per-component subquery shape,
   * which produces the read-once @c gate_times factoring §1 needs
   * (each pinned atom becomes its own @c gate_plus child of the top
   * @c gate_times). */
  apply_constant_selection_fd_pass(q, per_atom, &residual);

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

  /* Attach per-atom pushed conjuncts to the rewrite descriptors.  The
   * §1 constant-selection pre-pass above may have appended
   * synthesised @c Var @c = @c const conjuncts to some atoms' lists
   * (the propagated literals from constant-pinned classes); they
   * follow the same atom-local pushdown path as user-written
   * single-atom conjuncts and end up in the inner DISTINCT wrap's
   * @c WHERE. */
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
