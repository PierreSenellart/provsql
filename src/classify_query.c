/**
 * @file classify_query.c
 * @brief Query-time TID / BID / OPAQUE classifier.
 *
 * Invoked by @c provsql_planner on the top-level @c Query when the
 * @c provsql.classify_top_level GUC is on.  Emits a @c NOTICE carrying
 * the certified kind and the set of provenance-tracked base relations
 * the query touches.
 *
 * Scope :
 *
 *  - Single-source classification : a flat @c fromlist of
 *    @c RangeTblRefs, with no kind-altering features (@c SubLinks,
 *    modifying @c CTEs, @c cteList, @c DISTINCT, @c GROUP BY,
 *    @c HAVING, aggregates, window functions, set-returning
 *    functions in the target list).  Either zero or one
 *    provenance-tracked base relations are reached either directly
 *    (@c RTE_RELATION) or through any depth of subqueries
 *    (@c RTE_SUBQUERY -- view bodies after PG rewriting and inline
 *    @c FROM subqueries).  The recorded kind of the sole tracked
 *    source (TID / BID / OPAQUE) is preserved verbatim ; zero
 *    tracked sources is trivially TID.
 *  - @c UNION @c ALL specialisation : a fully-@c UNION-ALL tree of
 *    leg subqueries each of which classifies independently as TID
 *    over a base-relid set that is disjoint from every other leg's.
 *    The union is then TID with the cumulative source list.
 *    Anything that doesn't fit (@c INTERSECT, @c EXCEPT,
 *    @c UNION @c DISTINCT, mixed kinds, overlapping leg sources)
 *    falls back to OPAQUE.
 *
 * Everything else is reported as OPAQUE.  Independent-TID join
 * inference, BID block-key preservation under projection and
 * @c GROUP @c BY, and the per-relation base-ancestor registry the
 * disjointness check consults all live in this same file ; see
 * the helpers below.
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "classify_query.h"
#include "provsql_utils.h"

/** @brief Backing storage for the @c provsql.classify_top_level GUC. */
bool provsql_classify_top_level = false;

/** @brief Map a @c provsql_table_kind to its uppercase user-facing label. */
static const char *kind_label(provsql_table_kind k) {
  switch (k) {
    case PROVSQL_TABLE_TID:    return "TID";
    case PROVSQL_TABLE_BID:    return "BID";
    case PROVSQL_TABLE_OPAQUE: return "OPAQUE";
  }
  return "?";
}

/**
 * @brief Decide whether a @c jointree fromlist entry has a shape the
 *        classifier can certify : a plain @c RangeTblRef, or a
 *        @c JoinExpr with @c jointype @c == @c JOIN_INNER whose
 *        @c larg and @c rarg recursively satisfy the same predicate.
 *
 * ANSI-syntax inner joins (@c INNER @c JOIN, @c CROSS @c JOIN; PG
 * normalises both to @c JOIN_INNER) preserve TID per-row independence
 * because every output row corresponds to exactly one pair of source
 * rows -- the row's provenance is the AND of the two tokens.  Outer
 * joins (@c LEFT / @c RIGHT / @c FULL) introduce NULL-padding rows
 * whose provenance is the negation of the inner-match disjunction, so
 * they break the per-row TID property and stay OPAQUE.  Semi / anti
 * joins are also rejected (the planner uses @c JOIN_SEMI / @c JOIN_ANTI
 * for sublink-driven joins, which our @c hasSubLinks shape gate has
 * already filtered out upstream, but the explicit check here keeps
 * the predicate self-contained).
 */
static bool classify_fromlist_shape_ok(Node *n) {
  if (n == NULL)
    return false;
  if (IsA(n, RangeTblRef))
    return true;
  if (IsA(n, JoinExpr)) {
    JoinExpr *je = (JoinExpr *) n;
    if (je->jointype != JOIN_INNER)
      return false;
    return classify_fromlist_shape_ok(je->larg)
        && classify_fromlist_shape_ok(je->rarg);
  }
  return false;
}

