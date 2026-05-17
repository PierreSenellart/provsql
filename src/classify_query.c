/**
 * @file classify_query.c
 * @brief Query-time TID / BID / OPAQUE classifier.
 *
 * Invoked by @c provsql_planner on the top-level @c Query when the
 * @c provsql.classify_top_level GUC is on.  Emits a @c NOTICE carrying
 * the certified kind and the set of provenance-tracked base relations
 * the query touches.
 *
 * Initial scope : a flat @c fromlist of @c RangeTblRefs, no
 * @c SubLinks, no modifying @c CTEs, no set operations, and either
 * zero or one provenance-tracked base relations reached either
 * directly (@c RTE_RELATION) or through any depth of subqueries
 * (@c RTE_SUBQUERY -- view bodies after rewriting and inline
 * subqueries in the @c FROM list).  Everything else is reported as
 * OPAQUE.  See @c doc/TODO/safe-query-followups.md for the follow-up
 * extensions.
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

  /* Shape gate at this level.  A FROM-less @c SELECT (e.g.
   * @c SELECT @c 1) keeps the gate open because there are no
   * sub-structures to inspect; it ends up trivially TID at the
   * outer level. */
  if (q->hasSubLinks
      || q->hasModifyingCTE
      || q->cteList != NIL
      || q->setOperations != NULL)
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

void provsql_classify_query(Query *q, ProvSQLClassification *out) {
  bool shape_ok   = true;
  int  n_meta     = 0;
  Oid  sole_relid = InvalidOid;

  out->kind          = PROVSQL_TABLE_OPAQUE;
  out->source_relids = NIL;

  if (q == NULL || q->commandType != CMD_SELECT)
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

  provsql_notice("%s", buf.data);
  pfree(buf.data);
}
