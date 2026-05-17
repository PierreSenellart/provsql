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
 * Everything else is reported as OPAQUE.  See
 * @c doc/TODO/safe-query-followups.md for the follow-up
 * extensions (independent-TID join inference, BID block-key
 * preservation, the correlation registry).
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
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
      Node *n = (Node *) lfirst(lc);
      if (!IsA(n, RangeTblRef)) {
        *shape_ok = false;
        break;
      }
    }
  }

  /* Walk the range table.  RTE_RELATION entries with metadata are
   * collected as sources; @c RTE_SUBQUERY recurses (view bodies and
   * inline subqueries) so the underlying base relations join the
   * accumulator; the PG 18 virtual @c RTE_GROUP is skipped
   * transparently.  Any other @c rtekind (@c RTE_JOIN, @c RTE_VALUES,
   * @c RTE_CTE, @c RTE_FUNCTION, ...) trips the shape gate so we
   * conservatively report OPAQUE. */
  foreach (lc, q->rtable) {
    RangeTblEntry      *rte = (RangeTblEntry *) lfirst(lc);
    ProvenanceTableInfo info;

    if (rte->rtekind == RTE_RELATION) {
      if (provsql_lookup_table_info(rte->relid, &info)) {
        out->source_relids = lappend_oid(out->source_relids, rte->relid);
        *sole_relid = rte->relid;
        (*n_meta)++;
      }
    } else if (rte->rtekind == RTE_GROUP) {
      /* PG 18 virtual entry; skip without invalidating the shape. */
    } else if (rte->rtekind == RTE_SUBQUERY) {
      /* Descend.  The same shape gate is applied to the inner
       * @c Query : a @c SubLink or set operation inside the
       * subquery propagates opacity to the outer level, while the
       * visible inner @c RTE_RELATION sources are still added to
       * the accumulator for diagnostic purposes. */
      classify_walk(rte->subquery, out, shape_ok, n_meta, sole_relid);
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
 * it in @c provsql_table_info -- both belong with the CTAS-tag-
 * inheritance / correlation-registry slices.  See
 * @c doc/TODO/safe-query-followups.md.
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
    if (provsql_lookup_table_info(sole_relid, &info))
      out->kind = (provsql_table_kind) info.kind;
    /* If the lookup races and disappears between the two calls,
     * fall back to OPAQUE. */
  } else {
    /* Multiple tracked sources : joins are deferred to a later
     * slice (independent-TID join inference). */
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