/**
 * @brief Recursive walker shared by the top-level entry point and the
 *        @c RTE_SUBQUERY descent.
 *
 * The shape gate, source enumeration, and recursion live here so the
 * outer entry point can decide TID / BID / OPAQUE from the cumulative
 * @c n_meta / @c sole_relid pair after the whole tree has been
 * walked.  Tracked @c RTE_RELATION entries reachable through any
 * depth of @c RTE_SUBQUERY (view bodies after PG rewriting, inline
 * @c FROM-clause subqueries) contribute to the accumulator.
 *
 * The recursion is stack-bounded by the SQL parser's own nesting
 * limit ; no explicit depth cap is needed at this layer.
 */
static void classify_walk(Query                 *q,
                          ProvSQLClassification *out,
                          bool                  *shape_ok,
                          int                   *n_meta,
                          Oid                   *sole_relid) {
  ListCell *lc;

  if (q == NULL || q->commandType != CMD_SELECT) {
    *shape_ok = false;
    return;
  }

  /* Shape gate at this level.  Anything that turns row lineage
   * into a composite (aggregates, GROUP BY, HAVING, DISTINCT,
   * window functions, SRFs in the target list) breaks the
   * per-row independent-atom property TID demands, so we refuse
   * to certify.  Hidden subqueries, modifying CTEs, named CTEs,
   * and set operations are conservative rejects from the original
   * scope (set operations have a dedicated UNION ALL path higher
   * up the dispatcher).  A FROM-less @c SELECT (e.g.
   * @c SELECT @c 1) keeps the gate open because there are no
   * sub-structures to inspect; it ends up trivially TID at the
   * outer level. */
  if (q->hasSubLinks
      || q->hasModifyingCTE
      || q->cteList != NIL
      || q->setOperations != NULL
      || q->distinctClause != NIL
      || q->groupClause != NIL
      || q->groupingSets != NIL
      || q->havingQual != NULL
      || q->hasAggs
      || q->hasWindowFuncs
      || q->hasTargetSRFs)
    *shape_ok = false;

  if (*shape_ok && q->jointree != NULL && q->jointree->fromlist != NIL) {
    foreach (lc, q->jointree->fromlist) {
      if (!classify_fromlist_shape_ok((Node *) lfirst(lc))) {
        *shape_ok = false;
        break;
      }
    }
  }

  /* Walk the range table.  RTE_RELATION entries with metadata are
   * collected as sources; @c RTE_SUBQUERY recurses (view bodies and
   * inline subqueries) so the underlying base relations join the
   * accumulator.  @c RTE_JOIN entries (one per @c JoinExpr in the
   * fromlist) are synthetic union-aliases over the join's combined
   * column list ; they carry no source on their own and pass through.
   * The fromlist shape gate above already constrains them to
   * @c JOIN_INNER, so seeing one here does not change the
   * per-row independence story.  Any other @c rtekind (@c RTE_VALUES,
   * @c RTE_CTE, @c RTE_FUNCTION, the PG 18 synthetic @c RTE_GROUP,
   * ...) trips the shape gate so we conservatively report OPAQUE.
   * @c RTE_GROUP appears alongside the user RTEs when @c q->groupClause
   * is non-empty -- our shape gate already rejects @c groupClause
   * upstream, so by the time control reaches this loop on a GROUP BY
   * query @c *shape_ok is already false and the catch-all here is
   * just re-asserting it without enumerating @c RTE_GROUP as a
   * source.  Treating it as a generic non-source rtekind keeps the
   * file source-compatible across PG 12-18+ without a version
   * guard. */
  foreach (lc, q->rtable) {
    RangeTblEntry      *rte = (RangeTblEntry *) lfirst(lc);
    ProvenanceTableInfo info;

    if (rte->rtekind == RTE_RELATION) {
      if (provsql_lookup_table_info(rte->relid, &info)) {
        out->source_relids = lappend_oid(out->source_relids, rte->relid);
        *sole_relid = rte->relid;
        (*n_meta)++;
      }
    } else if (rte->rtekind == RTE_SUBQUERY) {
      /* Descend.  The same shape gate is applied to the inner
       * @c Query : a @c SubLink or set operation inside the
       * subquery propagates opacity to the outer level, while the
       * visible inner @c RTE_RELATION sources are still added to
       * the accumulator for diagnostic purposes. */
      classify_walk(rte->subquery, out, shape_ok, n_meta, sole_relid);
    } else if (rte->rtekind == RTE_JOIN) {
      /* Synthetic RTE for a JoinExpr; the underlying source RTEs
       * appear separately in the same rtable.  No-op here. */
    } else {
      *shape_ok = false;
    }
  }
}

/**
 * @brief Walk a @c SetOperationStmt tree, collecting each leaf
 *        leg's @c Query body into @c legs.
 *
 * The tree is a binary structure : interior @c SetOperationStmt
 * nodes carry the operator and an @c all flag, leaf @c RangeTblRefs
 * point at @c RTE_SUBQUERY entries in @c parent->rtable whose
 * @c subquery field is the leg's parsed @c Query.  Returns false
 * if any interior node is not a @c UNION @c ALL or any leaf is not
 * a subquery RTE, so that the dispatcher can fall back to OPAQUE
 * (only fully-UNION-ALL trees qualify for the TID promotion).
 */
static bool collect_union_all_legs(Node *node, Query *parent,
                                   List **legs) {
  if (node == NULL)
    return false;
  if (IsA(node, RangeTblRef)) {
    int            rtindex = ((RangeTblRef *) node)->rtindex;
    RangeTblEntry *rte;
    if (rtindex < 1 || rtindex > list_length(parent->rtable))
      return false;
    rte = (RangeTblEntry *) list_nth(parent->rtable, rtindex - 1);
    if (rte->rtekind != RTE_SUBQUERY || rte->subquery == NULL)
      return false;
    *legs = lappend(*legs, rte->subquery);
    return true;
  }
  if (IsA(node, SetOperationStmt)) {
    SetOperationStmt *s = (SetOperationStmt *) node;
    if (s->op != SETOP_UNION || !s->all)
      return false;
    return collect_union_all_legs(s->larg, parent, legs)
        && collect_union_all_legs(s->rarg, parent, legs);
  }
  return false;
}

/**
 * @brief Promote a fully-UNION-ALL @c Query to TID when each leg
 *        classifies as TID and the leg source-relid sets are
 *        pairwise disjoint.
 *
 * Returns true and populates @c out with TID + the cumulative
 * source list on success ; returns false to let the dispatcher
 * fall through to the conservative OPAQUE path on any failure
 * (non-UNION-ALL operator, a leg that classifies as BID/OPAQUE,
 * or overlapping leg sources -- the gate-level atoms of a relid
 * that appears in two legs are not disjoint, so the multiset
 * union no longer satisfies the TID property).
 *
 * Pairwise disjointness is checked at the relid level only.  A
 * future correlation registry will refine this by also rejecting
 * legs whose base ancestors overlap (e.g. two views sharing the
 * same underlying TID table), which the syntactic check cannot
 * detect.
 *
 * BID legs are deliberately not promoted, even when every leg is
 * BID with the same block-key column projected at the same target-
 * list position.  The UNION ALL output's "rows with the same
 * block-key value" set spans both legs, but a row from leg A and a
 * row from leg B sharing a block-key value are independent (the
 * legs are different relations), not mutually exclusive.  Calling
 * the result BID under that column would falsely advertise an
 * invariant the rows don't satisfy.  Recovering BID-ness would
 * require either certifying disjoint block-key values between
 * legs (not knowable from the query text) or emitting a synthetic
 * composite block key @c (leg_id, k) in the output and recording
 * it in @c provsql_table_info ; both paths are documented as
 * conservative-by-design in the safe-query follow-ups.
 */
static bool try_classify_union_all(Query *q,
                                   ProvSQLClassification *out) {
  List     *legs = NIL;
  List     *seen = NIL;
  ListCell *lc;

  if (q->setOperations == NULL || !IsA(q->setOperations, SetOperationStmt))
    return false;

  if (!collect_union_all_legs(q->setOperations, q, &legs)) {
    list_free(legs);
    return false;
  }

  foreach (lc, legs) {
    Query                 *leg_query = (Query *) lfirst(lc);
    ProvSQLClassification  leg;
    ListCell              *src_lc;

    provsql_classify_query(leg_query, &leg);
    if (leg.kind != PROVSQL_TABLE_TID) {
      list_free(leg.source_relids);
      list_free(seen);
      list_free(legs);
      return false;
    }
    foreach (src_lc, leg.source_relids) {
      Oid relid = lfirst_oid(src_lc);
      if (list_member_oid(seen, relid)) {
        list_free(leg.source_relids);
        list_free(seen);
        list_free(legs);
        return false;
      }
      seen = lappend_oid(seen, relid);
    }
    list_free(leg.source_relids);
  }
  list_free(legs);

  out->kind          = PROVSQL_TABLE_TID;
  out->source_relids = seen;
  return true;
}

/**
 * @brief Conservative multi-source promotion: when every tracked
 *        source in @p out->source_relids is TID and the registered
 *        ancestor sets are pairwise disjoint, promote the
 *        classification to TID.
 *
 * Mirrors the disjointness check the safe-query rewriter runs in
 * @c is_safe_query_candidate, just at the classifier layer so a
 * multi-source query no longer collapses to OPAQUE before the
 * rewriter even sees it.  The hierarchical-CQ structure is NOT
 * inspected here -- the rewriter runs the full check downstream,
 * and the classifier's job is only to certify the per-row
 * independence the user-visible NOTICE pill advertises.
 *
 * Returns @c true and sets @c out->kind on success.  On failure
 * (any source non-TID, no registry entry, or any pair of ancestor
 * sets overlapping), returns @c false and leaves @p out unchanged
 * so the caller falls through to OPAQUE.
 */
static bool try_classify_multi_source_tid(ProvSQLClassification *out) {
  ListCell *lc;
  int       i, j, n;
  uint16   *anc_n;
  Oid     (*anc)[PROVSQL_TABLE_INFO_MAX_ANCESTORS];

  n = list_length(out->source_relids);
  if (n < 2)
    return false;

  anc_n = palloc0(n * sizeof(uint16));
  anc   = palloc(n * sizeof(*anc));

  i = 0;
  foreach (lc, out->source_relids) {
    Oid                 relid = lfirst_oid(lc);
    ProvenanceTableInfo info;
    if (!provsql_lookup_table_info(relid, &info)
        || info.kind != PROVSQL_TABLE_TID) {
      pfree(anc);
      pfree(anc_n);
      return false;
    }
    if (!provsql_lookup_ancestry(relid, &anc_n[i], anc[i])) {
      /* Defensive : add_provenance / repair_key seed {self} eagerly,
       * so a tracked relation should always have a non-empty
       * registry entry.  If somehow missing, fall back to {self}
       * (matches the safe-query rewriter's same fallback). */
      anc[i][0] = relid;
      anc_n[i]  = 1;
    }
    i++;
  }

  for (i = 0; i < n; i++)
    for (j = i + 1; j < n; j++) {
      uint16 a, b;
      for (a = 0; a < anc_n[i]; a++)
        for (b = 0; b < anc_n[j]; b++)
          if (anc[i][a] == anc[j][b]) {
            pfree(anc);
            pfree(anc_n);
            return false;
          }
    }

  pfree(anc);
  pfree(anc_n);
  out->kind = PROVSQL_TABLE_TID;
  return true;
}

/**
 * @brief Resolve a base-level (@p varno, @p attno) pair in @p q
 *        transitively through @c RTE_SUBQUERY layers until reaching
 *        the underlying @c RTE_RELATION column.  Returns @c true on
 *        success and writes the base relid / base attno to
 *        @p out_relid / @p out_attno.  Returns @c false when any
 *        intermediate TLE is not a plain @c Var (possibly through
 *        @c RelabelType wrappers), when an outer-scope reference is
 *        hit, or when the chain ends on a non-relation rtekind.
 */
static bool resolve_var_to_base(Query *q, Index varno, AttrNumber attno,
                                Oid *out_relid, AttrNumber *out_attno) {
  RangeTblEntry *rte;
  if (q == NULL || varno < 1
      || (int) varno > list_length(q->rtable))
    return false;
  rte = (RangeTblEntry *) list_nth(q->rtable, varno - 1);
  if (rte->rtekind == RTE_RELATION) {
    *out_relid = rte->relid;
    *out_attno = attno;
    return true;
  }
  if (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL) {
    Query       *sub = rte->subquery;
    TargetEntry *te;
    Node        *e;
    Var         *v;
    ListCell    *lc;
    /* Match by resno : a TLE may carry a Var resjunk-tagged or
     * reordered, so scan rather than @c list_nth blindly. */
    te = NULL;
    foreach (lc, sub->targetList) {
      TargetEntry *t = (TargetEntry *) lfirst(lc);
      if (t->resno == attno && !t->resjunk) {
        te = t;
        break;
      }
    }
    if (te == NULL)
      return false;
    e = (Node *) te->expr;
    while (e != NULL && IsA(e, RelabelType))
      e = (Node *) ((RelabelType *) e)->arg;
    if (e == NULL || !IsA(e, Var))
      return false;
    v = (Var *) e;
    if (v->varlevelsup != 0)
      return false;
    return resolve_var_to_base(sub, v->varno, v->varattno,
                               out_relid, out_attno);
  }
  return false;
}

/**
 * @brief Decide whether every block-key column of @p info survives
 *        in @p q's target list -- resolved transitively through
 *        @c RTE_SUBQUERY descents so the same check works on
 *        @c SELECT @c k @c FROM @c bid_t and on
 *        @c SELECT @c k @c FROM @c (SELECT @c k @c FROM @c bid_t).
 *        Renamed projections (@c SELECT @c k @c AS @c b ...) still
 *        count as preserving -- the match is on the underlying
 *        @c Var, not the output column's name.  @c resjunk entries
 *        in the outer @c targetList are ignored.
 */
static bool bid_block_key_preserved(Query *q, Oid source_relid,
                                    const ProvenanceTableInfo *info) {
  for (uint16 i = 0; i < info->block_key_n; ++i) {
    AttrNumber  bk     = info->block_key[i];
    bool        found  = false;
    ListCell   *lc;
    foreach (lc, q->targetList) {
      TargetEntry *te = (TargetEntry *) lfirst(lc);
      Node        *e  = (Node *) te->expr;
      Var         *v;
      Oid          base_relid;
      AttrNumber   base_attno;
      if (te->resjunk)
        continue;
      while (e != NULL && IsA(e, RelabelType))
        e = (Node *) ((RelabelType *) e)->arg;
      if (e == NULL || !IsA(e, Var))
        continue;
      v = (Var *) e;
      if (v->varlevelsup != 0)
        continue;
      if (!resolve_var_to_base(q, v->varno, v->varattno,
                               &base_relid, &base_attno))
        continue;
      if (base_relid == source_relid && base_attno == bk) {
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }
  return true;
}

/**
 * @brief PG 18+ helper: when @p q has a synthetic @c RTE_GROUP
 *        entry (set @c parseCheckAggregates() appends it for every
 *        @c GROUP @c BY query), Vars in @c groupClause's TLE
 *        expressions point at the @c RTE_GROUP rather than the
 *        underlying source.  Resolve them through the @c RTE_GROUP's
 *        @c groupexprs list so the BID-block-key check below sees
 *        the source Var.  No-op on PG &lt; 18 and on queries without
 *        @c hasGroupRTE.
 */
static Node *resolve_through_group_rte(Query *q, Node *e) {
#if PG_VERSION_NUM >= 180000
  ListCell *lc;
  Index     group_rtindex = 0;
  Index     idx = 1;
  List     *groupexprs = NIL;
  Var      *v;

  if (e == NULL || !q->hasGroupRTE)
    return e;
  foreach (lc, q->rtable) {
    RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
    if (rte->rtekind == RTE_GROUP) {
      group_rtindex = idx;
      groupexprs    = rte->groupexprs;
      break;
    }
    idx++;
  }
  if (group_rtindex == 0)
    return e;

  while (e != NULL && IsA(e, RelabelType))
    e = (Node *) ((RelabelType *) e)->arg;
  if (e == NULL || !IsA(e, Var))
    return e;
  v = (Var *) e;
  if (v->varlevelsup != 0 || v->varno != group_rtindex)
    return e;
  if (v->varattno < 1 || v->varattno > list_length(groupexprs))
    return e;
  return (Node *) list_nth(groupexprs, v->varattno - 1);
#else
  (void) q;
  return e;
#endif
}

/**
 * @brief Pre-dispatch special case for @c GROUP @c BY on a single
 *        BID source's block-key columns.
 *
 * The generic shape gate rejects @c groupClause @c != @c NIL up
 * front, but @c SELECT @c k @c FROM @c bid_t @c GROUP @c BY @c k
 * (and the multi-column-key generalisation) has a well-defined
 * per-row provenance : each output row's @c block_key value
 * uniquely identifies one BID block, and the OR over that block's
 * mulinput slots reduces to the block's key token (an independent
 * @c gate_input).  So the output is per-row independent -- TID --
 * with the cumulative source list narrowed to the single BID
 * source.
 *
 * Conservative: requires no aggregates / window functions /
 * sublinks / SRFs / CTEs / set operations / HAVING / DISTINCT /
 * sortClause-with-side-effects, no @c LIMIT / @c OFFSET, a flat
 * fromlist of exactly one @c RangeTblRef pointing at a BID
 * @c RTE_RELATION, and a @c groupClause whose resolved Vars match
 * the source's block-key set exactly (no extra columns, no
 * missing ones).  When all met, returns @c true with
 * @p out populated.  Any failure leaves @p out untouched ; the
 * caller proceeds to the generic dispatcher path.
 */
static bool try_classify_groupby_block_key(Query *q,
                                           ProvSQLClassification *out) {
  RangeTblRef        *rtr;
  RangeTblEntry      *rte;
  ProvenanceTableInfo info;
  Bitmapset          *resolved = NULL;
  ListCell           *lc;

  if (q->groupClause == NIL)
    return false;
  if (q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs
      || q->hasSubLinks || q->hasModifyingCTE || q->hasDistinctOn)
    return false;
  if (q->cteList != NIL || q->groupingSets != NIL
      || q->havingQual != NULL || q->setOperations != NULL
      || q->distinctClause != NIL)
    return false;
  if (q->jointree == NULL
      || list_length(q->jointree->fromlist) != 1)
    return false;
  if (!IsA(linitial(q->jointree->fromlist), RangeTblRef))
    return false;
  rtr = (RangeTblRef *) linitial(q->jointree->fromlist);
  if (rtr->rtindex < 1 || rtr->rtindex > list_length(q->rtable))
    return false;
  rte = (RangeTblEntry *) list_nth(q->rtable, rtr->rtindex - 1);
  if (rte->rtekind != RTE_RELATION)
    return false;
  if (!provsql_lookup_table_info(rte->relid, &info))
    return false;
  if (info.kind != PROVSQL_TABLE_BID)
    return false;
  if (info.block_key_n == 0)
    return false;  /* whole-table BID : "GROUP BY {}" doesn't exist */

  foreach (lc, q->groupClause) {
    SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
    TargetEntry     *te;
    Node            *e;
    Var             *v;
    bool             attno_in_block_key = false;
    uint16           i;
    te = get_sortgroupclause_tle(sgc, q->targetList);
    if (te == NULL) { bms_free(resolved); return false; }
    e = (Node *) te->expr;
    /* On PG 18+, grouped Vars in the targetList point at the
     * synthetic @c RTE_GROUP entry ; resolve through its
     * @c groupexprs list back to the source-relation Var. */
    e = resolve_through_group_rte(q, e);
    while (e != NULL && IsA(e, RelabelType))
      e = (Node *) ((RelabelType *) e)->arg;
    if (e == NULL || !IsA(e, Var)) { bms_free(resolved); return false; }
    v = (Var *) e;
    if (v->varlevelsup != 0 || v->varno != rtr->rtindex) {
      bms_free(resolved); return false;
    }
    for (i = 0; i < info.block_key_n; ++i)
      if (info.block_key[i] == v->varattno) {
        attno_in_block_key = true; break;
      }
    if (!attno_in_block_key) { bms_free(resolved); return false; }
    resolved = bms_add_member(resolved, v->varattno);
  }
  /* Each block-key column must appear exactly once in groupClause. */
  for (uint16 i = 0; i < info.block_key_n; ++i)
    if (!bms_is_member(info.block_key[i], resolved)) {
      bms_free(resolved); return false;
    }
  bms_free(resolved);

  out->kind          = PROVSQL_TABLE_TID;
  out->source_relids = list_make1_oid(rte->relid);
  return true;
}

void provsql_classify_query(Query *q, ProvSQLClassification *out) {
  bool shape_ok   = true;
  int  n_meta     = 0;
  Oid  sole_relid = InvalidOid;

  out->kind          = PROVSQL_TABLE_OPAQUE;
  out->source_relids = NIL;

  if (q == NULL || q->commandType != CMD_SELECT)
    return;

  /* UNION ALL specialisation : a fully-UNION-ALL tree of subquery
   * legs each TID over a relid set disjoint from the other legs
   * promotes to TID with the cumulative source list.  Anything
   * else (INTERSECT, EXCEPT, UNION DISTINCT, mixed kinds,
   * overlapping leg sources) falls through to @c classify_walk,
   * which trips the shape gate on @c q->setOperations != NULL and
   * reports OPAQUE while still enumerating visible sources for
   * diagnostics. */
  if (q->setOperations != NULL && try_classify_union_all(q, out))
    return;

  /* GROUP BY on a single BID source's block-key columns reduces
   * the output to one row per block ; each row's provenance
   * collapses to the block's key token (an independent input
   * gate), so the result is TID.  Handled as a pre-dispatch
   * special case because the generic shape gate refuses
   * @c groupClause @c != @c NIL up front. */
  if (try_classify_groupby_block_key(q, out))
    return;

  classify_walk(q, out, &shape_ok, &n_meta, &sole_relid);

  if (!shape_ok) {
    /* Conservative : when we cannot fully see the query, we cannot
     * certify TID-ness even if the visible RTEs carry no metadata --
     * a hidden subquery might pull in correlated rows. */
    out->kind = PROVSQL_TABLE_OPAQUE;
  } else if (n_meta == 0) {
    /* Fully visible and no provenance-tracked source : the result is
     * deterministic, hence trivially TID. */
    out->kind = PROVSQL_TABLE_TID;
  } else if (n_meta == 1) {
    ProvenanceTableInfo info;
    if (provsql_lookup_table_info(sole_relid, &info)) {
      if (info.kind == PROVSQL_TABLE_BID) {
        /* BID : the output is BID iff every block-key column of the
         * source survives in the outer target list (matched by Var
         * resolution through any @c RTE_SUBQUERY descent, not by
         * output column name).  Otherwise the mutually-exclusive
         * partitioning the user could observe is lost -- downgrade
         * to OPAQUE.  Whole-table BID (@c block_key_n @c == @c 0)
         * is trivially preserved. */
        if (info.block_key_n == 0
            || bid_block_key_preserved(q, sole_relid, &info))
          out->kind = PROVSQL_TABLE_BID;
        else
          out->kind = PROVSQL_TABLE_OPAQUE;
      } else {
        out->kind = (provsql_table_kind) info.kind;
      }
    }
    /* If the lookup races and disappears between the two calls,
     * fall back to OPAQUE. */
  } else {
    /* Multiple tracked sources : promote to TID when every source
     * is TID and the registered ancestor sets are pairwise
     * disjoint.  Any failure (non-TID source, missing registry
     * entry, ancestor overlap) leaves @c out->kind at OPAQUE,
     * which is the conservative default already set above. */
    if (!try_classify_multi_source_tid(out))
      out->kind = PROVSQL_TABLE_OPAQUE;
  }
}

void provsql_classify_emit_notice(const ProvSQLClassification *c) {
  StringInfoData buf;
  ListCell      *lc;
  bool           first = true;

  initStringInfo(&buf);
  appendStringInfo(&buf, "query result is %s", kind_label(c->kind));

  /* TID / BID : the source list is complete and tells the user which
   * tracked relations contribute the per-row uncertainty.  OPAQUE :
   * we deliberately omit sources -- when the shape gate trips on a
   * sublink, a set operation, GROUP BY, etc., the rtable walk only
   * reaches the syntactically visible sources, so the list would
   * be partial and misleadingly suggest completeness.  The user
   * already has the query text in front of them and can identify
   * which relations are involved without our help. */
  if (c->kind != PROVSQL_TABLE_OPAQUE) {
    if (c->source_relids == NIL) {
      appendStringInfoString(&buf, " (no provenance-tracked sources)");
    } else {
      appendStringInfoString(&buf, " (sources: ");
      foreach (lc, c->source_relids) {
        Oid   relid   = lfirst_oid(lc);
        char *nspname = get_namespace_name(get_rel_namespace(relid));
        char *relname = get_rel_name(relid);

        if (!first)
          appendStringInfoString(&buf, ", ");
        first = false;

        if (nspname != NULL && relname != NULL)
          appendStringInfo(&buf, "%s.%s",
                           quote_identifier(nspname),
                           quote_identifier(relname));
        else if (relname != NULL)
          appendStringInfoString(&buf, quote_identifier(relname));
        else
          appendStringInfo(&buf, "<oid %u>", relid);
      }
      appendStringInfoChar(&buf, ')');
    }
  }

  provsql_notice("%s", buf.data);
  pfree(buf.data);
}
