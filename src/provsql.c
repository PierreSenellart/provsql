/**
 * @file provsql.c
 * @brief PostgreSQL planner hook for transparent provenance tracking.
 *
 * This file installs a @c planner_hook that intercepts every SELECT query
 * and rewrites it to propagate a provenance circuit token (UUID) alongside
 * normal result tuples.  The rewriting proceeds in three conceptual phases:
 *
 *  -# **Discovery** – scan the range table for relations/subqueries that
 *     already carry a @c provsql UUID column (@c get_provenance_attributes).
 *  -# **Expression building** – combine the discovered tokens according
 *     to the semiring operation that corresponds to the SQL operator in use
 *     (⊗ for joins, ⊕ for duplicate elimination, ⊖ for EXCEPT) and wrap
 *     aggregations (@c make_provenance_expression,
 *     @c make_aggregation_expression).
 *  -# **Splice** – append the resulting provenance expression to the target
 *     list and replace any explicit @c provenance() call in the query with
 *     the computed expression (@c add_to_select,
 *     @c replace_provenance_function_by_expression).
 */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pg_config.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_class.h"           /* RELKIND_VIEW */
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/print.h"
#include "executor/executor.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parse_coerce.h"
#include "parser/parse_oper.h"
#include "rewrite/rewriteManip.h"
#include "parser/parse_relation.h"
#include "utils/builtins.h"
#include "parser/parsetree.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "catalog/namespace.h"
#include "catalog/pg_cast.h"
#include "commands/createas.h"
#include "executor/spi.h"
#include "tcop/utility.h"
#include "tcop/tcopprot.h"             /* pg_parse_query, pg_analyze_and_rewrite_fixedparams */
#include <time.h>

#include "classify_query.h"
#include "classify_having.h"
#include "provsql_mmap.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"
#include "safe_query.h"

#if PG_VERSION_NUM < 100000
#error "ProvSQL requires PostgreSQL version 10 or later"
#endif

#include "compatibility.h"

PG_MODULE_MAGIC; ///< Required PostgreSQL extension magic block

/* -------------------------------------------------------------------------
 * Global state & forward declarations
 * ------------------------------------------------------------------------- */

bool provsql_interrupted = false;
static bool provsql_active = true; ///< @c true while ProvSQL query rewriting is enabled
bool provsql_where_provenance = false;
static bool provsql_update_provenance = false; ///< @c true when provenance tracking for DML is enabled
int provsql_verbose = 100; ///< Verbosity level; controlled by the @c provsql.verbose_level GUC
char *provsql_last_eval_method = NULL; ///< Last probability evaluation method(s) used; exposed via @c provsql.last_eval_method
bool provsql_aggtoken_text_as_uuid = false; ///< When @c true, @c agg_token::text emits the underlying provenance UUID instead of @c "value (*)"
char *provsql_tool_search_path = NULL; ///< Colon-separated directory list prepended to @c PATH when invoking external tools (d4, c2d, minic2d, dsharp, weightmc, graph-easy); controlled by the @c provsql.tool_search_path GUC. Superuser-only (@c PGC_SUSET): it dictates which directories the postgres OS user searches for executables, so a non-privileged role must not be able to point it at an attacker-controlled binary.
char *provsql_fallback_compiler = NULL; ///< Compiler used by @c BooleanCircuit::makeDD as the final fallback after @c interpretAsDD and tree-decomposition both fail; controlled by the @c provsql.fallback_compiler GUC (default @c "d4")
char *provsql_kcmcp_server = NULL; ///< Launch command for the managed KCMCP server (with a @c {endpoint} placeholder); controlled by the @c provsql.kcmcp_server GUC. Empty means no managed server is launched.
int provsql_monte_carlo_seed = -1; ///< Seed for the Monte Carlo sampler; -1 means non-deterministic (std::random_device); controlled by the @c provsql.monte_carlo_seed GUC
int provsql_rv_mc_samples = 10000; ///< Default sample count for analytical-evaluator MC fallbacks; 0 disables fallback (callers raise instead); controlled by the @c provsql.rv_mc_samples GUC
bool provsql_simplify_on_load = true; ///< Run universal cmp-resolution passes when @c getGenericCircuit returns; controlled by the @c provsql.simplify_on_load GUC
bool provsql_hybrid_evaluation = true; ///< Run the hybrid-evaluator simplifier inside @c probability_evaluate; controlled by the @c provsql.hybrid_evaluation GUC
bool provsql_cmp_probability_evaluation = true; ///< Run closed-form / analytic probability evaluators for @c gate_cmps inside @c probability_evaluate (currently the Poisson-binomial pre-pass for HAVING-COUNT; future MIN / MAX / SUM evaluators will gate on the same GUC); controlled by the @c provsql.cmp_probability_evaluation GUC
bool provsql_inversion_free = true; ///< Insert the inversion-free structured-d-DNNF path into the default probability chain (after independent, when a certificate is present); controlled by the @c provsql.inversion_free GUC
bool provsql_boolean_provenance = false; ///< Opt-in safe-query optimisation: when @c true, rewrites hierarchical conjunctive queries to a read-once form whose probability is computable in linear time. The resulting circuit is tagged so that semiring evaluations admitting no homomorphism from Boolean functions refuse to run on it. Controlled by the @c provsql.boolean_provenance GUC.


extern void _PG_init(void);
extern void _PG_fini(void);

static planner_hook_type prev_planner = NULL; ///< Previous planner hook (chained)

static Query *process_query(const constants_t *constants, Query *q,
                            bool **removed, bool wrap_root, bool top_level,
                            const InvFreeMarkerCtx *inv_ctx);
static Expr *wrap_in_assume_boolean(const constants_t *constants, Expr *expr);
static Expr *wrap_in_annotate(const constants_t *constants, Expr *expr,
                              const char *cert);

/* -------------------------------------------------------------------------
 * Provenance attribute construction
 * ------------------------------------------------------------------------- */

/**
 * @brief Build a Var node that references the provenance column of a relation.
 *
 * Creates a @c Var pointing to attribute @p attid of range-table entry
 * @p relid, typed as UUID, and marks the column as selected in the
 * permission bitmap so PostgreSQL grants access correctly.
 *
 * @param constants  Extension OID cache.
 * @param q          Owning query (needed to update permission info on PG 16+).
 * @param r          Range-table entry that owns the provenance column.
 * @param relid      1-based index of @p r in @p q->rtable.
 * @param attid      1-based attribute number of the provenance column in @p r.
 * @return  A freshly allocated @c Var node.
 */
static Var *make_provenance_attribute(const constants_t *constants, Query *q,
                                      RangeTblEntry *r, Index relid,
                                      AttrNumber attid) {
  Var *v = makeNode(Var);

  v->varno = relid;
  v->varattno = attid;

#if PG_VERSION_NUM >= 130000
  v->varnosyn = relid;
  v->varattnosyn = attid;
#else
  v->varnoold = relid;
  v->varoattno = attid;
#endif

  v->vartype = constants->OID_TYPE_UUID;
  v->varcollid = InvalidOid;
  v->vartypmod = -1;
  v->location = -1;

#if PG_VERSION_NUM >= 160000
  if (r->perminfoindex != 0) {
    RTEPermissionInfo *rpi =
      list_nth_node(RTEPermissionInfo, q->rteperminfos, r->perminfoindex - 1);
    rpi->selectedCols = bms_add_member(
      rpi->selectedCols, attid - FirstLowInvalidHeapAttributeNumber);
  }
#else
  r->selectedCols = bms_add_member(r->selectedCols,
                                   attid - FirstLowInvalidHeapAttributeNumber);
#endif

  return v;
}

/* -------------------------------------------------------------------------
 * Helper mutators: attribute-number fixup and type patching
 * ------------------------------------------------------------------------- */

/** @brief Context for the @c reduce_varattno_mutator tree walker. */
typedef struct reduce_varattno_mutator_context {
  Index varno;  ///< Range-table entry whose attribute numbers are being adjusted
  int *offset;  ///< Per-attribute cumulative shift to apply
} reduce_varattno_mutator_context;

/**
 * @brief Tree-mutator callback that adjusts Var attribute numbers.
 * @param node  Current expression tree node.
 * @param ctx   Pointer to a @c reduce_varattno_mutator_context.
 * @return      Possibly modified node.
 */
static Node *reduce_varattno_mutator(Node *node, void *ctx) {
  reduce_varattno_mutator_context *context = (reduce_varattno_mutator_context *)ctx;
  if (node == NULL)
    return NULL;

  if (IsA(node, Var)) {
    Var *v = (Var *)node;

    if (v->varno == context->varno) {
      v->varattno += context->offset[v->varattno - 1];
    }
  }

  return expression_tree_mutator(node, reduce_varattno_mutator, ctx);
}

/**
 * @brief Adjust Var attribute numbers in @p targetList after columns are removed.
 *
 * When provenance columns are stripped from a subquery's target list, the
 * remaining columns shift left.  This function applies a pre-computed
 * @p offset array (one entry per original column) to correct all @c Var
 * nodes that reference range-table entry @p varno.
 *
 * @param targetList  Target list of the outer query to patch.
 * @param varno       Range-table entry whose attribute numbers need fixing.
 * @param offset      Cumulative shift per original attribute (negative or zero).
 */
static void reduce_varattno_by_offset(List *targetList, Index varno,
                                      int *offset) {
  ListCell *lc;
  reduce_varattno_mutator_context context = {varno, offset};

  foreach (lc, targetList) {
    Node *te = lfirst(lc);
    expression_tree_mutator(te, reduce_varattno_mutator, &context);
  }
}

/** @brief Context for the @c aggregation_type_mutator tree walker. */
typedef struct aggregation_type_mutator_context {
  Index varno;                ///< Range-table entry index of the aggregate var
  Index varattno;             ///< Attribute number of the aggregate column
  const constants_t *constants; ///< Extension OID cache
} aggregation_type_mutator_context;

/**
 * @brief Check if a Var matches the target aggregate column.
 */
static bool is_target_agg_var(Node *node,
                              aggregation_type_mutator_context *context) {
  if (IsA(node, Var)) {
    Var *v = (Var *)node;
    return v->varno == context->varno && v->varattno == context->varattno;
  }
  return false;
}

/**
 * @brief Tree-mutator that retypes a specific Var to @c agg_token.
 *
 * When the target Var is inside a cast FuncExpr, replaces the cast
 * function with the equivalent agg_token→target cast from pg_cast.
 * When the Var appears bare (e.g. in a TargetEntry for display), it is
 * retyped to agg_token directly.  In all other contexts (arithmetic,
 * window functions, etc.), wraps the Var in an explicit agg_token→original
 * cast so that parent nodes receive the expected type.
 *
 * @param node  Current expression tree node.
 * @param ctx   Pointer to an @c aggregation_type_mutator_context (varno,
 *              varattno, and constants).
 * @return      Possibly modified node.
 */
static Node *
aggregation_type_mutator(Node *node, void *ctx) {
  aggregation_type_mutator_context *context = (aggregation_type_mutator_context *)ctx;
  if (node == NULL)
    return NULL;

  if (IsA(node, FuncExpr)) {
    FuncExpr *f = (FuncExpr *)node;

    /* Check if this is a cast wrapping our target Var */
    if (list_length(f->args) == 1 &&
        is_target_agg_var(linitial(f->args), context)) {
      /* Look up the cast from agg_token to the target type */
      HeapTuple castTuple = SearchSysCache2(CASTSOURCETARGET,
                                            ObjectIdGetDatum(context->constants->OID_TYPE_AGG_TOKEN),
                                            ObjectIdGetDatum(f->funcresulttype));

      if (HeapTupleIsValid(castTuple)) {
        Form_pg_cast castForm = (Form_pg_cast) GETSTRUCT(castTuple);
        if (OidIsValid(castForm->castfunc)) {
          f->funcid = castForm->castfunc;
        }
        ReleaseSysCache(castTuple);
      }

      /* Retype the Var inside */
      ((Var *)linitial(f->args))->vartype =
        context->constants->OID_TYPE_AGG_TOKEN;

      return (Node *)f;
    }
  }

  if (IsA(node, Var)) {
    Var *v = (Var *)node;

    if (v->varno == context->varno && v->varattno == context->varattno) {
      v->vartype = context->constants->OID_TYPE_AGG_TOKEN;
    }
  }
  return expression_tree_mutator(node, aggregation_type_mutator, ctx);
}

/**
 * @brief Retypes aggregation-result Vars in @p q from UUID to @c agg_token.
 *
 * After a subquery that contains @c provenance_aggregate is processed, its
 * result type is @c agg_token rather than plain UUID.  This mutator walks
 * the outer query and updates the type of every @c Var referencing that
 * result column so that subsequent type-checking passes correctly.
 *
 * @param constants   Extension OID cache.
 * @param q           Outer query to patch.
 * @param rteid       Range-table index of the subquery in @p q.
 * @param targetList  Target list of the subquery (to locate provenance_aggregate columns).
 */
static void fix_type_of_aggregation_result(const constants_t *constants,
                                           Query *q, Index rteid,
                                           List *targetList) {
  ListCell *lc;
  aggregation_type_mutator_context context = {0, 0, constants};
  Index attno = 1;

  foreach (lc, targetList) {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    if (IsA(te->expr, FuncExpr)) {
      FuncExpr *f = (FuncExpr *)te->expr;

      if (f->funcid == constants->OID_FUNCTION_PROVENANCE_AGGREGATE) {
        context.varno = rteid;
        context.varattno = attno;
        query_tree_mutator(q, aggregation_type_mutator, &context,
                           QTW_DONT_COPY_QUERY | QTW_IGNORE_RC_SUBQUERIES);

        /* Check if the retyped column is used in ORDER BY or GROUP BY */
        {
          ListCell *lc2;
          foreach (lc2, q->targetList) {
            TargetEntry *outer_te = (TargetEntry *)lfirst(lc2);
            if (IsA(outer_te->expr, Var)) {
              Var *v = (Var *)outer_te->expr;
              if (v->varno == rteid && v->varattno == attno &&
                  outer_te->ressortgroupref > 0) {
                ListCell *lc3;
                foreach (lc3, q->sortClause) {
                  SortGroupClause *sgc = (SortGroupClause *)lfirst(lc3);
                  if (sgc->tleSortGroupRef == outer_te->ressortgroupref)
                    provsql_error("ORDER BY on aggregate results from "
                                  "a subquery not supported");
                }
                foreach (lc3, q->groupClause) {
                  SortGroupClause *sgc = (SortGroupClause *)lfirst(lc3);
                  if (sgc->tleSortGroupRef == outer_te->ressortgroupref)
                    provsql_error("GROUP BY on aggregate results from "
                                  "a subquery not supported");
                }
              }
            }
          }
        }
      }
    }
    ++attno;
  }
}

/**
 * @brief Memo entry mapping a recursive-CTE name to its lowered scan subquery.
 *
 * A recursive CTE referenced more than once (e.g. in two arms of a top-level
 * UNION) must be lowered -- and its backing temp table created by
 * @c eval_recursive -- exactly once: re-running the fixpoint would
 * @c DROP @c TABLE the temp table that an earlier reference's analyzed scan
 * already bound to by OID, yielding "could not open relation with OID ...".
 * @c inline_ctes_in_rtable records each lowering here and reuses it for
 * subsequent references to the same CTE.
 */
typedef struct LoweredCte {
  const char *name;
  Query      *subquery;
} LoweredCte;

static Query *lookup_lowered_cte(List *lowered, const char *name) {
  ListCell *lc;
  foreach (lc, lowered) {
    LoweredCte *e = (LoweredCte *)lfirst(lc);
    if (strcmp(e->name, name) == 0)
      return e->subquery;
  }
  return NULL;
}

#if PG_VERSION_NUM >= 150000
/**
 * @brief Lower a recursive CTE to a provenance-aware fixpoint (PROTOTYPE).
 *
 * ProvSQL cannot rewrite @c WITH @c RECURSIVE in place: the recursive term
 * forbids the aggregate that provenance-merging needs.  Instead, for a recursive
 * CTE whose body touches provenance-tracked relations, we deparse the body to
 * SQL, run @c provsql.eval_recursive over SPI now (at plan time) -- it evaluates
 * @c base @c UNION @c recursive to a fixpoint, letting ProvSQL's own rewriting
 * compute the join @c times gates, the untracked base branch's @c gate_one, and
 * the @c UNION @c plus merge -- and leaves a tracked temp table named after the
 * CTE holding @c (cols..., @c provsql).  We then rewrite this CTE reference into
 * a plain scan of that table, which the rest of @c process_query handles as an
 * ordinary tracked relation.
 *
 * Returns @c true on success, @c false if the shape is unsupported (the caller
 * falls back to the historical error).  Boolean provenance, acyclic data, and
 * UNION (set) recursion only; the driver guards non-termination.  This is a
 * feasibility prototype: it performs SPI work and temp-table creation during
 * planning, and recognises only the linear/UNION shape.  See poc/recursive/.
 */
static bool lower_recursive_cte(CommonTableExpr *cte, RangeTblEntry *r) {
  Query         *cteq = (Query *) cte->ctequery;
  char          *body_text;
  StringInfoData cols, coldef, call, scan;
  ListCell      *lcn, *lct;
  bool           first = true;
  int            rc;

  if (cteq == NULL || !IsA(cteq, Query))
    return false;

  /* Only UNION (set) recursion is in scope.  Reject UNION ALL (bag semantics,
   * which the set-fixpoint driver does not model -- and which is unbounded on a
   * graph with several paths) and anything that is not a plain UNION; the
   * caller then raises the usual "Recursive CTEs not supported". */
  if (cteq->setOperations == NULL ||
      !IsA(cteq->setOperations, SetOperationStmt) ||
      ((SetOperationStmt *) cteq->setOperations)->op != SETOP_UNION ||
      ((SetOperationStmt *) cteq->setOperations)->all)
    return false;

  /* Reject a term whose target list contains a set-returning function
   * (e.g. SELECT unnest(...)).  Such a CTE is not a provenance fixpoint we
   * can lower, and -- more importantly -- the per-round
   * INSERT ... SELECT ... UNION SELECT srf(...) the driver would build
   * crashes PostgreSQL's planner: the SRF tlist split leaves a NULL expr
   * in the PathTarget, which get_expr_width then dereferences.  Bail out so
   * the caller raises the usual "Recursive CTEs not supported" error. */
  {
    ListCell *lc;
    foreach(lc, cteq->rtable) {
      RangeTblEntry *sub = (RangeTblEntry *) lfirst(lc);
      if (sub->rtekind == RTE_SUBQUERY && sub->subquery != NULL &&
          sub->subquery->hasTargetSRFs)
        return false;
    }
  }

  /* Deparse the whole recursive CTE body to SQL.  It references the working
   * relation by the CTE name; the driver creates a temp table of that name. */
  body_text = pg_get_querydef(cteq, false);

  /* User column names (comma list) and column definitions (name type). */
  initStringInfo(&cols);
  initStringInfo(&coldef);
  forboth(lcn, cte->ctecolnames, lct, cte->ctecoltypes) {
    char *name = strVal(lfirst(lcn));
    Oid   typid = lfirst_oid(lct);
    if (!first) {
      appendStringInfoString(&cols, ", ");
      appendStringInfoString(&coldef, ", ");
    }
    first = false;
    appendStringInfoString(&cols, quote_identifier(name));
    appendStringInfo(&coldef, "%s %s", quote_identifier(name), format_type_be(typid));
  }

  if (provsql_verbose >= 20)
    provsql_notice("Lowering recursive CTE '%s':\n body   = %s\n coldef = %s",
                   cte->ctename, body_text, coldef.data);

  /* Drive the fixpoint now, leaving a tracked temp table `ctename`. */
  initStringInfo(&call);
  appendStringInfo(&call, "SELECT provsql.eval_recursive(%s, %s, %s, %s)",
                   quote_literal_cstr(body_text),
                   quote_literal_cstr(cte->ctename),
                   quote_literal_cstr(cols.data),
                   quote_literal_cstr(coldef.data));
  if ((rc = SPI_connect()) != SPI_OK_CONNECT)
    provsql_error("Recursive CTE lowering: SPI_connect failed (%d)", rc);
  rc = SPI_execute(call.data, false, 0);
  SPI_finish();
  if (rc < 0)
    provsql_error("Recursive CTE lowering: eval_recursive failed (%d)", rc);

  /* Replace the CTE reference with a scan of the populated table. */
  initStringInfo(&scan);
  appendStringInfo(&scan, "SELECT %s FROM %s",
                   cols.data, quote_identifier(cte->ctename));
  {
    List *raw = pg_parse_query(scan.data);
    List *analyzed = pg_analyze_and_rewrite_fixedparams(
                       linitial_node(RawStmt, raw), scan.data, NULL, 0, NULL);
    r->rtekind  = RTE_SUBQUERY;
    r->subquery = linitial_node(Query, analyzed);
    r->ctename  = NULL;
    r->ctelevelsup = 0;
  }
  return true;
}
#endif

/**
 * @brief Inline CTE references as subqueries within a query.
 *
 * Replaces each non-recursive RTE_CTE entry in @p rtable with an
 * RTE_SUBQUERY containing a copy of the CTE's query, looking up
 * definitions in @p cteList.  Recurses into newly inlined subqueries
 * to handle nested CTE references (ctelevelsup > 0).
 *
 * @param rtable   Range table to scan for RTE_CTE entries.
 * @param cteList  CTE definitions to look up names in.
 * @param lowered  In/out memo of recursive CTEs already lowered (name ->
 *                 scan subquery), so a recursive CTE referenced more than
 *                 once is lowered exactly once and later references reuse
 *                 the first lowering instead of recreating its temp table.
 */
static void inline_ctes_in_rtable(List *rtable, List *cteList, List **lowered) {
  ListCell *lc;
  foreach (lc, rtable) {
    RangeTblEntry *r = (RangeTblEntry *)lfirst(lc);
    if (r->rtekind == RTE_CTE) {
      ListCell *lc2;
      foreach (lc2, cteList) {
        CommonTableExpr *cte = (CommonTableExpr *)lfirst(lc2);
        if (strcmp(cte->ctename, r->ctename) == 0) {
          if (cte->cterecursive) {
#if PG_VERSION_NUM >= 150000
            /* A recursive CTE referenced more than once (e.g. once per
             * UNION arm) must be lowered exactly once: re-running
             * eval_recursive would DROP and recreate its temp table,
             * invalidating the OID the first reference's analyzed scan
             * bound to.  Reuse the earlier lowering when we have it. */
            Query *memo = lookup_lowered_cte(*lowered, cte->ctename);
            if (memo != NULL) {
              r->rtekind = RTE_SUBQUERY;
              r->subquery = copyObject(memo);
              r->ctename = NULL;
              r->ctelevelsup = 0;
            } else if (lower_recursive_cte(cte, r)) {
              /* Lowering succeeded; remember the scan subquery so any
               * further reference to this CTE reuses it. */
              LoweredCte *e = (LoweredCte *)palloc(sizeof(LoweredCte));
              e->name = pstrdup(cte->ctename);
              e->subquery = copyObject(r->subquery);
              *lowered = lappend(*lowered, e);
            } else {
              /* Unsupported recursion shape (e.g. UNION ALL). */
              provsql_error("Recursive CTEs not supported (unsupported recursion shape)");
            }
#else
            provsql_error("Recursive CTEs not supported");
#endif
          } else {
            r->rtekind = RTE_SUBQUERY;
            r->subquery = copyObject((Query *)cte->ctequery);
            r->ctename = NULL;
            r->ctelevelsup = 0;
            /* Recurse: the inlined subquery may reference other CTEs
             * from the same cteList */
            inline_ctes_in_rtable(r->subquery->rtable, cteList, lowered);
          }
          break;
        }
      }
    } else if (r->rtekind == RTE_SUBQUERY && r->subquery != NULL) {
      /* Recurse into existing subqueries (e.g., UNION branches) to
       * inline CTE references they may contain */
      inline_ctes_in_rtable(r->subquery->rtable, cteList, lowered);
    }
  }
}

/**
 * @brief Inline all CTE references in @p q as subqueries.
 */
static void inline_ctes(Query *q) {
  List *lowered = NIL;
  if (q->cteList == NIL)
    return;
  inline_ctes_in_rtable(q->rtable, q->cteList, &lowered);
  q->cteList = NIL;
}

/**
 * @brief Collect all provenance Var nodes reachable from @p q's range table.
 *
 * Walks every RTE in @p q->rtable:
 * - @c RTE_RELATION: looks for a column named @c provsql of type UUID.
 * - @c RTE_SUBQUERY: recursively calls @c process_query and splices the
 *   resulting provenance column back into the parent's column list, also
 *   patching outer Var attribute numbers if inner columns were removed.
 * - @c RTE_CTE: non-recursive CTEs are inlined as @c RTE_SUBQUERY before
 *   the main loop, then processed as above.  Recursive CTEs raise an error.
 * - @c RTE_FUNCTION: handled when the function returns a single UUID column
 *   named @c provsql.
 * - @c RTE_JOIN / @c RTE_VALUES / @c RTE_GROUP: handled passively (the
 *   underlying base-table RTEs supply the tokens).
 *
 * @param constants  Extension OID cache.
 * @param q          Query whose range table is scanned (subquery RTEs are
 *                   modified in place by the recursive call).
 * @param inv_ctx    Inversion-free marker context for @p q, or @c NULL; its
 *                   per-subquery child context is threaded into each recursive
 *                   @c process_query call so a flattened view's base inputs
 *                   receive their order markers.
 * @return  List of @c Var nodes, one per provenance source; @c NIL if the
 *          query has no provenance-bearing relation.
 */
static List *get_provenance_attributes(const constants_t *constants, Query *q,
                                       const InvFreeMarkerCtx *inv_ctx) {
  List *prov_atts = NIL;

  for(Index rteid = 1; rteid <= q->rtable->length; ++rteid) {
    RangeTblEntry *r = list_nth_node(RangeTblEntry, q->rtable, rteid-1);

    if (r->rtekind == RTE_RELATION) {
      ListCell *lc;
      AttrNumber attid = 1;

      /* PG 14 and 15 leave the OLD/NEW rule-placeholder RTEs (relkind
       * = RELKIND_VIEW, inFromCl = false) in the rewritten range table
       * for any view body.  PG 16+ removes them.  They are never
       * scanned and the planner does not build a RelOptInfo for them,
       * so any Var we point at them later fails find_base_rel().
       * Filter them out here; any post-rewrite RTE_RELATION whose
       * relkind is still a view is one of these artifacts. */
      if (r->relkind == RELKIND_VIEW)
        continue;

      foreach (lc, r->eref->colnames) {
        const char *v = strVal(lfirst(lc));

        if (!strcmp(v, PROVSQL_COLUMN_NAME) &&
            get_atttype(r->relid, attid) == constants->OID_TYPE_UUID) {
          prov_atts =
            lappend(prov_atts,
                    make_provenance_attribute(constants, q, r, rteid, attid));
        }

        ++attid;
      }
    } else if (r->rtekind == RTE_SUBQUERY) {
      bool *inner_removed = NULL;
      int old_targetlist_length =
        r->subquery->targetList ? r->subquery->targetList->length : 0;
      Query *new_subquery =
        process_query(constants, r->subquery, &inner_removed, false, false,
                      (inv_ctx && rteid - 1 < (Index) inv_ctx->natoms)
                        ? inv_ctx->sub[rteid - 1] : NULL);
      if (new_subquery != NULL) {
        int i = 0;
        int *offset = (int *)palloc(old_targetlist_length * sizeof(int));
        unsigned varattnoprovsql;
        ListCell *cell, *prev;

        r->subquery = new_subquery;

        if (inner_removed != NULL) {
          for (cell = list_head(r->eref->colnames), prev = NULL;
               cell != NULL;) {
            if (inner_removed[i]) {
              r->eref->colnames =
                my_list_delete_cell(r->eref->colnames, cell, prev);
              if (prev)
                cell = my_lnext(r->eref->colnames, prev);
              else
                cell = list_head(r->eref->colnames);
            } else {
              prev = cell;
              cell = my_lnext(r->eref->colnames, cell);
            }
            ++i;
          }
          for (i = 0; i < old_targetlist_length; ++i) {
            offset[i] =
              (i == 0 ? 0 : offset[i - 1]) - (inner_removed[i] ? 1 : 0);
          }

          reduce_varattno_by_offset(q->targetList, rteid, offset);
        }

        varattnoprovsql = 0;
        for (cell = list_head(new_subquery->targetList); cell != NULL;
             cell = my_lnext(new_subquery->targetList, cell)) {
          TargetEntry *te = (TargetEntry *)lfirst(cell);
          ++varattnoprovsql;
          if (te->resname && !strcmp(te->resname, PROVSQL_COLUMN_NAME))
            break;
        }

        /* In a UNION, every branch must expose a provsql column so the set
         * operation's columns line up.  A branch with no provenance source
         * (constant rows, or an untracked relation) is returned unchanged by
         * process_query above and has no provsql column; such rows are present
         * unconditionally, so their provenance is the multiplicative identity.
         * Append a gate_one() provsql column.  Set-operation branches never
         * carry resjunk entries (those are rejected by the planner), so the
         * column lands last -- the same ordinal position as the provsql column
         * the provenance-bearing branches get. */
        if (cell == NULL && q->setOperations != NULL &&
            IsA(q->setOperations, SetOperationStmt) &&
            ((SetOperationStmt *)q->setOperations)->op == SETOP_UNION) {
          FuncExpr *one_expr = makeNode(FuncExpr);
          TargetEntry *one_te;
          one_expr->funcid = constants->OID_FUNCTION_GATE_ONE;
          one_expr->funcresulttype = constants->OID_TYPE_UUID;
          one_expr->args = NIL;
          one_expr->location = -1;
          one_te = makeTargetEntry((Expr *)one_expr,
                                   list_length(new_subquery->targetList) + 1,
                                   pstrdup(PROVSQL_COLUMN_NAME), false);
          new_subquery->targetList = lappend(new_subquery->targetList, one_te);
          varattnoprovsql = list_length(new_subquery->targetList);
          cell = list_tail(new_subquery->targetList);
        }

        if (cell != NULL) {
          r->eref->colnames = list_insert_nth(r->eref->colnames, varattnoprovsql-1,
                                              makeString(pstrdup(PROVSQL_COLUMN_NAME)));
          prov_atts =
            lappend(prov_atts, make_provenance_attribute(
                      constants, q, r, rteid, varattnoprovsql));
        }
        fix_type_of_aggregation_result(constants, q, rteid,
                                       r->subquery->targetList);
      }
    } else if (r->rtekind == RTE_JOIN) {
      if (r->jointype == JOIN_INNER || r->jointype == JOIN_LEFT ||
          r->jointype == JOIN_FULL || r->jointype == JOIN_RIGHT) {
        // Nothing to do, there will also be RTE entries for the tables
        // that are part of the join, from which we will extract the
        // provenance information
      } else { // Semijoin (should be feasible, but check whether the second
               // provenance information is available) Antijoin (feasible with
               // negation)
        provsql_error("JOIN type not supported");
      }
    } else if (r->rtekind == RTE_FUNCTION) {
      ListCell *lc;
      AttrNumber attid = 1;

      foreach (lc, r->functions) {
        RangeTblFunction *func = (RangeTblFunction *)lfirst(lc);

        if (func->funccolcount == 1) {
          FuncExpr *expr = (FuncExpr *)func->funcexpr;
          if (expr->funcresulttype == constants->OID_TYPE_UUID &&
              !strcmp(get_rte_attribute_name(r, attid), PROVSQL_COLUMN_NAME)) {
            prov_atts = lappend(prov_atts, make_provenance_attribute(
                                  constants, q, r, rteid, attid));
          }
        } else {
          provsql_error("FROM function with multiple output "
                        "attributes not supported");
        }

        attid += func->funccolcount;
      }
    } else if (r->rtekind == RTE_VALUES) {
      // Nothing to do, no provenance attribute in literal values
    } else if (r->rtekind == RTE_RESULT) {
      // Empty-FROM RTE (no provenance).  Also what the outer-join lowering
      // leaves behind when it neutralises an orphaned subquery arm.
#if PG_VERSION_NUM >= 180000
    } else if (r->rtekind == RTE_GROUP) {
      // Introduced in PostgreSQL 18, we already handle group by from
      // groupClause
#endif
    } else {
      provsql_error("FROM clause not supported");
    }
  }

  return prov_atts;
}

/* -------------------------------------------------------------------------
 * Target-list surgery
 * ------------------------------------------------------------------------- */

/**
 * @brief Strip provenance UUID columns from @p q's SELECT list.
 *
 * Scans the target list and removes every @c Var entry whose column name is
 * @c provsql and whose type is UUID.  The remaining entries have their
 * @c resno values decremented to fill the gaps.
 *
 * @param constants  Extension OID cache.
 * @param q          Query to modify in place.
 * @param removed    Out-param: allocated boolean array (length =
 *                   original target list length) where @c true means the
 *                   corresponding entry was removed.  The caller must
 *                   @c pfree this array when done.
 * @return  Bitmapset of @c ressortgroupref values whose entries were
 *          removed (so the caller can clean up GROUP BY / ORDER BY).
 */
static Bitmapset *
remove_provenance_attributes_select(const constants_t *constants, Query *q,
                                    bool **removed) {
  int nbRemoved = 0;
  int i = 0;
  Bitmapset *ressortgrouprefs = NULL;
  ListCell *cell, *prev;
  *removed = (bool *)palloc(q->targetList->length * sizeof(bool));

  for (cell = list_head(q->targetList), prev = NULL; cell != NULL;) {
    TargetEntry *rt = (TargetEntry *)lfirst(cell);
    (*removed)[i] = false;

    if (rt->expr->type == T_Var) {
      Var *v = (Var *)rt->expr;

      if (v->vartype == constants->OID_TYPE_UUID) {
        const char *colname;

        if (rt->resname)
          colname = rt->resname;
        else {
          /* This case occurs, for example, when grouping by a column
           * that is projected out */
          RangeTblEntry *r = (RangeTblEntry *)list_nth(q->rtable, v->varno - 1);
          colname = strVal(list_nth(r->eref->colnames, v->varattno - 1));
        }

        if (!strcmp(colname, PROVSQL_COLUMN_NAME)) {
          q->targetList = my_list_delete_cell(q->targetList, cell, prev);

          (*removed)[i] = true;
          ++nbRemoved;

          if (rt->ressortgroupref > 0)
            ressortgrouprefs =
              bms_add_member(ressortgrouprefs, rt->ressortgroupref);
        }
      }
    }

    if ((*removed)[i]) {
      if (prev) {
        cell = my_lnext(q->targetList, prev);
      } else {
        cell = list_head(q->targetList);
      }
    } else {
      rt->resno -= nbRemoved;
      prev = cell;
      cell = my_lnext(q->targetList, cell);
    }

    ++i;
  }

  return ressortgrouprefs;
}

/**
 * @brief Semiring operation used to combine provenance tokens.
 *
 * @c SR_TIMES corresponds to the multiplicative operation (joins, Cartesian
 * products), @c SR_PLUS to the additive operation (duplicate elimination), and
 * @c SR_MONUS to the monus / set-difference operation (EXCEPT).
 *
 * @see https://provsql.org/lean-docs/Provenance/QueryRewriting.html
 *      Lean 4 formalization of rewriting rules (R1)--(R5) and correctness
 *      theorem @c Query.rewriting_valid.
 */
typedef enum {
  SR_PLUS,  ///< Semiring addition (UNION, SELECT DISTINCT)
  SR_MONUS, ///< Semiring monus / set difference (EXCEPT)
  SR_TIMES  ///< Semiring multiplication (JOIN, Cartesian product)
} semiring_operation;

/* -------------------------------------------------------------------------
 * Semiring expression builders
 * ------------------------------------------------------------------------- */

/**
 * @brief Wrap @p toExpr in a @c provenance_eq gate if @p fromOpExpr is an
 *        equality between two tracked columns.
 *
 * Used for where-provenance: each equijoin condition (and some WHERE
 * equalities) introduces an @c eq gate that records which attribute positions
 * were compared.  Because this function is also called for WHERE predicates,
 * it applies extra guards and silently returns @p toExpr unchanged when the
 * expression does not match the expected shape (both sides must be @c Var
 * nodes, possibly wrapped in a @c RelabelType).
 *
 * @param constants    Extension OID cache.
 * @param fromOpExpr   The equality @c OpExpr to inspect.
 * @param toExpr       Existing provenance expression to wrap.
 * @param columns      Per-RTE column-numbering array.  EQ gate positions
 *                     carry the same sequential-number caveat as PROJECT
 *                     gate positions (see @c build_column_map()); they are
 *                     only correct when each operand's RTE is either a join
 *                     RTE or a subquery, not a bare provenance-tracked base
 *                     table.
 * @return  @p toExpr wrapped in @c provenance_eq(toExpr, col1, col2), or
 *          @p toExpr unchanged if the shape is unsupported.
 */
static Expr *add_eq_from_OpExpr_to_Expr(const constants_t *constants,
                                        OpExpr *fromOpExpr, Expr *toExpr,
                                        int **columns) {
  Datum first_arg;
  Datum second_arg;
  FuncExpr *fc;
  Const *c1;
  Const *c2;
  Var *v1;
  Var *v2;

  if (my_lnext(fromOpExpr->args, list_head(fromOpExpr->args))) {
    /* Sometimes Var is nested within a RelabelType */
    if (IsA(linitial(fromOpExpr->args), Var)) {
      v1 = linitial(fromOpExpr->args);
    } else if (IsA(linitial(fromOpExpr->args), RelabelType)) {
      /* In the WHERE case it can be a Const */
      RelabelType *rt1 = linitial(fromOpExpr->args);
      if (IsA(rt1->arg, Var)) { /* Can be Param in the WHERE case */
        v1 = (Var *)rt1->arg;
      } else
        return toExpr;
    } else
      return toExpr;
    if (!columns[v1->varno - 1])
      return toExpr;
    first_arg = Int16GetDatum(columns[v1->varno - 1][v1->varattno - 1]);

    if (IsA(lsecond(fromOpExpr->args), Var)) {
      v2 = lsecond(fromOpExpr->args);
    } else if (IsA(lsecond(fromOpExpr->args), RelabelType)) {
      /* In the WHERE case it can be a Const */
      RelabelType *rt2 = lsecond(fromOpExpr->args);
      if (IsA(rt2->arg, Var)) { /* Can be Param in the WHERE case */
        v2 = (Var *)rt2->arg;
      } else
        return toExpr;
    } else
      return toExpr;
    if (!columns[v2->varno - 1])
      return toExpr;
    second_arg = Int16GetDatum(columns[v2->varno - 1][v2->varattno - 1]);

    fc = makeNode(FuncExpr);
    fc->funcid = constants->OID_FUNCTION_PROVENANCE_EQ;
    fc->funcvariadic = false;
    fc->funcresulttype = constants->OID_TYPE_UUID;
    fc->location = -1;

    c1 = makeConst(constants->OID_TYPE_INT, -1, InvalidOid, sizeof(int16),
                   first_arg, false, true);

    c2 = makeConst(constants->OID_TYPE_INT, -1, InvalidOid, sizeof(int16),
                   second_arg, false, true);

    fc->args = list_make3(toExpr, c1, c2);
    return (Expr *)fc;
  }
  return toExpr;
}

/**
 * @brief Walk a join-condition or WHERE quals node and add @c eq gates for
 *        every equality it contains.
 *
 * Dispatches to @c add_eq_from_OpExpr_to_Expr for simple @c OpExpr nodes
 * and iterates over the arguments of an AND @c BoolExpr.  OR/NOT inside a
 * join ON clause are rejected with an error.
 *
 * @param constants  Extension OID cache.
 * @param quals      Root of the quals tree (@c OpExpr or @c BoolExpr), or
 *                   @c NULL (in which case @p result is returned unchanged).
 * @param result     Provenance expression to wrap.
 * @param columns    Per-RTE column-numbering array.
 * @return  Updated provenance expression with zero or more @c eq gates added.
 */
static Expr *add_eq_from_Quals_to_Expr(const constants_t *constants,
                                       Node *quals, Expr *result,
                                       int **columns) {
  OpExpr *oe;

  if (!quals)
    return result;

  if (IsA(quals, OpExpr)) {
    oe = (OpExpr *)quals;
    result = add_eq_from_OpExpr_to_Expr(constants, oe, result, columns);
  } /* Sometimes OpExpr is nested within a BoolExpr */
  else if (IsA(quals, BoolExpr)) {
    BoolExpr *be = (BoolExpr *)quals;
    /* In some cases, there can be an OR or a NOT specified with ON clause */
    if (be->boolop == OR_EXPR || be->boolop == NOT_EXPR) {
      provsql_error("Boolean operators OR and NOT in a join...on "
                    "clause are not supported");
    } else {
      ListCell *lc2;
      foreach (lc2, be->args) {
        if (IsA(lfirst(lc2), OpExpr)) {
          oe = (OpExpr *)lfirst(lc2);
          result = add_eq_from_OpExpr_to_Expr(constants, oe, result, columns);
        }
      }
    }
  } else { /* Handle other cases */
  }
  return result;
}

/**
 * @brief Build the per-row provenance token for an aggregate rewrite.
 *
 * Used by both @c make_aggregation_expression (for the agg_token /
 * @c provenance_semimod path) and @c make_rv_aggregate_expression (for
 * the inline RV-aggregate path).  Combines @p prov_atts via
 * @c provenance_times (under @c SR_TIMES) or @c provenance_monus
 * (under @c SR_MONUS); a single @c prov_att is returned as-is.
 *
 * @return  An @c Expr returning UUID; never @c NULL.
 */
static Expr *combine_prov_atts(const constants_t *constants,
                               List *prov_atts, semiring_operation op) {
  FuncExpr *combine;

  if (my_lnext(prov_atts, list_head(prov_atts)) == NULL)
    return (Expr *)linitial(prov_atts);

  combine = makeNode(FuncExpr);
  if (op == SR_TIMES) {
    ArrayExpr *array = makeNode(ArrayExpr);

    combine->funcid = constants->OID_FUNCTION_PROVENANCE_TIMES;
    combine->funcvariadic = true;

    array->array_typeid = constants->OID_TYPE_UUID_ARRAY;
    array->element_typeid = constants->OID_TYPE_UUID;
    array->elements = prov_atts;
    array->location = -1;

    combine->args = list_make1(array);
  } else { // SR_MONUS
    combine->funcid = constants->OID_FUNCTION_PROVENANCE_MONUS;
    combine->args = prov_atts;
  }
  combine->funcresulttype = constants->OID_TYPE_UUID;
  combine->location = -1;
  return (Expr *)combine;
}

/**
 * @brief Inline rewrite of an RV-returning aggregate into the same
 *        aggregate over provenance-wrapped per-row arguments.
 *
 * Originally Phase 1 of the SUM-over-RV story (see @c aggregation-of-rvs.md);
 * extended to any aggregate whose result type is @c random_variable
 * (e.g. @c provsql.sum, @c provsql.avg).  Replaces @c agg(@c x) with an
 * Aggref whose per-row argument is lifted through @c rv_aggregate_semimod
 * to attach the row's provenance: each row contributes
 * @c mixture(prov_token, X_i, as_random(0)).  The aggregate itself
 * (@c aggfnoid) is preserved verbatim, so its SFUNC / FFUNC decide what
 * gate shape to build from the per-row mixtures.  In particular:
 *  - @c sum(random_variable) collects the mixtures into a single
 *    @c gate_arith @c PLUS root, realising
 *    @f$\mathrm{SUM}(x) = \sum_i \mathbf{1}\{\varphi_i\} \cdot X_i@f$;
 *  - @c avg(random_variable) walks each mixture to recover @c prov_i
 *    and emits @c gate_arith(DIV, @c sum(mixture(p_i,x_i,0)),
 *    @c sum(mixture(p_i,1,0))), the natural lift of "AVG = SUM / COUNT"
 *    into the @c random_variable algebra.
 *
 * Routing happens at @c make_aggregation_expression on
 * @c agg_ref->aggtype @c == @c OID_TYPE_RANDOM_VARIABLE, so any future
 * RV-returning aggregate inherits the same per-row provenance wrap
 * without further C-side dispatch.
 *
 * SR_PLUS (UNION outer level) is handled by the caller; this builder
 * never runs for SR_PLUS.
 */
static Expr *make_rv_aggregate_expression(const constants_t *constants,
                                          Aggref *agg_ref, List *prov_atts,
                                          semiring_operation op) {
  Expr *prov_expr = combine_prov_atts(constants, prov_atts, op);
  Expr *rv_arg = ((TargetEntry *)linitial(agg_ref->args))->expr;
  FuncExpr *wrap;
  Aggref *new_agg;
  TargetEntry *te;

  /* Wrap the per-row RV in mixture(prov, rv, as_random(0)) via the
   * rv_aggregate_semimod SQL helper.  Going through the helper avoids
   * having to look up the specific (uuid, random_variable,
   * random_variable) overload of mixture and the (double precision)
   * overload of as_random at OID-cache time. */
  wrap = makeNode(FuncExpr);
  wrap->funcid = constants->OID_FUNCTION_RV_AGGREGATE_SEMIMOD;
  wrap->funcresulttype = constants->OID_TYPE_RANDOM_VARIABLE;
  wrap->args = list_make2(prov_expr, rv_arg);
  wrap->location = -1;

  /* Rebuild an Aggref calling the SAME aggregate (sum_rv, avg_rv, ...),
   * but with the argument now wrapped.  Inherit the original Aggref's
   * clause positioning; aggargtypes / aggtype stay random_variable. */
  te = makeNode(TargetEntry);
  te->resno = 1;
  te->expr = (Expr *)wrap;

  new_agg = makeNode(Aggref);
  new_agg->aggfnoid = agg_ref->aggfnoid;
  new_agg->aggtype = constants->OID_TYPE_RANDOM_VARIABLE;
  new_agg->aggargtypes = list_make1_oid(constants->OID_TYPE_RANDOM_VARIABLE);
  new_agg->aggkind = AGGKIND_NORMAL;
  new_agg->aggtranstype = InvalidOid;
  new_agg->args = list_make1(te);
  new_agg->location = agg_ref->location;
#if PG_VERSION_NUM >= 140000
  new_agg->aggno = new_agg->aggtransno = -1;
#endif

  return (Expr *)new_agg;
}

/**
 * @brief Build the provenance expression for a single aggregate function.
 *
 * For @c SR_PLUS (union context) returns the first provenance attribute
 * directly.  For @c SR_TIMES or @c SR_MONUS, constructs:
 * @code
 *   provenance_aggregate(fn_oid, result_type,
 *                        original_aggref,
 *                        array_agg(provenance_semimod(arg, times_or_monus_token)))
 * @endcode
 * COUNT(*) and COUNT(expr) are remapped to SUM so that the semimodule
 * semantics (scalar × token → token) work correctly.
 *
 * @param constants  Extension OID cache.
 * @param agg_ref    The original @c Aggref node from the query.
 * @param prov_atts  List of provenance @c Var nodes.
 * @param op         Semiring operation (determines how tokens are combined).
 * @return  Provenance expression of type @c agg_token.
 */
static Expr *make_aggregation_expression(const constants_t *constants,
                                         Aggref *agg_ref, List *prov_atts,
                                         semiring_operation op) {
  Expr *result;
  FuncExpr *expr, *expr_s;
  Aggref *agg = makeNode(Aggref);
  FuncExpr *plus = makeNode(FuncExpr);
  TargetEntry *te_inner = makeNode(TargetEntry);
  Const *fn = makeNode(Const);
  Const *typ = makeNode(Const);

  if (op == SR_PLUS) {
    result = linitial(prov_atts);
  } else {
    Oid aggregation_function = agg_ref->aggfnoid;

    /* Aggregates that return random_variable (sum_rv, avg_rv, and any
     * future RV-returning aggregate) get a different rewrite: instead
     * of going through provenance_semimod (which builds a gate_value
     * from CAST(val AS VARCHAR), nonsensical for an RV), each per-row
     * argument is wrapped in mixture(prov, rv, as_random(0)) and the
     * original aggregate's SFUNC / FFUNC decide what gate shape to
     * build from the resulting mixtures.  See aggregation-of-rvs.md. */
    if (OidIsValid(constants->OID_TYPE_RANDOM_VARIABLE) &&
        agg_ref->aggtype == constants->OID_TYPE_RANDOM_VARIABLE) {
      return make_rv_aggregate_expression(constants, agg_ref, prov_atts, op);
    }

    if (my_lnext(prov_atts, list_head(prov_atts)) == NULL)
      expr = linitial(prov_atts);
    else {
      expr = makeNode(FuncExpr);
      if (op == SR_TIMES) {
        ArrayExpr *array = makeNode(ArrayExpr);

        expr->funcid = constants->OID_FUNCTION_PROVENANCE_TIMES;
        expr->funcvariadic = true;

        array->array_typeid = constants->OID_TYPE_UUID_ARRAY;
        array->element_typeid = constants->OID_TYPE_UUID;
        array->elements = prov_atts;
        array->location = -1;

        expr->args = list_make1(array);
      } else { // SR_MONUS
        expr->funcid = constants->OID_FUNCTION_PROVENANCE_MONUS;
        expr->args = prov_atts;
      }
      expr->funcresulttype = constants->OID_TYPE_UUID;
      expr->location = -1;
    }

    // semimodule function
    expr_s = makeNode(FuncExpr);
    expr_s->funcid = constants->OID_FUNCTION_PROVENANCE_SEMIMOD;
    expr_s->funcresulttype = constants->OID_TYPE_UUID;

    // check the particular case of count
    if (aggregation_function == F_COUNT_) // count(*): counts every row
    {
      Const *one = makeConst(constants->OID_TYPE_INT, -1, InvalidOid,
                             sizeof(int32), Int32GetDatum(1), false, true);
      expr_s->args = list_make2(one, expr);
      aggregation_function = F_SUM_INT4;
    } else if (aggregation_function == F_COUNT_ANY) // count(expr)
    {
      /* count(expr) counts only rows where expr IS NOT NULL, but -- unlike the
       * other aggregates -- an all-NULL group still has a defined result of 0
       * (not NULL), so the row must stay PRESENT in the aggregate to carry the
       * group's existence; it just contributes 0.  Pass the per-row value
       * CASE WHEN expr IS NOT NULL THEN 1 ELSE 0 END: a NULL expr (e.g. the
       * NULL-padded rows a LEFT JOIN manufactures) contributes 0 to the count
       * yet keeps the group alive, so HAVING count(expr)=0 is correctly true.
       * count(*) keeps the constant 1 above. */
      Expr *arg = ((TargetEntry *)linitial(agg_ref->args))->expr;
      CaseExpr *ce = makeNode(CaseExpr);
      CaseWhen *cw = makeNode(CaseWhen);
      NullTest *nt = makeNode(NullTest);

      nt->arg = (Expr *)arg;
      nt->nulltesttype = IS_NOT_NULL;
      nt->argisrow = false;
      nt->location = -1;

      cw->expr = (Expr *)nt;
      cw->result = (Expr *)makeConst(constants->OID_TYPE_INT, -1, InvalidOid,
                                     sizeof(int32), Int32GetDatum(1), false,
                                     true);
      cw->location = -1;

      ce->casetype = constants->OID_TYPE_INT;
      ce->casecollid = InvalidOid;
      ce->arg = NULL;
      ce->args = list_make1(cw);
      ce->defresult = (Expr *)makeConst(constants->OID_TYPE_INT, -1, InvalidOid,
                                        sizeof(int32), Int32GetDatum(0), false,
                                        true);
      ce->location = -1;

      expr_s->args = list_make2(ce, expr);
      aggregation_function = F_SUM_INT4;
    } else {
      expr_s->args =
        list_make2(((TargetEntry *)linitial(agg_ref->args))->expr, expr);
    }

    expr_s->location = -1;

    // aggregating all semirings in an array
    te_inner->resno = 1;
    te_inner->expr = (Expr *)expr_s;
    agg->aggfnoid = constants->OID_FUNCTION_ARRAY_AGG;
    agg->aggtype = constants->OID_TYPE_UUID_ARRAY;
    agg->args = list_make1(te_inner);
    agg->aggkind = AGGKIND_NORMAL;
    agg->location = -1;
#if PG_VERSION_NUM >= 140000
    agg->aggno = agg->aggtransno = -1;
#endif

    agg->aggargtypes = list_make1_oid(constants->OID_TYPE_UUID);

    // final aggregation function
    plus->funcid = constants->OID_FUNCTION_PROVENANCE_AGGREGATE;

    fn = makeConst(constants->OID_TYPE_INT, -1, InvalidOid, sizeof(int32),
                   Int32GetDatum(aggregation_function), false, true);

    typ = makeConst(constants->OID_TYPE_INT, -1, InvalidOid, sizeof(int32),
                    Int32GetDatum(agg_ref->aggtype), false, true);

    plus->funcresulttype = constants->OID_TYPE_AGG_TOKEN;
    plus->args = list_make4(fn, typ, agg_ref, agg);
    plus->location = -1;

    result = (Expr *)plus;
  }

  return result;
}

/* -------------------------------------------------------------------------
 * HAVING / WHERE-on-aggregates rewriting
 * ------------------------------------------------------------------------- */

/* Forward declaration needed because having_BoolExpr_to_provenance and
 * having_Expr_to_provenance_cmp are mutually recursive. */
static FuncExpr *having_Expr_to_provenance_cmp(Expr *expr, const constants_t *constants, bool negated);

/* Forward declaration: defined alongside the other tree walkers
 * further down in the file. */
static bool needs_having_lift(Node *havingQual, const constants_t *constants);

/**
 * @brief Convert a comparison @c OpExpr on aggregate results into a
 *        @c provenance_cmp gate expression.
 *
 * Each argument of @p opExpr must be one of:
 * - A @c Var of type @c agg_token (or a @c FuncExpr implicit-cast wrapper
 *   around one) → cast to UUID via @c agg_token_to_uuid.
 * - A scalar @c Const, or a bare grouped-column @c Var (necessarily a GROUP BY
 *   key in a HAVING clause, hence constant within each group) → wrapped in
 *   @c provenance_semimod(value, gate_one()).
 *
 * If @p negated is true the operator OID is replaced by its negator so that
 * NOT(a < b) becomes a >= b at the provenance level.
 *
 * @param opExpr     The comparison expression from the HAVING clause.
 * @param constants  Extension OID cache.
 * @param negated    Whether the expression appears under a NOT.
 * @return  A @c provenance_cmp(lhs, op_oid, rhs) @c FuncExpr.
 */
static FuncExpr *having_OpExpr_to_provenance_cmp(OpExpr *opExpr, const constants_t *constants, bool negated) {
  FuncExpr *cmpExpr;
  Node *arguments[2];
  Const *oid;
  Oid opno = opExpr->opno;

  for (unsigned i = 0; i < 2; ++i) {
    Node *node = (Node *)lfirst(list_nth_cell(opExpr->args, i));

    if (IsA(node, FuncExpr)) {
      FuncExpr *fe = (FuncExpr *)node;
      if (fe->funcformat == COERCE_IMPLICIT_CAST ||
          fe->funcformat == COERCE_EXPLICIT_CAST) {
        if (fe->args->length == 1)
          node = lfirst(list_head(fe->args));
      }
    }

    if ((IsA(node, FuncExpr) &&
         ((FuncExpr *)node)->funcid ==
           constants->OID_FUNCTION_PROVENANCE_AGGREGATE) ||
        (IsA(node, Var) &&
         ((Var *)node)->vartype == constants->OID_TYPE_AGG_TOKEN)) {
      // The aggregate side: add an explicit cast of the agg_token to UUID.
      FuncExpr *castToUUID = makeNode(FuncExpr);

      castToUUID->funcid = constants->OID_FUNCTION_AGG_TOKEN_UUID;
      castToUUID->funcresulttype = constants->OID_TYPE_UUID;
      castToUUID->args = list_make1(node);
      castToUUID->location = -1;

      arguments[i] = (Node *)castToUUID;
    } else if (IsA(node, Const) || IsA(node, Var)) {
      // The value side: a literal, or a bare grouped-column Var.  A non-agg_token
      // Var in HAVING is necessarily a GROUP BY key, hence constant within each
      // group, so it is wrapped exactly like a literal in a value gate carrying
      // the (per-group) datum with certain provenance.
      FuncExpr *oneExpr = makeNode(FuncExpr);
      FuncExpr *semimodExpr = makeNode(FuncExpr);

      // gate_one() expression
      oneExpr->funcid = constants->OID_FUNCTION_GATE_ONE;
      oneExpr->funcresulttype = constants->OID_TYPE_UUID;
      oneExpr->args = NIL;
      oneExpr->location = -1;

      // provenance_semimod(value, gate_one())
      semimodExpr->funcid = constants->OID_FUNCTION_PROVENANCE_SEMIMOD;
      semimodExpr->funcresulttype = constants->OID_TYPE_UUID;
      semimodExpr->args = list_make2((Expr *)node, (Expr *)oneExpr);
      semimodExpr->location = -1;

      arguments[i] = (Node *)semimodExpr;
    } else {
      provsql_error("cannot handle complex HAVING expressions");
    }
  }

  if (negated) {
    opno = get_negator(opno);
    if (!opno)
      provsql_error("Missing negator");
  }

  oid = makeConst(constants->OID_TYPE_INT, -1, InvalidOid, sizeof(int32),
                  Int32GetDatum(opno), false, true);

  cmpExpr = makeNode(FuncExpr);
  cmpExpr->funcid = constants->OID_FUNCTION_PROVENANCE_CMP;
  cmpExpr->funcresulttype = constants->OID_TYPE_UUID;
  cmpExpr->args = list_make3(arguments[0], oid, arguments[1]);
  cmpExpr->location = opExpr->location;

  return cmpExpr;
}

/**
 * @brief Convert a Boolean combination of HAVING comparisons into a
 *        @c provenance_times / @c provenance_plus gate expression.
 *
 * Applies De Morgan duality when @p negated is true: AND becomes
 * @c provenance_plus (OR) and vice-versa.  NOT is handled by flipping
 * @p negated and delegating to @c having_Expr_to_provenance_cmp.
 *
 * @param be         Boolean expression from the HAVING clause.
 * @param constants  Extension OID cache.
 * @param negated    Whether the expression appears under a NOT.
 * @return  A @c FuncExpr combining the sub-expressions.
 */
static FuncExpr *having_BoolExpr_to_provenance(BoolExpr *be, const constants_t *constants, bool negated) {
  if(be->boolop == NOT_EXPR) {
    Expr *expr = (Expr *) lfirst(list_head(be->args));
    return having_Expr_to_provenance_cmp(expr, constants, !negated);
  } else {
    FuncExpr *result;
    List *l = NULL;
    ListCell *lc;
    ArrayExpr *array = makeNode(ArrayExpr);

    array->array_typeid = constants->OID_TYPE_UUID_ARRAY;
    array->element_typeid = constants->OID_TYPE_UUID;
    array->location = -1;

    result = makeNode(FuncExpr);
    result->funcresulttype = constants->OID_TYPE_UUID;
    result->funcvariadic = true;
    result->location = be->location;
    result->args = list_make1(array);

    if ((be->boolop == AND_EXPR && !negated) || (be->boolop == OR_EXPR && negated))
      result->funcid = constants->OID_FUNCTION_PROVENANCE_TIMES;
    else if ((be->boolop == AND_EXPR && negated) || (be->boolop == OR_EXPR && !negated))
      result->funcid = constants->OID_FUNCTION_PROVENANCE_PLUS;
    else
      provsql_error("Unknown Boolean operator");

    foreach (lc, be->args) {
      Expr *expr = (Expr *)lfirst(lc);
      FuncExpr *arg = having_Expr_to_provenance_cmp(expr, constants, negated);
      l = lappend(l, arg);
    }

    array->elements = l;

    return result;
  }
}

/**
 * @brief Dispatch a HAVING sub-expression to the appropriate converter.
 *
 * Entry point for the mutual recursion between
 * @c having_BoolExpr_to_provenance and @c having_OpExpr_to_provenance_cmp.
 *
 * @param expr       Sub-expression to convert (@c BoolExpr or @c OpExpr).
 * @param constants  Extension OID cache.
 * @param negated    Whether the expression appears under a NOT.
 * @return  Converted @c FuncExpr.
 */
static FuncExpr *having_Expr_to_provenance_cmp(Expr *expr, const constants_t *constants, bool negated)
{
  if (IsA(expr, BoolExpr))
    return having_BoolExpr_to_provenance((BoolExpr *)expr, constants, negated);
  else if (IsA(expr, OpExpr))
    return having_OpExpr_to_provenance_cmp((OpExpr *)expr, constants, negated);
  else
    provsql_error("Unknown structure within Boolean expression");
}

/* -------------------------------------------------------------------------
 * Random-variable WHERE-clause rewriting
 *
 * Mirror of the HAVING trio above.  An OpExpr whose @c opfuncid matches
 * one of the @c random_variable_{eq,ne,le,lt,ge,gt} procedures is an
 * RV comparison; the planner hook lifts it out of @c jointree->quals,
 * builds an equivalent @c provenance_cmp(left_uuid, op_oid, right_uuid)
 * @c FuncExpr, and conjoins the resulting UUID into the row's
 * provenance via @c provenance_times.  The lifted WHERE conjunct is
 * removed (or the whole WHERE replaced by @c NULL when only RV cmps
 * were present); what remains is purely Boolean and the executor
 * evaluates it in the usual way.  The RV-cmp operators themselves are
 * boolean placeholders -- their procedure raises if reached, which can
 * happen only when the planner hook is bypassed (e.g. provsql.active
 * off).
 * ------------------------------------------------------------------------- */

/**
 * @brief Test whether @p funcoid is one of the @c random_variable_*
 *        comparison procedures, and if so return its
 *        @c ComparisonOperator index.
 *
 * @param  constants  Extension OID cache.
 * @param  funcoid    Procedure OID to test (typically @c OpExpr->opfuncid).
 * @return Index in @c [0..6) on match, @c -1 otherwise.  Match indices
 *         line up with @c ComparisonOperator (EQ=0, NE=1, LE=2, LT=3,
 *         GE=4, GT=5).
 */
static int rv_cmp_index(const constants_t *constants, Oid funcoid)
{
  for (int i = 0; i < 6; ++i) {
    if (funcoid == constants->OID_FUNCTION_RV_CMP[i])
      return i;
  }
  return -1;
}

/**
 * @brief Wrap an expression returning @c random_variable in a
 *        binary-coercible cast to @c uuid.
 *
 * Operand of the comparison may be a Var, a constant lifted by an
 * implicit cast, or another OpExpr (e.g. <tt>a + b</tt>).
 * @c random_variable and @c uuid share the same byte layout, so we
 * emit a @c RelabelType node -- the planner sees a zero-cost type
 * relabel, the executor never dispatches through a runtime
 * conversion function.
 */
static Expr *
wrap_random_variable_uuid(Node *operand, const constants_t *constants)
{
  RelabelType *rt = makeNode(RelabelType);
  rt->arg = (Expr *) operand;
  rt->resulttype = constants->OID_TYPE_UUID;
  rt->resulttypmod = -1;
  rt->resultcollid = InvalidOid;
  rt->relabelformat = COERCE_IMPLICIT_CAST;
  rt->location = -1;
  return (Expr *) rt;
}

/* Forward declaration: the BoolExpr and Expr walkers below are mutually
 * recursive (BoolExpr recurses into Expr for each AND/OR child). */
static FuncExpr *rv_Expr_to_provenance(Expr *expr,
                                       const constants_t *constants,
                                       bool negated);

/**
 * @brief Convert a single RV-comparison @c OpExpr into a
 *        @c provenance_cmp() FuncExpr returning UUID.
 *
 * If @p negated is true the operator OID is replaced by its negator
 * (so <tt>NOT (a &gt; b)</tt> becomes <tt>a &le; b</tt> at the
 * provenance level), exactly as @c having_OpExpr_to_provenance_cmp
 * does.
 *
 * @param opExpr    The comparison expression from the WHERE clause.
 *                  Must satisfy @c rv_cmp_index(opExpr->opfuncid) &ge; 0;
 *                  callers are responsible for the type check.
 * @param constants Extension OID cache.
 * @param negated   Whether the expression appears under a NOT.
 */
static FuncExpr *
rv_OpExpr_to_provenance_cmp(OpExpr *opExpr, const constants_t *constants,
                            bool negated)
{
  FuncExpr *cmpExpr;
  Const    *oid_const;
  Oid       opno = opExpr->opno;
  Node     *left = (Node *)linitial(opExpr->args);
  Node     *right = (Node *)lsecond(opExpr->args);

  if (negated) {
    opno = get_negator(opno);
    if (!opno)
      provsql_error("Missing negator for random_variable comparison");
  }

  oid_const = makeConst(constants->OID_TYPE_INT, -1, InvalidOid,
                        sizeof(int32), Int32GetDatum(opno), false, true);

  cmpExpr = makeNode(FuncExpr);
  cmpExpr->funcid = constants->OID_FUNCTION_PROVENANCE_CMP;
  cmpExpr->funcresulttype = constants->OID_TYPE_UUID;
  cmpExpr->args = list_make3(
    wrap_random_variable_uuid(left, constants),
    oid_const,
    wrap_random_variable_uuid(right, constants));
  cmpExpr->location = opExpr->location;

  return cmpExpr;
}

/**
 * @brief Convert a Boolean combination of RV comparisons into a
 *        @c provenance_times / @c provenance_plus expression.
 *
 * Same De Morgan handling as @c having_BoolExpr_to_provenance: under
 * negation, AND ↔ OR (which means PROVENANCE_TIMES ↔ PROVENANCE_PLUS).
 * NOT flips @c negated and recurses.
 */
static FuncExpr *
rv_BoolExpr_to_provenance(BoolExpr *be, const constants_t *constants,
                          bool negated)
{
  FuncExpr   *result;
  ArrayExpr  *array;
  List       *l = NIL;
  ListCell   *lc;

  if (be->boolop == NOT_EXPR) {
    Expr *child = (Expr *)linitial(be->args);
    return rv_Expr_to_provenance(child, constants, !negated);
  }

  array = makeNode(ArrayExpr);
  array->array_typeid = constants->OID_TYPE_UUID_ARRAY;
  array->element_typeid = constants->OID_TYPE_UUID;
  array->location = -1;

  result = makeNode(FuncExpr);
  result->funcresulttype = constants->OID_TYPE_UUID;
  result->funcvariadic = true;
  result->location = be->location;
  result->args = list_make1(array);

  if ((be->boolop == AND_EXPR && !negated) ||
      (be->boolop == OR_EXPR  && negated))
    result->funcid = constants->OID_FUNCTION_PROVENANCE_TIMES;
  else if ((be->boolop == AND_EXPR && negated) ||
           (be->boolop == OR_EXPR  && !negated))
    result->funcid = constants->OID_FUNCTION_PROVENANCE_PLUS;
  else
    provsql_error("Unknown Boolean operator in random_variable WHERE clause");

  foreach (lc, be->args) {
    FuncExpr *arg = rv_Expr_to_provenance((Expr *)lfirst(lc),
                                          constants, negated);
    l = lappend(l, arg);
  }
  array->elements = l;

  return result;
}

/**
 * @brief Dispatch a WHERE sub-expression to the appropriate RV converter.
 *
 * Entry point for the mutual recursion between
 * @c rv_BoolExpr_to_provenance and @c rv_OpExpr_to_provenance_cmp.
 */
static FuncExpr *
rv_Expr_to_provenance(Expr *expr, const constants_t *constants, bool negated)
{
  if (IsA(expr, BoolExpr))
    return rv_BoolExpr_to_provenance((BoolExpr *)expr, constants, negated);
  if (IsA(expr, OpExpr)) {
    OpExpr *opExpr = (OpExpr *)expr;
    if (rv_cmp_index(constants, opExpr->opfuncid) >= 0)
      return rv_OpExpr_to_provenance_cmp(opExpr, constants, negated);
  }
  provsql_error("Unsupported sub-expression in random_variable WHERE clause "
                "(only Boolean combinations of RV comparisons are accepted)");
  return NULL; /* unreachable, silences -Wreturn-type */
}

/**
 * @brief Test whether an Expr (sub-)tree contains any RV comparison.
 *
 * Used by the WHERE-clause extractor to decide whether a top-level
 * conjunct mentions any random_variable comparator and therefore
 * needs lifting (or, if the conjunct mixes RV and non-RV operators
 * in a way we cannot rewrite, errors).
 */
static bool
expr_contains_rv_cmp(Node *node, const constants_t *constants)
{
  if (node == NULL)
    return false;
  if (IsA(node, OpExpr)) {
    OpExpr *opExpr = (OpExpr *)node;
    if (rv_cmp_index(constants, opExpr->opfuncid) >= 0)
      return true;
  }
  if (IsA(node, BoolExpr)) {
    BoolExpr *be = (BoolExpr *)node;
    ListCell *lc;
    foreach (lc, be->args) {
      if (expr_contains_rv_cmp(lfirst(lc), constants))
        return true;
    }
    return false;
  }
  return false;
}

/**
 * @brief Test whether @p expr is a Boolean combination of @em only
 *        random_variable comparisons (no other leaves allowed).
 *
 * Mirrors @c check_expr_on_aggregate / @c check_boolexpr_on_aggregate
 * for the agg_token WHERE-to-HAVING migration path.  Recursively
 * accepts:
 * - @c BoolExpr (AND/OR/NOT) all of whose children pass; and
 * - @c OpExpr matching one of the @c random_variable_* comparators.
 *
 * Anything else (a non-RV @c OpExpr, a @c Var, a @c Const, a non-cmp
 * @c FuncExpr) makes the expression mixed and unsupportable by the
 * RV-only walker, so the function returns @c false and the caller
 * raises a clear error.
 */
static bool
check_expr_on_rv(Expr *expr, const constants_t *constants)
{
  if (expr == NULL)
    return false;
  if (IsA(expr, OpExpr))
    return rv_cmp_index(constants, ((OpExpr *)expr)->opfuncid) >= 0;
  if (IsA(expr, BoolExpr)) {
    BoolExpr *be = (BoolExpr *)expr;
    ListCell *lc;
    foreach (lc, be->args) {
      if (!check_expr_on_rv((Expr *)lfirst(lc), constants))
        return false;
    }
    return true;
  }
  return false;
}

/* The earlier RV-only WHERE walker (@c extract_rv_cmps_from_quals)
 * has been folded into the unified classifier
 * @c migrate_probabilistic_quals further down in this file; both the
 * agg_token and the random_variable migration paths are now special
 * cases of one walk over @c q->jointree->quals.  See the comment on
 * @c qual_class for the routing matrix. */

/**
 * @brief Build the combined provenance expression to be added to the SELECT list.
 *
 * Combines the tokens in @p prov_atts according to @p op:
 * - @c SR_PLUS  → use the first token directly (union branch; the outer
 *                  @c array_agg / @c provenance_plus is added later if needed).
 * - @c SR_TIMES → wrap all tokens in @c provenance_times(...).
 * - @c SR_MONUS → wrap all tokens in @c provenance_monus(...).
 *
 * When @p aggregation or @p group_by_rewrite is true, wraps the result in
 * @c array_agg + @c provenance_plus to collapse groups.  A @c provenance_delta
 * gate is added for plain aggregations without a HAVING clause.
 *
 * If a HAVING clause is present it is removed from @p q->havingQual and
 * converted into a provenance expression via @c having_Expr_to_provenance_cmp.
 *
 * If @c provsql_where_provenance is enabled, equality gates (@c provenance_eq)
 * are prepended for join conditions and WHERE equalities, and a projection gate
 * is appended if the output columns form a proper subset of the input columns.
 *
 * @param constants        Extension OID cache.
 * @param q                Query being rewritten (HAVING is cleared if present).
 * @param prov_atts        List of provenance @c Var nodes.
 * @param aggregation      True if the query contains aggregate functions.
 * @param group_by_rewrite True if a GROUP BY requires the plus-aggregate wrapper.
 * @param op               Semiring operation to use for combining tokens.
 * @param columns          Per-RTE column-numbering array (for where-provenance).
 *                         For provenance-tracked @c RTE_RELATION entries, the
 *                         -1 sentinel is used to identify them; the PROJECT
 *                         gate positions for their columns use @c varattno
 *                         rather than the query-order-dependent sequential
 *                         numbers (see @c build_column_map() for the
 *                         rationale).
 * @param nbcols           Total number of non-provenance output columns.
 * @param wrap_assumed_boolean If true, wrap the result in
 *                         @c provenance_assumed_boolean so downstream
 *                         probability evaluators may treat it as Boolean.
 * @param inv_cert         If non-NULL, a serialised inversion-free certificate
 *                         to attach to the per-row root via @c provsql.annotate
 *                         (transparent for every evaluator; read back by the
 *                         probability dispatcher).  Mutually compatible with
 *                         @c wrap_assumed_boolean only in principle -- the
 *                         inversion-free path never sets the latter.
 * @return  The provenance @c Expr to be appended to the target list.
 */
static Expr *make_provenance_expression(const constants_t *constants, Query *q,
                                        List *prov_atts, bool aggregation,
                                        bool group_by_rewrite,
                                        semiring_operation op, int **columns,
                                        int nbcols, bool wrap_assumed_boolean,
                                        const char *inv_cert) {
  Expr *result;
  ListCell *lc_v;

  if (op == SR_PLUS) {
    result = linitial(prov_atts);
  } else {
    if (my_lnext(prov_atts, list_head(prov_atts)) == NULL) {
      result = linitial(prov_atts);
    } else {
      FuncExpr *expr = makeNode(FuncExpr);
      if (op == SR_TIMES) {
        ArrayExpr *array = makeNode(ArrayExpr);

        expr->funcid = constants->OID_FUNCTION_PROVENANCE_TIMES;
        expr->funcvariadic = true;

        array->array_typeid = constants->OID_TYPE_UUID_ARRAY;
        array->element_typeid = constants->OID_TYPE_UUID;
        array->elements = prov_atts;
        array->location = -1;

        expr->args = list_make1(array);
      } else { // SR_MONUS
        expr->funcid = constants->OID_FUNCTION_PROVENANCE_MONUS;
        expr->args = prov_atts;
      }
      expr->funcresulttype = constants->OID_TYPE_UUID;
      expr->location = -1;

      result = (Expr *)expr;
    }

    if (group_by_rewrite || aggregation) {
      Aggref *agg = makeNode(Aggref);
      FuncExpr *plus = makeNode(FuncExpr);
      TargetEntry *te_inner = makeNode(TargetEntry);

      q->hasAggs = true;

      te_inner->resno = 1;
      te_inner->expr = (Expr *)result;

      agg->aggfnoid = constants->OID_FUNCTION_ARRAY_AGG;
      agg->aggtype = constants->OID_TYPE_UUID_ARRAY;
      agg->args = list_make1(te_inner);
      agg->aggkind = AGGKIND_NORMAL;
      agg->location = -1;
#if PG_VERSION_NUM >= 140000
      agg->aggno = agg->aggtransno = -1;
#endif

      agg->aggargtypes = list_make1_oid(constants->OID_TYPE_UUID);

      plus->funcid = constants->OID_FUNCTION_PROVENANCE_PLUS;
      plus->args = list_make1(agg);
      plus->funcresulttype = constants->OID_TYPE_UUID;
      plus->location = -1;

      result = (Expr *)plus;
    }

    /* HAVING quals come in two flavours.  A qual that references an
     * agg_token Var or a provenance_aggregate() wrapper must be lifted
     * into a provenance_cmp gate so the per-group truth value is
     * carried by the provenance circuit (and the corresponding gate_agg
     * remains evaluable).  Anything else -- a deterministic scalar
     * predicate, or one over random_variable aggregates collapsed by
     * expected() / variance() / moment() to a plain double -- is left
     * in q->havingQual for PostgreSQL to evaluate natively, and the
     * per-group provenance still gets a delta wrapper. */
    {
      bool lift_having = q->havingQual != NULL &&
                         needs_having_lift((Node *) q->havingQual, constants);

      if (aggregation && !lift_having) {
        FuncExpr *deltaExpr = makeNode(FuncExpr);

        // adding the delta gate to the provenance circuit
        deltaExpr->funcid = constants->OID_FUNCTION_PROVENANCE_DELTA;
        deltaExpr->args = list_make1(result);
        deltaExpr->funcresulttype = constants->OID_TYPE_UUID;
        deltaExpr->location = -1;

        result = (Expr *)deltaExpr;
      }

      if (lift_having) {
        result = (Expr*) having_Expr_to_provenance_cmp((Expr*)q->havingQual, constants, false);
        q->havingQual = NULL;
      }
    }
  }

  /* Part to handle eq gates used for where-provenance.
   * Placed before projection gates because they need
   * to be deeper in the provenance tree. */
  if (provsql_where_provenance && q->jointree) {
    ListCell *lc;
    foreach (lc, q->jointree->fromlist) {
      if (IsA(lfirst(lc), JoinExpr)) {
        JoinExpr *je = (JoinExpr *)lfirst(lc);
        /* Study equalities coming from From clause */
        result =
          add_eq_from_Quals_to_Expr(constants, je->quals, result, columns);
      }
    }
    /* Study equalities coming from WHERE clause */
    result = add_eq_from_Quals_to_Expr(constants, q->jointree->quals, result,
                                       columns);
  }

  if (provsql_where_provenance) {
    ArrayExpr *array = makeNode(ArrayExpr);
    FuncExpr *fe = makeNode(FuncExpr);
    bool projection = false;
    int nb_column = 0;
    /* Cumulative offset of each RTE within the TIMES gate's concatenated
     * locator vector.  WhereCircuit::evaluate(TIMES) appends the locator
     * vector of each child input in q->rtable order, so a column at
     * varattno k of the i-th provenance-tracked base RTE lands at
     * prov_offset[i] + k in the concat.  varattno alone (the recent fix
     * documented at the top of this file) is correct only when there is a
     * single provenance-tracked input; for multi-input joins it omits the
     * preceding inputs' nb_user_cols and the project gate then reads from
     * the wrong table's locator slice.
     *
     * 1-indexed by rteid for direct indexing via Var->varno; entry 0 is
     * unused.  Length q->rtable->length + 1. */
    int *prov_offset = (int *)palloc0((q->rtable->length + 1) * sizeof(int));
    int cum = 0;
    Index r;

    fe->funcid = constants->OID_FUNCTION_PROVENANCE_PROJECT;
    fe->funcvariadic = true;
    fe->funcresulttype = constants->OID_TYPE_UUID;
    fe->location = -1;

    array->array_typeid = constants->OID_TYPE_INT_ARRAY;
    array->element_typeid = constants->OID_TYPE_INT;
    array->elements = NIL;
    array->location = -1;

    for (r = 1; r <= (Index)q->rtable->length; ++r) {
      prov_offset[r] = cum;
      if (columns[r-1]) {
        RangeTblEntry *rte_r = (RangeTblEntry *)list_nth(q->rtable, r-1);
        int ncols = list_length(rte_r->eref->colnames);
        bool is_prov = false;
        int nb_user = 0;
        int k;
        for (k = 0; k < ncols; ++k) {
          if (columns[r-1][k] == -1) is_prov = true;
          else if (columns[r-1][k] > 0) nb_user++;
        }
        if (is_prov) cum += nb_user;
      }
    }

    foreach (lc_v, q->targetList) {
      TargetEntry *te_v = (TargetEntry *)lfirst(lc_v);
      if (IsA(te_v->expr, Var)) {
        Var *vte_v = (Var *)te_v->expr;
        RangeTblEntry *rte_v =
          (RangeTblEntry *)lfirst(list_nth_cell(q->rtable, vte_v->varno - 1));
        int value_v;
#if PG_VERSION_NUM >= 180000
        if (rte_v->rtekind == RTE_GROUP) {
          Expr *ge = lfirst(list_nth_cell(rte_v->groupexprs, vte_v->varattno - 1));
          if(IsA(ge, Var)) {
            Var *v = (Var *) ge;
            value_v = columns[v->varno - 1] ?
                      columns[v->varno - 1][v->varattno - 1] : 0;
          } else {
            Const *ce = makeConst(constants->OID_TYPE_INT, -1, InvalidOid,
                                  sizeof(int32), Int32GetDatum(0), false, true);

            array->elements = lappend(array->elements, ce);
            value_v = 0;
          }
        } else
#endif
        if (rte_v->rtekind != RTE_JOIN) { // Normal RTE
          if (rte_v->rtekind == RTE_RELATION && columns[vte_v->varno - 1]) {
            /* Determine whether this base table is provenance-tracked by
             * scanning for the sentinel -1 entry that build_column_map()
             * assigns to the provsql column. */
            bool is_prov = false;
            int ncols_rte = list_length(rte_v->eref->colnames);
            for (int k = 0; k < ncols_rte; k++) {
              if (columns[vte_v->varno - 1][k] == -1) {
                is_prov = true;
                break;
              }
            }
            if (is_prov) {
              int raw = columns[vte_v->varno - 1][vte_v->varattno - 1];
              /* Local position within this table is `varattno` (the
               * provsql column is appended last by add_provenance(), so
               * user columns occupy 1..nb_user_cols exactly matching the
               * IN gate's Locator vector).  We then shift by
               * prov_offset[varno] to land in the right slice of the
               * TIMES gate's concatenated locator vector when the query
               * joins multiple provenance-tracked relations. */
              value_v = (raw == -1) ? -1
                                    : (int)vte_v->varattno
                        + prov_offset[vte_v->varno];
            } else {
              /* Non-provenance base table: no IN gate exists for it, so
               * the position would be out of range regardless.  Explicitly
               * record 0 so evaluate() returns an empty locator set and
               * the positions array stays in sync with the output column
               * count. */
              Const *ce =
                makeConst(constants->OID_TYPE_INT, -1, InvalidOid,
                          sizeof(int32), Int32GetDatum(0), false, true);
              array->elements = lappend(array->elements, ce);
              projection = true;
              continue;
            }
          } else {
            /* RTE_SUBQUERY and others: the sequential number equals the
            * column's 1-indexed position in the subquery's output list,
            * which matches what the child gate's evaluate() expects. */
            value_v = columns[vte_v->varno - 1] ?
                      columns[vte_v->varno - 1][vte_v->varattno - 1] : 0;
          }
        } else { // Join RTE
          Var *jav_v = (Var *)lfirst(
            list_nth_cell(rte_v->joinaliasvars, vte_v->varattno - 1));
          if (jav_v && IsA(jav_v, Var) && columns[jav_v->varno - 1]) {
            RangeTblEntry *jrte_v = (RangeTblEntry *)lfirst(
              list_nth_cell(q->rtable, jav_v->varno - 1));
            if (jrte_v->rtekind == RTE_RELATION) {
              /* Provenance-tracking check and varattno fix — same rationale
               * as the RTE_RELATION branch above. */
              bool is_prov = false;
              int ncols_jrte = list_length(jrte_v->eref->colnames);
              for (int k = 0; k < ncols_jrte; k++) {
                if (columns[jav_v->varno - 1][k] == -1) {
                  is_prov = true;
                  break;
                }
              }
              if (is_prov) {
                int raw = columns[jav_v->varno - 1][jav_v->varattno - 1];
                value_v = (raw == -1) ? -1
                                      : (int)jav_v->varattno
                          + prov_offset[jav_v->varno];
              } else {
                Const *ce =
                  makeConst(constants->OID_TYPE_INT, -1, InvalidOid,
                            sizeof(int32), Int32GetDatum(0), false, true);
                array->elements = lappend(array->elements, ce);
                projection = true;
                continue;
              }
            } else {
              value_v = columns[jav_v->varno - 1][jav_v->varattno - 1];
            }
          } else {
            value_v = 0;
          }
        }

        /* If this is a valid column */
        if (value_v > 0) {
          Const *ce =
            makeConst(constants->OID_TYPE_INT, -1, InvalidOid, sizeof(int32),
                      Int32GetDatum(value_v), false, true);

          array->elements = lappend(array->elements, ce);

          if (value_v != ++nb_column)
            projection = true;
        } else {
          if (value_v != -1)
            projection = true;
        }
      } else { // we have a function in target
        Const *ce = makeConst(constants->OID_TYPE_INT, -1, InvalidOid,
                              sizeof(int32), Int32GetDatum(0), false, true);

        array->elements = lappend(array->elements, ce);
        projection = true;
      }
    }

    if (nb_column != nbcols)
      projection = true;

    if (projection) {
      fe->args = list_make2(result, array);
      result = (Expr *)fe;
    } else {
      pfree(array);
      pfree(fe);
    }
  }

  /* Wrap the finished per-row root in a @c gate_assumed_boolean when
   * our caller (the safe-query rewrite path in @c process_query) asks
   * for it.  Wrapping here -- before @c add_to_select and
   * @c replace_provenance_function_by_expression -- means every
   * per-row root reference in the final target list carries the
   * marker uniformly.  Subqueries that this same Query body opens
   * (per-atom DISTINCT projections inserted by the rewriter) are
   * handled by their own deeper @c process_query / @c make_provenance_expression
   * calls with @c wrap_assumed_boolean = false, so the marker sits
   * only at the outermost root that surfaces as the user-visible
   * row provenance. */
  if (wrap_assumed_boolean &&
      OidIsValid(constants->OID_FUNCTION_ASSUME_BOOLEAN))
    result = wrap_in_assume_boolean(constants, result);

  /* Attach the inversion-free tractability certificate to the per-row root.
   * The annotation gate is transparent for every evaluator; the probability
   * dispatcher reads the certificate back from its extra. */
  if (inv_cert != NULL &&
      OidIsValid(constants->OID_FUNCTION_ANNOTATE))
    result = wrap_in_annotate(constants, result, inv_cert);

  return result;
}

/* -------------------------------------------------------------------------
 * Set-operation & DISTINCT rewriting
 * ------------------------------------------------------------------------- */

#if PG_VERSION_NUM >= 180000
typedef struct {
  Index group_rtindex;
  List *groupexprs;
} resolve_group_rte_ctx;

static Node *
resolve_group_rte_vars_mutator(Node *node, void *raw_ctx) {
  resolve_group_rte_ctx *ctx = (resolve_group_rte_ctx *)raw_ctx;
  if (node == NULL)
    return NULL;
  if (IsA(node, Var)) {
    Var *v = (Var *)node;
    if (v->varno == ctx->group_rtindex) {
      Node *resolved = copyObject(list_nth(ctx->groupexprs, v->varattno - 1));
      /* Clear varnullingrels: the group-step nulling bits reference the
       * group_rtindex RTE which does not exist in the fresh inner query.
       * Leaving them set causes the planner to access simple_rel_array at
       * group_rtindex (which has no RelOptInfo), triggering
       * "unrecognized RTE kind: 9". */
      if (IsA(resolved, Var))
        ((Var *)resolved)->varnullingrels = NULL;
      return resolved;
    }
  }
  return expression_tree_mutator(node, resolve_group_rte_vars_mutator, raw_ctx);
}

/**
 * @brief Strip PG 18's virtual @c RTE_GROUP entry from @p q in place.
 *
 * @c parseCheckAggregates() appends an @c RTE_GROUP entry at the end of
 * @c q->rtable whenever the query has a @c GROUP @c BY clause; references
 * to grouped columns in @c targetList and @c jointree->quals point at that
 * synthetic RTE rather than the underlying base tables.  ProvSQL's
 * rewriters need a flat range-table to do their own index arithmetic, so
 * we remove the @c RTE_GROUP and resolve every @c Var(@c group_rtindex,
 * @c i) back to its base-table expression before going further.
 *
 * Idempotent: when @c q->hasGroupRTE is already false, returns without
 * doing anything.
 */
void strip_group_rte_pg18(Query *q) {
  resolve_group_rte_ctx grp_ctx;
  bool found = false;
  ListCell *lc;
  Index idx = 1;
  int rte_len = 0;

  if (!q->hasGroupRTE)
    return;

  foreach (lc, q->rtable) {
    RangeTblEntry *r = (RangeTblEntry *) lfirst(lc);
    if (r->rtekind == RTE_GROUP) {
      grp_ctx.group_rtindex = idx;
      grp_ctx.groupexprs    = r->groupexprs;
      found    = true;
      rte_len  = idx - 1;
      break;
    }
    idx++;
  }

  if (!found)
    return;

  q->rtable      = list_truncate(q->rtable, rte_len);
  q->hasGroupRTE = false;

  foreach (lc, q->targetList) {
    TargetEntry *te = (TargetEntry *) lfirst(lc);
    te->expr = (Expr *) resolve_group_rte_vars_mutator(
      (Node *) te->expr, &grp_ctx);
  }
  if (q->jointree && q->jointree->quals)
    q->jointree->quals = resolve_group_rte_vars_mutator(
      q->jointree->quals, &grp_ctx);
  /* HAVING too: when the GROUP BY key is a constant (e.g. GROUP BY 1, or
   * any literal grouping expression), PostgreSQL 18 rewrites a matching
   * literal on the other side of a HAVING comparison (HAVING count(*) = 1)
   * into a grouped Var referencing the RTE_GROUP entry.  Left unresolved
   * it reaches having_OpExpr_to_provenance_cmp as a bare Var and trips the
   * "cannot handle complex HAVING expressions" bail; resolving it back to
   * the underlying grouping expression restores the Const the converter
   * expects. */
  if (q->havingQual)
    q->havingQual = resolve_group_rte_vars_mutator(q->havingQual, &grp_ctx);
}
#endif

/* Forward declaration — defined later but needed by rewrite_agg_distinct */
static bool provenance_function_walker(Node *node, void *data);

/**
 * @brief Build the inner GROUP-BY subquery for one @c AGG(DISTINCT key).
 *
 * Produces:
 * @code
 *   SELECT key_expr, gb_col1, gb_col2, ...
 *   FROM   <same tables as q>
 *   GROUP BY key_expr, gb_col1, gb_col2, ...
 * @endcode
 *
 * @param q           Original query (supplies FROM / WHERE).
 * @param key_expr    The DISTINCT argument expression.
 * @param groupby_tes Non-aggregate target entries that are GROUP BY columns.
 * @return  Fresh inner @c Query.
 */
static Query *build_inner_for_distinct_key(Query *q, Expr *key_expr,
                                           List *groupby_tes) {
  Query *inner;
  List *new_tl = NIL;
  List *new_gc = NIL;
  ListCell *lc;
  int resno = 1, sgref = 1;

  inner = copyObject(q);

  inner->hasAggs    = false;
  inner->sortClause = NIL;
  inner->limitCount = NULL;
  inner->limitOffset = NULL;
  inner->distinctClause = NIL;
  inner->hasDistinctOn = false;
  inner->havingQual  = NULL;

  /* First column: the DISTINCT key */
  {
    TargetEntry *kte = makeNode(TargetEntry);
    SortGroupClause *sgc = makeNode(SortGroupClause);

    kte->expr   = copyObject(key_expr);
    kte->resno  = resno++;
    kte->resname = "key";
    sgc->tleSortGroupRef = kte->ressortgroupref = sgref++;
    get_sort_group_operators(exprType((Node *)kte->expr), true, true, false,
                             &sgc->sortop, &sgc->eqop, NULL, &sgc->hashable);
    new_gc = list_make1(sgc);
    new_tl = list_make1(kte);
  }

  /* Remaining columns: GROUP BY columns from the original query */
  foreach (lc, groupby_tes) {
    TargetEntry *gyte = copyObject((TargetEntry *)lfirst(lc));
    SortGroupClause *sgc = makeNode(SortGroupClause);

    gyte->resno   = resno++;
    gyte->resjunk = false;
    sgc->tleSortGroupRef = gyte->ressortgroupref = sgref++;
    get_sort_group_operators(exprType((Node *)gyte->expr), true, true, false,
                             &sgc->sortop, &sgc->eqop, NULL, &sgc->hashable);
    new_gc = lappend(new_gc, sgc);
    new_tl = lappend(new_tl, gyte);
  }

  inner->targetList  = new_tl;
  inner->groupClause = new_gc;
  return inner;
}

/**
 * @brief Wrap @p inner in an outer query that applies the original aggregate.
 *
 * Produces:
 * @code
 *   SELECT AGG(key_col), gb_col1, gb_col2, ...
 *   FROM   inner
 *   GROUP BY gb_col1, gb_col2, ...
 * @endcode
 * The DISTINCT flag is cleared; @p inner provides exactly one row per
 * (key, group-by) combination, so the plain aggregate gives the right count.
 *
 * @param orig_agg_te  Original @c TargetEntry containing @c AGG(DISTINCT key).
 * @param inner        Inner query from @c build_inner_for_distinct_key.
 * @param n_gb         Number of GROUP BY columns (trailing entries in @p inner).
 * @param constants    Extension OID cache.
 * @return  Fresh outer @c Query.
 */
static Query *build_outer_for_distinct_key(TargetEntry *orig_agg_te,
                                           Query *inner, int n_gb,
                                           const constants_t *constants) {
  Query *outer = makeNode(Query);
  RangeTblEntry *rte = makeNode(RangeTblEntry);
  Alias *alias = makeNode(Alias), *eref = makeNode(Alias);
  RangeTblRef *rtr = makeNode(RangeTblRef);
  FromExpr *jt = makeNode(FromExpr);
  List *new_tl = NIL, *new_gc = NIL;
  ListCell *lc;
  int resno = 1, sgref = 1;
  int inner_len = list_length(inner->targetList);
  int attno;

  /* Wrap inner in a subquery RTE */
  alias->aliasname = eref->aliasname = "d";
  eref->colnames = NIL;
  foreach (lc, inner->targetList) {
    TargetEntry *te = lfirst(lc);
    eref->colnames = lappend(eref->colnames,
                             makeString(te->resname ? pstrdup(te->resname) : ""));
  }
  rte->alias   = alias;
  rte->eref    = eref;
  rte->rtekind = RTE_SUBQUERY;
  rte->subquery = inner;
  rte->inFromCl = true;
#if PG_VERSION_NUM < 160000
  rte->requiredPerms = ACL_SELECT;
#endif

  rtr->rtindex = 1;
  jt->fromlist = list_make1(rtr);

  outer->commandType = CMD_SELECT;
  outer->canSetTag   = true;
  outer->rtable      = list_make1(rte);
  outer->jointree    = jt;
  outer->hasAggs     = true;

  /* First output column: the aggregate over the key (col 1 of inner) */
  {
    TargetEntry *agg_te = copyObject(orig_agg_te);
    Aggref *ar = (Aggref *)agg_te->expr;
    Var *key_var = makeNode(Var);
    TargetEntry *arg_te = makeNode(TargetEntry);

    key_var->varno      = 1;
    key_var->varattno   = 1;    /* key is first column of inner */
    key_var->vartype    = linitial_oid(ar->aggargtypes);
    key_var->varcollid  = exprCollation((Node *)((TargetEntry *)linitial(ar->args))->expr);
    key_var->vartypmod  = -1;
    key_var->location   = -1;
    arg_te->resno = 1;
    arg_te->expr  = (Expr *)key_var;

    ar->args        = list_make1(arg_te);
    ar->aggdistinct = NIL;
    agg_te->resno   = resno++;
    new_tl = list_make1(agg_te);
  }

  /* Remaining output columns: GROUP BY cols (trailing cols of inner) */
  for (attno = inner_len - n_gb + 1; attno <= inner_len; attno++) {
    TargetEntry *inner_te = list_nth(inner->targetList, attno - 1);
    Var *gb_var = makeNode(Var);
    TargetEntry *gb_te = makeNode(TargetEntry);
    SortGroupClause *sgc = makeNode(SortGroupClause);

    gb_var->varno      = 1;
    gb_var->varattno   = attno;
    gb_var->vartype    = exprType((Node *)inner_te->expr);
    gb_var->varcollid  = exprCollation((Node *)inner_te->expr);
    gb_var->vartypmod  = -1;
    gb_var->location   = -1;

    gb_te->resno   = resno++;
    gb_te->expr    = (Expr *)gb_var;
    gb_te->resname = inner_te->resname;

    sgc->tleSortGroupRef = gb_te->ressortgroupref = sgref++;
    sgc->nulls_first = false;
    get_sort_group_operators(gb_var->vartype, true, true, false,
                             &sgc->sortop, &sgc->eqop, NULL, &sgc->hashable);
    new_gc = lappend(new_gc, sgc);
    new_tl = lappend(new_tl, gb_te);
  }

  outer->targetList  = new_tl;
  outer->groupClause = new_gc;
  return outer;
}

/** @brief Collector for @c AGG(DISTINCT) Aggrefs inside a HAVING clause. */
typedef struct having_distinct_ctx {
  List *aggs;   ///< Aggref* nodes carrying @c aggdistinct, in traversal order
} having_distinct_ctx;

/**
 * @brief Walker that collects @c AGG(DISTINCT) Aggrefs from an expression.
 *
 * Does not descend into an @c Aggref's own arguments, so the traversal order
 * matches @c replace_having_distinct_mutator below (both stop at every
 * @c Aggref), keeping the per-aggregate outer-subquery indices aligned.
 */
static bool collect_having_distinct_walker(Node *node, void *ctx) {
  if (node == NULL)
    return false;
  if (IsA(node, Aggref)) {
    Aggref *ar = (Aggref *) node;
    if (list_length(ar->aggdistinct) > 0)
      ((having_distinct_ctx *) ctx)->aggs =
        lappend(((having_distinct_ctx *) ctx)->aggs, ar);
    return false;  /* don't recurse into aggregate arguments */
  }
  return expression_tree_walker(node, collect_having_distinct_walker, ctx);
}

/** @brief Context for @c replace_having_distinct_mutator: next outer RT index. */
typedef struct having_replace_ctx {
  int next_rtindex;
} having_replace_ctx;

/**
 * @brief Mutator that replaces each @c AGG(DISTINCT) Aggref in a HAVING
 *        clause with @c Var(next_rtindex++, 1) -- the deduped count column of
 *        its outer subquery (built in the same order by
 *        @c rewrite_agg_distinct).  The @c Var is typed as the aggregate's
 *        result so the surrounding comparison is intercepted by the HAVING
 *        provenance path exactly as a non-DISTINCT count would be.
 */
static Node *replace_having_distinct_mutator(Node *node, void *ctx) {
  if (node == NULL)
    return NULL;
  if (IsA(node, Aggref)) {
    Aggref *ar = (Aggref *) node;
    if (list_length(ar->aggdistinct) > 0) {
      having_replace_ctx *c = (having_replace_ctx *) ctx;
      Var *v = makeNode(Var);
      v->varno     = c->next_rtindex++;
      v->varattno  = 1;            /* agg result is col 1 of each outer */
      v->vartype   = ar->aggtype;
      v->vartypmod = -1;
      v->varcollid = ar->aggcollid;
      v->location  = -1;
      return (Node *) v;
    }
    return node;  /* non-DISTINCT aggregate: leave for the normal HAVING path */
  }
  return expression_tree_mutator(node, replace_having_distinct_mutator, ctx);
}

/**
 * @brief Rewrite every @c AGG(DISTINCT key) in @p q using independent subqueries.
 *
 * For a single DISTINCT aggregate, produces a subquery:
 * @code
 *   SELECT AGG(key), gb...  FROM (SELECT key, gb... FROM t GROUP BY key, gb...) GROUP BY gb...
 * @endcode
 * For multiple DISTINCT aggregates with different keys, produces an JOIN
 * of one such subquery per aggregate, joined on the GROUP BY columns.
 * Non-DISTINCT aggregates are left untouched.
 *
 * @c AGG(DISTINCT) aggregates appearing in the @c HAVING clause are handled
 * the same way (one deduped outer per aggregate) and the @c HAVING Aggref is
 * replaced by a @c Var to its outer's count column, so the comparison's
 * provenance is built over the per-distinct-value rows rather than the raw
 * tuples.
 *
 * @param q            Query to inspect and possibly rewrite.
 * @param constants    Extension OID cache.
 * @return   Rewritten query, or @c NULL if no @c AGG(DISTINCT) was found.
 */
static Query *rewrite_agg_distinct(Query *q, const constants_t *constants) {
  List *distinct_agg_tes = NIL;
  List *groupby_tes = NIL;
  ListCell *lc;
  having_distinct_ctx hctx = { NIL };

#if PG_VERSION_NUM >= 180000
  /* In PostgreSQL 18, parseCheckAggregates() injects a virtual RTE_GROUP
   * entry at the END of the range table.  GROUP BY column Vars in the
   * SELECT list point to this entry (varno == group_rtindex) instead of
   * the underlying base-table RTE.
   *
   * Strip that entry now, before we do any index arithmetic (fll, rtr->rtindex,
   * agg_idx) or copy q->targetList into groupby_tes.  Once removed:
   *  - q->rtable contains only real RTEs, so appending outer-subquery RTEs
   *    lands at the correct indices.
   *  - groupby_tes will carry resolved (base-table) Var expressions, so
   *    the WHERE equalities and the inner-query target list are correct.
   * We also resolve the Var(group_rtindex) refs in q's own targetList and
   * WHERE clause so the final query doesn't reference the stripped entry. */
  strip_group_rte_pg18(q);
#endif

  /* Extract AGG(DISTINCT) and GROUP BY targets from the target list.
   * Regular AGG() aggregations and expressions containing provenance()
   * are left untouched. */
  foreach (lc, q->targetList) {
    TargetEntry *te = lfirst(lc);
    if (IsA(te->expr, Aggref)) {
      Aggref *ar = (Aggref *)te->expr;
      if (list_length(ar->aggdistinct) > 0)
        distinct_agg_tes = lappend(distinct_agg_tes, te);
    } else if (provenance_function_walker((Node *)te->expr,
                                          (void *)constants)) {
      /* Expression contains provenance() — skip it, it will be
       * handled later by the provenance rewriter */
    } else {
      /* Non-aggregate column — treat as GROUP BY key */
      TargetEntry *te_copy = copyObject(te);
      te_copy->resjunk = false;
      groupby_tes = lappend(groupby_tes, te_copy);
    }
  }

  /* Also collect AGG(DISTINCT) aggregates from the HAVING clause; they are
   * not TargetEntries, so they get their own list and a Var-replacement
   * mutator below. */
  if (q->havingQual != NULL)
    collect_having_distinct_walker(q->havingQual, &hctx);

  if (distinct_agg_tes == NIL && hctx.aggs == NIL)
    return NULL;

  {
    int n_having = list_length(hctx.aggs);
    int n_aggs = list_length(distinct_agg_tes) + n_having;
    int n_gb   = list_length(groupby_tes);
    List *outer_queries = NIL;

    /* -----------------------------------------------------------------------
     * For each DISTINCT aggregate, build:
     *   inner_i: SELECT key_i, gb... FROM original... GROUP BY key_i, gb...
     *   outer_i: SELECT AGG(key_i) ASS agg_i, gb... FROM inner_i GROUP BY gb...
     *
     * Then produce a final query:
     *   SELECT gb..., agg_0, ..., agg_{N-1}
     *   FROM original... JOIN outer_0 ON gb... = gb... [JOIN ...]
     *  keeping the same order for the output columns.
     *
     * Column order in the final target list follows q->targetList:
     *   - DISTINCT agg i  → Var(n+i, 1)   (agg col of outer_i)
     * ----------------------------------------------------------------------- */

    /* Build one inner + one outer query per DISTINCT aggregate */
    foreach (lc, distinct_agg_tes) {
      TargetEntry *agg_te = lfirst(lc);
      Aggref *ar = (Aggref *)agg_te->expr;
      if(list_length(ar->args) != 1)
        provsql_error("AGG(DISTINCT) with more than one argument is not supported");
      else {
        Expr *key_expr = (Expr *)((TargetEntry *)linitial(ar->args))->expr;
        Query *inner = build_inner_for_distinct_key(q, key_expr, groupby_tes);
        Query *outer = build_outer_for_distinct_key(agg_te, inner, n_gb, constants);
        outer_queries = lappend(outer_queries, outer);
      }
    }

    /* Build one inner + one outer query per HAVING-clause DISTINCT aggregate,
     * appended after the target-list ones so their RT indices are the last
     * n_having entries of the final from-list (matched by the mutator below). */
    foreach (lc, hctx.aggs) {
      Aggref *ar = lfirst(lc);
      if(list_length(ar->args) != 1)
        provsql_error("AGG(DISTINCT) with more than one argument is not supported");
      else {
        TargetEntry *syn = makeNode(TargetEntry);
        Expr *key_expr = (Expr *)((TargetEntry *)linitial(ar->args))->expr;
        Query *inner = build_inner_for_distinct_key(q, key_expr, groupby_tes);
        Query *outer;
        syn->expr  = (Expr *) copyObject(ar);
        syn->resno = 1;
        outer = build_outer_for_distinct_key(syn, inner, n_gb, constants);
        outer_queries = lappend(outer_queries, outer);
      }
    }

    {
      /* One subquery RTE per outer query */
      int i = 0;
      foreach (lc, outer_queries) {
        Query *oq = lfirst(lc);
        RangeTblEntry *rte = makeNode(RangeTblEntry);
        Alias *alias = makeNode(Alias), *eref = makeNode(Alias);
        ListCell *lc2;
        char buf[16];

        snprintf(buf, sizeof(buf), "d%d", i + 1);
        alias->aliasname = eref->aliasname = pstrdup(buf);
        eref->colnames = NIL;
        foreach (lc2, oq->targetList) {
          TargetEntry *te = lfirst(lc2);
          eref->colnames = lappend(eref->colnames,
                                   makeString(te->resname ? pstrdup(te->resname) : ""));
        }
        rte->alias    = alias;
        rte->eref     = eref;
        rte->rtekind  = RTE_SUBQUERY;
        rte->subquery = oq;
        rte->inFromCl = true;
#if PG_VERSION_NUM < 160000
        rte->requiredPerms = ACL_SELECT;
#endif
        q->rtable = lappend(q->rtable, rte);
        i++;
      }

      /* Build FROM list and WHERE conditions for the implicit join.
       * Use a simple FROM original..., outer_i, ... WHERE original.gb_j = outer_i.gb_j */
      {
        FromExpr *jt = q->jointree;
        List *from_list = jt->fromlist;
        unsigned fll = list_length(from_list);
        List *where_args = NIL;

        for (i = fll+1; i <= fll+n_aggs; i++) {
          RangeTblRef *rtr = makeNode(RangeTblRef);
          ListCell *lc2;
          unsigned j=0;

          rtr->rtindex = i;
          from_list = lappend(from_list, rtr);

          /* outer_0.gb_j = outer_i.gb_j for each GROUP BY column j */
          foreach(lc2, groupby_tes) {
            TargetEntry *gb_te = lfirst(lc2);
            int gb_attno = ++j + 1; /* col 1 = agg, cols 2+ = GB */
            Oid ytype  = exprType((Node *)gb_te->expr);
            Oid opno   = find_equality_operator(ytype, ytype);
            Operator opInfo = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
            Form_pg_operator opform;
            OpExpr *oe = makeNode(OpExpr);
            Expr *le = copyObject(gb_te->expr);
            Var *rv = makeNode(Var);
            Oid collation=exprCollation((Node*) le);

            if (!HeapTupleIsValid(opInfo))
              provsql_error("could not find equality operator for type %u",
                            ytype);
            opform = (Form_pg_operator)GETSTRUCT(opInfo);

            oe->opno         = opno;
            oe->opfuncid     = opform->oprcode;
            oe->opresulttype = opform->oprresult;
            oe->opcollid     = InvalidOid;
            oe->inputcollid  = collation;
            oe->location     = -1;
            ReleaseSysCache(opInfo);

            rv->varno = i; rv->varattno = gb_attno;
            rv->vartype = ytype; rv->varcollid = collation;
            rv->vartypmod = -1; rv->location = -1;

            oe->args   = list_make2(le, rv);
            where_args = lappend(where_args, oe);
          }
        }

        if (list_length(where_args) == 0) {
          jt->quals = NULL;
        } else if (list_length(where_args) == 1) {
          jt->quals = linitial(where_args);
        } else {
          BoolExpr *be = makeNode(BoolExpr);
          be->boolop   = AND_EXPR;
          be->args     = where_args;
          be->location = -1;
          jt->quals    = (Node *)be;
        }
      }

      /* Build final target list in original column order.
       * DISTINCT agg i → Var(i+1, 1);  GROUP BY col j → Var(1, 2+j). */
      {
        int agg_idx   = list_length(q->jointree->fromlist) - n_aggs + 1;
        ListCell *lc2;

        foreach (lc2, q->targetList) {
          TargetEntry *te = lfirst(lc2);

          if (IsA(te->expr, Aggref) &&
              ((Aggref *)te->expr)->aggdistinct != NIL) {
            Var *v = makeNode(Var);
            v->varno = agg_idx++; /* outer_{agg_idx} RTE */
            v->varattno = 1;      /* agg result is col 1 of each outer */
            v->vartypmod = -1;
            v->location  = -1;
            te->expr = (Expr*)v;
          }
        }
      }

      /* Replace HAVING-clause DISTINCT aggregates with Vars to their outer
       * subqueries -- the last n_having entries of the from-list, in the same
       * order collect_having_distinct_walker visited them. */
      if (n_having > 0) {
        having_replace_ctx hrc;
        hrc.next_rtindex =
          list_length(q->jointree->fromlist) - n_having + 1;
        q->havingQual = replace_having_distinct_mutator(q->havingQual, &hrc);
      }

      return q;
    }
  }
}


/* -------------------------------------------------------------------------
 * Aggregation replacement mutator
 * ------------------------------------------------------------------------- */

/** @brief Context for the @c aggregation_mutator tree walker. */
typedef struct aggregation_mutator_context {
  List *prov_atts;              ///< List of provenance Var nodes
  semiring_operation op;        ///< Semiring operation for combining tokens
  const constants_t *constants; ///< Extension OID cache
} aggregation_mutator_context;

/**
 * @brief Tree-mutator that replaces Aggrefs with provenance-aware aggregates.
 * @param node  Current expression tree node.
 * @param ctx   Pointer to an @c aggregation_mutator_context (prov_atts,
 *              op, and constants).
 * @return      Possibly modified node.
 */
static Node *aggregation_mutator(Node *node, void *ctx) {
  aggregation_mutator_context *context = (aggregation_mutator_context *)ctx;
  if (node == NULL)
    return NULL;

  if (IsA(node, Aggref)) {
    Aggref *ar_v = (Aggref *)node;
    return (Node *)make_aggregation_expression(context->constants, ar_v,
                                               context->prov_atts, context->op);
  }

  return expression_tree_mutator(node, aggregation_mutator, ctx);
}

/**
 * @brief Wrap a @c provenance_aggregate FuncExpr with a cast to the
 *        original aggregate return type.
 *
 * @param prov_agg  The provenance_aggregate FuncExpr to wrap.
 * @param constants Extension OID cache.
 * @return          Cast FuncExpr wrapping @p prov_agg.
 */
static Node *wrap_agg_token_with_cast(FuncExpr *prov_agg,
                                      const constants_t *constants) {
  Const *typ_const = (Const *)lsecond(prov_agg->args);
  Oid target_type = DatumGetObjectId(typ_const->constvalue);
  CoercionPathType pathtype;
  Oid castfuncid;

  pathtype = find_coercion_pathway(target_type,
                                   constants->OID_TYPE_AGG_TOKEN,
                                   COERCION_EXPLICIT, &castfuncid);
  if (pathtype == COERCION_PATH_FUNC && OidIsValid(castfuncid)) {
    FuncExpr *cast = makeNode(FuncExpr);
    cast->funcid = castfuncid;
    cast->funcresulttype = target_type;
    cast->funcretset = false;
    cast->funcvariadic = false;
    cast->funcformat = COERCE_IMPLICIT_CAST;
    cast->args = list_make1(prov_agg);
    cast->location = -1;
    return (Node *)cast;
  }

  provsql_error("no cast from agg_token to %s for arithmetic on aggregate",
                format_type_be(target_type));
  return (Node *)prov_agg; /* unreachable */
}

/**
 * @brief Cast @c provenance_aggregate arguments of an operator or
 *        function when the formal parameter type requires it.
 *
 * For each argument in @p args that is a @c provenance_aggregate call,
 * check the corresponding formal parameter type of the parent function
 * @p parent_funcid.  If the formal type is polymorphic or @c agg_token
 * itself, the argument is left alone.  Otherwise a cast to the original
 * aggregate return type is inserted.
 *
 * @param args           Argument list to inspect (modified in place).
 * @param parent_funcid  OID of the parent function / operator implementor.
 * @param constants      Extension OID cache.
 */
static void maybe_cast_agg_token_args(List *args, Oid parent_funcid,
                                      const constants_t *constants) {
  HeapTuple tp;
  Form_pg_proc procForm;
  ListCell *lc;
  int i;

  tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(parent_funcid));
  if (!HeapTupleIsValid(tp))
    return;
  procForm = (Form_pg_proc) GETSTRUCT(tp);

  i = 0;
  foreach(lc, args) {
    Node *arg = lfirst(lc);

    if (i < procForm->pronargs && IsA(arg, FuncExpr) &&
        ((FuncExpr *)arg)->funcid == constants->OID_FUNCTION_PROVENANCE_AGGREGATE) {
      Oid formal_type = procForm->proargtypes.values[i];

      if (formal_type != constants->OID_TYPE_AGG_TOKEN &&
          !IsPolymorphicType(formal_type)) {
        lfirst(lc) = wrap_agg_token_with_cast((FuncExpr *)arg, constants);
      }
    }
    i++;
  }

  ReleaseSysCache(tp);
}

/**
 * @brief Tree-mutator that casts @c provenance_aggregate results back
 *        to the original aggregate return type where needed.
 *
 * After the aggregation mutator replaces Aggrefs with
 * @c provenance_aggregate calls (returning @c agg_token), this
 * post-processing step inserts casts where the surrounding expression
 * expects a different type (e.g. @c SUM(id)+1).  Arguments to
 * functions that accept @c agg_token or polymorphic types are left
 * alone.
 *
 * @param node Current expression tree node.
 * @param ctx  Pointer to the @c constants_t OID cache.
 * @return     Possibly modified node.
 */
static Node *cast_agg_token_mutator(Node *node, void *ctx) {
  const constants_t *constants = (const constants_t *)ctx;
  Node *result;

  if (node == NULL)
    return NULL;

  /* Recurse first, then fix up arguments at this level. */
  result = expression_tree_mutator(node, cast_agg_token_mutator, ctx);

  if (IsA(result, OpExpr)) {
    OpExpr *op = (OpExpr *)result;
    set_opfuncid(op);
    maybe_cast_agg_token_args(op->args, op->opfuncid, constants);
  } else if (IsA(result, FuncExpr)) {
    FuncExpr *fe = (FuncExpr *)result;
    if (fe->funcid != constants->OID_FUNCTION_PROVENANCE_AGGREGATE)
      maybe_cast_agg_token_args(fe->args, fe->funcid, constants);
  }

  return result;
}

/**
 * @brief Replace every @c Aggref in @p q with a provenance-aware aggregate.
 *
 * Walks the query tree and substitutes each @c Aggref node with the result
 * of @c make_aggregation_expression, which wraps the original aggregate in
 * the semimodule machinery (@c provenance_semimod + @c array_agg +
 * @c provenance_aggregate).
 *
 * @param constants  Extension OID cache.
 * @param q          Query to mutate in place.
 * @param prov_atts  List of provenance @c Var nodes.
 * @param op         Semiring operation for combining tokens across rows.
 */
static void
replace_aggregations_by_provenance_aggregate(const constants_t *constants,
                                             Query *q, List *prov_atts,
                                             semiring_operation op) {

  aggregation_mutator_context context = {prov_atts, op, constants};
  ListCell *lc;

  query_tree_mutator(q, aggregation_mutator, &context,
                     QTW_DONT_COPY_QUERY | QTW_IGNORE_RT_SUBQUERIES);

  /* Post-processing: for target-list entries where a provenance_aggregate
   * result is nested inside an outer expression (e.g. SUM(id)+1),
   * insert a cast from agg_token back to the original aggregate return
   * type.  Standalone provenance_aggregate entries are left as agg_token
   * so they display as "value (*)". */
  foreach(lc, q->targetList) {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    if (te->expr == NULL)
      continue;
    /* Skip standalone provenance_aggregate calls */
    if (IsA(te->expr, FuncExpr) &&
        ((FuncExpr *)te->expr)->funcid == constants->OID_FUNCTION_PROVENANCE_AGGREGATE)
      continue;
    te->expr = (Expr *)cast_agg_token_mutator((Node *)te->expr,
                                              (void *)constants);
  }
}

/**
 * @brief Append the provenance expression to @p q's target list.
 *
 * Inserts a new @c TargetEntry named @c provsql immediately before any
 * @c resjunk entries (which must remain last) and adjusts the @c resno
 * of subsequent entries accordingly.
 *
 * @param q           Query to modify in place.
 * @param provenance  Expression to add (becomes the @c provsql output column).
 */
static void add_to_select(Query *q, Expr *provenance) {
  TargetEntry *newte = makeNode(TargetEntry);
  bool inserted = false;
  unsigned resno = 0;

  newte->expr = provenance;
  newte->resname = (char *)PROVSQL_COLUMN_NAME;

  if (IsA(provenance, Var)) {
    RangeTblEntry *rte = list_nth(q->rtable, ((Var *)provenance)->varno - 1);
    newte->resorigtbl = rte->relid;
    newte->resorigcol = ((Var *)provenance)->varattno;
  }

  /* Make sure to insert before all resjunk Target Entry */
  for (ListCell *cell = list_head(q->targetList); cell != NULL;) {
    TargetEntry *te = (TargetEntry *)lfirst(cell);

    if (!inserted)
      ++resno;

    if (te->resjunk) {
      if (!inserted) {
        newte->resno = resno;
        q->targetList = list_insert_nth(q->targetList, resno - 1, newte);
        cell = list_nth_cell(q->targetList, resno);
        te = (TargetEntry *)lfirst(cell);
        inserted = true;
      }

      ++te->resno;
    }

    cell = my_lnext(q->targetList, cell);
  }

  if (!inserted) {
    newte->resno = resno + 1;
    q->targetList = lappend(q->targetList, newte);
  }
}

/* -------------------------------------------------------------------------
 * Provenance function replacement
 * ------------------------------------------------------------------------- */

/** @brief Context for the @c provenance_mutator tree walker. */
typedef struct provenance_mutator_context {
  Expr *provsql;                ///< Provenance expression to substitute for provenance() calls
  const constants_t *constants; ///< Extension OID cache
  bool provsql_has_aggref;      ///< @c true when @c provsql contains an @c Aggref (set once by @c replace_provenance_function_by_expression).  When @c true, a @c provenance() substitution that lands inside another @c Aggref's argument tree would produce a nested same-level aggregate -- @c parse_agg.c forbids that shape, the planner's @c preprocess_aggrefs_walker does not recurse through @c Aggref boundaries, and the inner @c Aggref's @c aggno stays at the @c -1 sentinel and crashes @c ExecInterpExpr on @c ecxt_aggvalues[-1].
  bool inside_aggref;           ///< @c true while descending the argument tree of an @c Aggref node.
} provenance_mutator_context;

/**
 * @brief @c expression_tree_walker predicate: returns @c true on the first
 *        @c Aggref it encounters.
 *
 * Used to decide whether the provenance expression about to be substituted
 * would inject a nested aggregate when a @c provenance() call lives inside
 * another @c Aggref's argument tree.
 */
static bool
expr_contains_aggref_walker(Node *node, void *context) {
  if (node == NULL)
    return false;
  if (IsA(node, Aggref))
    return true;
  return expression_tree_walker(node, expr_contains_aggref_walker, context);
}

/**
 * @brief Tree-mutator that replaces provenance() calls with the actual provenance expression.
 * @param node  Current expression tree node.
 * @param ctx   Pointer to a @c provenance_mutator_context (provenance
 *              expression and constants).
 * @return      Possibly modified node.
 */
static Node *provenance_mutator(Node *node, void *ctx) {
  provenance_mutator_context *context = (provenance_mutator_context *)ctx;
  if (node == NULL)
    return NULL;

  if (IsA(node, Aggref)) {
    /* Descend into the Aggref's arguments with @c inside_aggref set so we
     * can refuse substitutions that would create a nested same-level
     * aggregate.  Save and restore the flag so sibling sub-expressions
     * outside this Aggref see the original value. */
    bool saved = context->inside_aggref;
    Node *result;
    context->inside_aggref = true;
    result = expression_tree_mutator(node, provenance_mutator, ctx);
    context->inside_aggref = saved;
    return result;
  }

  if (IsA(node, FuncExpr)) {
    FuncExpr *f = (FuncExpr *)node;

    if (f->funcid == context->constants->OID_FUNCTION_PROVENANCE) {
      if (context->inside_aggref && context->provsql_has_aggref) {
        provsql_error(
          "applying an SQL aggregate on top of a ProvSQL-introduced "
          "aggregation is not supported: the inner provenance() would "
          "be substituted with an expression containing an aggregate, "
          "producing a nested same-level aggregate that PostgreSQL "
          "rejects.  Evaluate the per-row provenance in a subquery "
          "and aggregate the resulting scalar outside, or drop the "
          "surrounding aggregate.");
      }
      return (Node *)copyObject(context->provsql);
    }
  } else if (IsA(node, RangeTblEntry) || IsA(node, RangeTblFunction)) {
    // A provenance() expression in a From (not within a subquery) is
    // non-sensical
    return node;
  }

  return expression_tree_mutator(node, provenance_mutator, ctx);
}

/**
 * @brief Replace every explicit @c provenance() call in @p q with @p provsql.
 *
 * Users can write @c provenance() in the target list or WHERE to refer to the
 * provenance token of the current tuple.  This mutator substitutes those calls
 * with the actual computed provenance expression.
 *
 * @param constants  Extension OID cache.
 * @param q          Query to mutate in place.
 * @param provsql    Provenance expression to substitute.
 */
static void
replace_provenance_function_by_expression(const constants_t *constants,
                                          Query *q, Expr *provsql) {
  provenance_mutator_context context;

  context.provsql = provsql;
  context.constants = constants;
  context.provsql_has_aggref =
    expr_contains_aggref_walker((Node *) provsql, NULL);
  context.inside_aggref = false;

  query_tree_mutator(q, provenance_mutator, &context,
                     QTW_DONT_COPY_QUERY | QTW_IGNORE_RT_SUBQUERIES);
}

/**
 * @brief Convert a SELECT DISTINCT into an equivalent GROUP BY.
 *
 * ProvSQL cannot handle DISTINCT directly (it would collapse provenance
 * tokens that should remain separate).  This function moves every entry
 * from @p q->distinctClause into @p q->groupClause (skipping any that are
 * already there) and clears @p q->distinctClause.
 *
 * @param q  Query to modify in place.
 */
static void transform_distinct_into_group_by(Query *q) {
  // First check which are already in the group by clause
  // Should be either none or all as "SELECT DISTINCT a, b ... GROUP BY a"
  // is invalid
  Bitmapset *already_in_group_by = NULL;
  ListCell *lc;
  foreach (lc, q->groupClause) {
    SortGroupClause *sgc = (SortGroupClause *)lfirst(lc);
    already_in_group_by =
      bms_add_member(already_in_group_by, sgc->tleSortGroupRef);
  }

  foreach (lc, q->distinctClause) {
    SortGroupClause *sgc = (SortGroupClause *)lfirst(lc);
    if (!bms_is_member(sgc->tleSortGroupRef, already_in_group_by)) {
      q->groupClause = lappend(q->groupClause, sgc);
    }
  }

  q->distinctClause = NULL;
}

/**
 * @brief Remove sort/group references that belonged to removed provenance columns.
 *
 * After @c remove_provenance_attributes_select strips provenance entries from
 * the target list, any GROUP BY, ORDER BY, or DISTINCT clause that referenced
 * them by @c tleSortGroupRef must be cleaned up.
 *
 * @param q                      Query to modify in place.
 * @param removed_sortgrouprefs  Bitmapset of @c ressortgroupref values to remove.
 */
static void
remove_provenance_attribute_groupref(Query *q,
                                     const Bitmapset *removed_sortgrouprefs) {
  List **lists[3] = {&q->groupClause, &q->distinctClause, &q->sortClause};
  int i = 0;

  for (i = 0; i < 3; ++i) {
    ListCell *cell, *prev;

    for (cell = list_head(*lists[i]), prev = NULL; cell != NULL;) {
      SortGroupClause *sgc = (SortGroupClause *)lfirst(cell);
      if (bms_is_member(sgc->tleSortGroupRef, removed_sortgrouprefs)) {
        *lists[i] = my_list_delete_cell(*lists[i], cell, prev);

        if (prev) {
          cell = my_lnext(*lists[i], prev);
        } else {
          cell = list_head(*lists[i]);
        }
      } else {
        prev = cell;
        cell = my_lnext(*lists[i], cell);
      }
    }
  }
}

/**
 * @brief Strip the provenance column's type info from a set-operation node.
 *
 * When a provenance column is removed from a UNION/EXCEPT query's target list,
 * the matching entries in the @c SetOperationStmt's @c colTypes, @c colTypmods,
 * and @c colCollations lists must also be removed.
 *
 * @param q        Query containing @c setOperations.
 * @param removed  Boolean array (from @c remove_provenance_attributes_select)
 *                 indicating which columns were removed.
 */
static void remove_provenance_attribute_setoperations(Query *q, bool *removed) {
  SetOperationStmt *so = (SetOperationStmt *)q->setOperations;
  List **lists[3] = {&so->colTypes, &so->colTypmods, &so->colCollations};
  int i = 0;

  for (i = 0; i < 3; ++i) {
    ListCell *cell, *prev;
    int j;

    for (cell = list_head(*lists[i]), prev = NULL, j = 0; cell != NULL; ++j) {
      if (removed[j]) {
        *lists[i] = my_list_delete_cell(*lists[i], cell, prev);

        if (prev) {
          cell = my_lnext(*lists[i], prev);
        } else {
          cell = list_head(*lists[i]);
        }
      } else {
        prev = cell;
        cell = my_lnext(*lists[i], cell);
      }
    }
  }
}

/**
 * @brief Wrap a non-ALL set operation in an outer GROUP BY query.
 *
 * UNION / EXCEPT (without ALL) would deduplicate tuples before ProvSQL can
 * attach provenance tokens.  To avoid this, the set operation is converted to
 * UNION ALL / EXCEPT ALL and a new outer query is built that groups the results
 * by all non-provenance columns, collecting tokens into an array for the
 * @c provenance_plus evaluation.
 *
 * After this rewrite the recursive call to @c process_query handles the
 * now-ALL inner set operation normally.
 *
 * @param q  Query whose @c setOperations is non-ALL (modified to ALL in place).
 * @return   New outer query that wraps @p q as a subquery RTE.
 */
static Query *rewrite_non_all_into_external_group_by(Query *q) {
  Query *new_query = makeNode(Query);
  RangeTblEntry *rte = makeNode(RangeTblEntry);
  FromExpr *jointree = makeNode(FromExpr);
  RangeTblRef *rtr = makeNode(RangeTblRef);

  SetOperationStmt *stmt = (SetOperationStmt *)q->setOperations;

  ListCell *lc;
  int sortgroupref = 0;

  stmt->all = true;
  // we might leave sub nodes of the SetOperationsStmt tree with all = false
  // but only for recursive trees of operators and only union can be recursive
  // https://doxygen.postgresql.org/prepunion_8c_source.html#l00479
  // we will set therefore set them later in process_set_operation_union

  rte->rtekind = RTE_SUBQUERY;
  rte->subquery = q;
  rte->eref = copyObject(((RangeTblEntry *)linitial(q->rtable))->eref);
  rte->inFromCl = true;
#if PG_VERSION_NUM < 160000
  // For PG_VERSION_NUM >= 160000, rte->perminfoindex==0 so no need to
  // care about permissions
  rte->requiredPerms = ACL_SELECT;
#endif

  rtr->rtindex = 1;
  jointree->fromlist = list_make1(rtr);

  new_query->commandType = CMD_SELECT;
  new_query->canSetTag = true;
  new_query->rtable = list_make1(rte);
  new_query->jointree = jointree;
  new_query->targetList = copyObject(q->targetList);

  if (new_query->targetList) {
    foreach (lc, new_query->targetList) {
      TargetEntry *te = (TargetEntry *)lfirst(lc);
      SortGroupClause *sgc = makeNode(SortGroupClause);

      sgc->tleSortGroupRef = te->ressortgroupref = ++sortgroupref;

      get_sort_group_operators(exprType((Node *)te->expr), false, true, false,
                               &sgc->sortop, &sgc->eqop, NULL, &sgc->hashable);

      new_query->groupClause = lappend(new_query->groupClause, sgc);
    }
  } else {
    GroupingSet *gs = makeNode(GroupingSet);
    gs->kind = GROUPING_SET_EMPTY;
    gs->content = 0;
    gs->location = -1;
    new_query->groupingSets = list_make1(gs);
  }

  return new_query;
}

/* -------------------------------------------------------------------------
 * Detection walkers
 * ------------------------------------------------------------------------- */

/**
 * @brief Tree walker that returns true if any @c provenance() call is found.
 *
 * Used to detect whether a query explicitly calls @c provenance(), which
 * triggers the substitution in @c replace_provenance_function_by_expression.
 * @param node  Current expression tree node.
 * @param data  Pointer to @c constants_t (cast from @c void*).
 * @return      @c true if a @c provenance() call is found anywhere in @p node.
 */
static bool provenance_function_walker(Node *node, void *data) {
  const constants_t *constants = (const constants_t *)data;
  if (node == NULL)
    return false;

  if (IsA(node, FuncExpr)) {
    FuncExpr *f = (FuncExpr *)node;

    if (f->funcid == constants->OID_FUNCTION_PROVENANCE)
      return true;
  }

  return expression_tree_walker(node, provenance_function_walker, data);
}

/**
 * @brief Check whether a @c provenance() call appears in the GROUP BY list.
 *
 * When the user writes @c GROUP BY provenance(), ProvSQL must not add its own
 * group-by wrapper (the query is already grouping on the token).
 *
 * @param constants  Extension OID cache.
 * @param q          Query to inspect.
 * @return  True if any GROUP BY key contains a @c provenance() call.
 */
static bool provenance_function_in_group_by(const constants_t *constants,
                                            Query *q) {
  ListCell *lc;

  /* Build the set of ressortgrouprefs that are actually in GROUP BY
   * (not ORDER BY or DISTINCT, which also set ressortgroupref). */
  Bitmapset *group_refs = NULL;
  foreach (lc, q->groupClause) {
    SortGroupClause *sgc = (SortGroupClause *)lfirst(lc);
    group_refs = bms_add_member(group_refs, sgc->tleSortGroupRef);
  }

  foreach (lc, q->targetList) {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    if (te->ressortgroupref > 0 &&
        bms_is_member(te->ressortgroupref, group_refs)) {
      if(expression_tree_walker((Node *)te, provenance_function_walker,
                                (void *)constants)) {
        return true;
      }

#if PG_VERSION_NUM >= 180000
      // Starting from PostgreSQL 18, the content of the GROUP BY is not
      // in the groupClause but in an associated RTE_GROUP RangeTblEntry
      if(IsA(te->expr, Var)) {
        Var *v = (Var *) te->expr;
        RangeTblEntry *r = (RangeTblEntry *)list_nth(q->rtable, v->varno - 1);
        if(r->rtekind == RTE_GROUP)
          if(expression_tree_walker((Node *) r->groupexprs, provenance_function_walker,
                                    (void *)constants)) {
            return true;
          }
      }
#endif
    }
  }

  return false;
}

/**
 * @brief Tree walker that detects any provenance-bearing relation or provenance() call.
 * @param node  Current expression tree node.
 * @param data  Pointer to @c constants_t (cast from @c void*).
 * @return      @c true if provenance rewriting is needed for this node.
 */
/**
 * @brief Recursive helper for @c has_provenance_walker that detects
 *        rv_cmp @c OpExpr and @c provenance() @c FuncExpr in
 *        expression subtrees.
 *
 * Stops at Query boundaries: @c SubLink subselects (used as
 * scalar/array subqueries in expressions) are not rewritten by the
 * outer planner_hook pass, so a tracked relation inside one must not
 * cause the OUTER query's gate to engage.  Only the @c testexpr of a
 * SubLink is followed (it lives in the outer's evaluation scope).
 */
static bool has_rv_or_provenance_call(Node *node, void *data) {
  const constants_t *constants = (const constants_t *)data;
  if (node == NULL)
    return false;

  if (IsA(node, OpExpr)) {
    OpExpr *op = (OpExpr *)node;
    if (rv_cmp_index(constants, op->opfuncid) >= 0)
      return true;
  }

  if (IsA(node, FuncExpr)) {
    FuncExpr *f = (FuncExpr *)node;
    if (f->funcid == constants->OID_FUNCTION_PROVENANCE)
      return true;
  }

  if (IsA(node, SubLink)) {
    SubLink *sl = (SubLink *)node;
    return has_rv_or_provenance_call((Node *)sl->testexpr, data);
  }

  /* Query nodes are opaque here; expression_tree_walker returns false
   * on them.  Explicit short-circuit just makes the intent obvious. */
  if (IsA(node, Query))
    return false;

  return expression_tree_walker(node, has_rv_or_provenance_call, data);
}

/**
 * @brief Walker (this query level only): true if an @c EXPR_SUBLINK whose body
 *        is a decorrelatable value subquery over a provenance-tracked base
 *        relation appears in an expression.
 *
 * Lets the planner gate engage for a scalar subquery over a tracked relation
 * even when the OUTER query has no tracked relation -- decorrelate_scalar_
 * sublinks then handles it (wrapping the untracked outer with a certain
 * gate_one() provenance and warning that its tuple provenance is lost).  The
 * shape conditions mirror decorrelate's subselect validation, so engagement
 * implies the decorrelation succeeds (no engage-then-error regression); a
 * non-decorrelatable scalar subquery still leaves the gate untouched and runs
 * as plain SQL.  Does not descend into nested Query / SubLink subselects.
 */
static bool decorr_value_sublink_walker(Node *node, void *data) {
  const constants_t *constants = (const constants_t *)data;
  if (node == NULL)
    return false;
  if (IsA(node, SubLink)) {
    SubLink *sl = (SubLink *)node;
    if (sl->subLinkType == EXPR_SUBLINK && sl->subselect &&
        IsA(sl->subselect, Query)) {
      Query *sub = (Query *)sl->subselect;
      if (!sub->hasAggs && !sub->groupClause && !sub->groupingSets &&
          !sub->distinctClause && !sub->setOperations && !sub->hasWindowFuncs &&
          !sub->hasSubLinks && !sub->limitCount && !sub->limitOffset &&
          !sub->cteList && list_length(sub->rtable) == 1 &&
          list_length(sub->targetList) == 1 && sub->jointree &&
          list_length(sub->jointree->fromlist) == 1 &&
          IsA(linitial(sub->jointree->fromlist), RangeTblRef)) {
        RangeTblEntry *qr = (RangeTblEntry *)linitial(sub->rtable);
        if (qr->rtekind == RTE_RELATION) {
          ListCell *lc;
          AttrNumber a = 0;
          foreach (lc, qr->eref->colnames) {
            ++a;
            if (!strcmp(strVal(lfirst(lc)), PROVSQL_COLUMN_NAME) &&
                get_atttype(qr->relid, a) == constants->OID_TYPE_UUID)
              return true;
          }
        }
      }
    }
    return false; /* do not descend into the subselect */
  }
  if (IsA(node, Query))
    return false; /* nested queries are handled by has_provenance_walker */
  return expression_tree_walker(node, decorr_value_sublink_walker, data);
}

static bool has_provenance_walker(Node *node, void *data) {
  const constants_t *constants = (const constants_t *)data;
  if (node == NULL)
    return false;

  if (IsA(node, Query)) {
    Query *q = (Query *)node;
    ListCell *rc;

    /* Walk into CTE subqueries explicitly: they will be inlined as
     * subqueries by the rewriter, so a tracked-table inside one (or
     * an rv_cmp / provenance() call) matters for this query. */
    foreach (rc, q->cteList) {
      CommonTableExpr *cte = (CommonTableExpr *)lfirst(rc);
      if (has_provenance_walker((Node *)cte->ctequery, data))
        return true;
    }

    /* Walk this query's own expressions for rv_cmp OpExpr and
     * provenance() FuncExpr.  Use the SubLink-aware walker so we
     * don't descend into expression-context subqueries (they get
     * planned standalone; an rv_cmp inside one matters only to
     * that planning pass).
     *
     * This intentionally replaces a single query_tree_walker call:
     * that helper recurses with the passed walker into BOTH rtable
     * RTEs (RTE_SUBQUERY) and SubLink subselects, which would erase
     * the SubLink/RTE_SUBQUERY distinction we need. */
    if (has_rv_or_provenance_call((Node *)q->targetList, data))
      return true;
    if (has_rv_or_provenance_call((Node *)q->jointree, data))
      return true;
    if (has_rv_or_provenance_call((Node *)q->havingQual, data))
      return true;
    if (has_rv_or_provenance_call((Node *)q->returningList, data))
      return true;

    /* A decorrelatable value scalar subquery over a tracked relation engages
     * the gate even with an untracked outer (handled with a warning). */
    if (decorr_value_sublink_walker((Node *)q->targetList, data))
      return true;
    if (q->jointree &&
        decorr_value_sublink_walker((Node *)q->jointree->quals, data))
      return true;

    foreach (rc, q->rtable) {
      RangeTblEntry *r = (RangeTblEntry *)lfirst(rc);
      if (r->rtekind == RTE_RELATION) {
        ListCell *lc;
        AttrNumber attid = 1;

        foreach (lc, r->eref->colnames) {
          const char *v = strVal(lfirst(lc));

          if (!strcmp(v, PROVSQL_COLUMN_NAME) &&
              get_atttype(r->relid, attid) == constants->OID_TYPE_UUID) {
            return true;
          }

          ++attid;
        }
      } else if (r->rtekind == RTE_FUNCTION) {
        ListCell *lc;
        AttrNumber attid = 1;

        foreach (lc, r->functions) {
          RangeTblFunction *func = (RangeTblFunction *)lfirst(lc);

          if (func->funccolcount == 1) {
            FuncExpr *expr = (FuncExpr *)func->funcexpr;
            if (expr->funcresulttype == constants->OID_TYPE_UUID &&
                !strcmp(get_rte_attribute_name(r, attid),
                        PROVSQL_COLUMN_NAME)) {
              return true;
            }
          }

          attid += func->funccolcount;
        }
      } else if (r->rtekind == RTE_SUBQUERY && r->subquery != NULL) {
        /* A FROM-source subquery contributes its provenance to ours;
         * process_query recurses on it explicitly, so we must detect
         * tracked relations / rv_cmp / provenance() inside it. */
        if (has_provenance_walker((Node *)r->subquery, data))
          return true;
      }
    }
  }

  /* For non-Query nodes, use the expression-only walker.  It detects
   * rv_cmp OpExpr and provenance() FuncExpr inside arbitrary
   * sub-expressions (BoolExpr around an rv comparison, RV cmp under
   * IS-DISTINCT-FROM, ...) but stops at Query boundaries so a sibling
   * subquery's tracked rtable doesn't make THIS query's gate engage
   * (subqueries have their own planner_hook pass). */
  return has_rv_or_provenance_call(node, data);
}

/**
 * @brief Return true if @p q involves any provenance-bearing relation or
 *        contains an explicit @c provenance() call.
 *
 * This is the gate condition checked by @c provsql_planner before doing any
 * rewriting: if neither condition holds the query is passed through unchanged.
 *
 * @param constants  Extension OID cache.
 * @param q          Query to inspect.
 * @return  True if provenance rewriting is needed.
 */
static bool has_provenance(const constants_t *constants, Query *q) {
  return has_provenance_walker((Node *)q, (void *)constants);
}

/**
 * @brief Walker: true if @p node (descending through nested queries) contains
 *        an explicit @c provenance() call.
 */
static bool calls_provenance_walker(Node *node, void *data) {
  if (node == NULL)
    return false;
  if (IsA(node, FuncExpr) &&
      ((FuncExpr *)node)->funcid ==
          ((const constants_t *)data)->OID_FUNCTION_PROVENANCE)
    return true;
  if (IsA(node, Query))
    return query_tree_walker((Query *)node, calls_provenance_walker, data, 0);
  return expression_tree_walker(node, calls_provenance_walker, data);
}

/**
 * @brief Walker: true if a @c SubLink subselect calls @c provenance().
 *
 * A @c SubLink subselect (scalar / @c IN / @c EXISTS) is planned standalone, so
 * it never goes through this hook -- a @c provenance() call inside one is never
 * rewritten and falls through to its runtime stub (NULL or a misleading error).
 * ProvSQL does not propagate provenance through a @c SubLink, so we detect the
 * @c provenance() use up front and raise a clear error instead.
 *
 * Only the explicit @c provenance() call is flagged, not a mere read of a
 * tracked relation's columns: @c (SELECT @c array_agg(provsql) @c FROM @c t) and
 * other plain column reads inside a @c SubLink are legitimate and must keep
 * working.  Tracked relations reached through the @c FROM clause
 * (@c RTE_SUBQUERY) are fully supported and never reach this walker's @c SubLink
 * arm.
 */
static bool provenance_in_sublink_walker(Node *node, void *data) {
  if (node == NULL)
    return false;
  if (IsA(node, Query))
    return query_tree_walker((Query *)node, provenance_in_sublink_walker, data, 0);
  if (IsA(node, SubLink)) {
    SubLink *sl = (SubLink *)node;
    if (sl->subselect && IsA(sl->subselect, Query) &&
        calls_provenance_walker(sl->subselect, data))
      return true;
  }
  return expression_tree_walker(node, provenance_in_sublink_walker, data);
}

/**
 * @brief Remove the auto-added @c provsql output column from a rewritten query.
 *
 * The inverse of @c add_to_select: drops the @c TargetEntry named
 * @c PROVSQL_COLUMN_NAME and decrements the @c resno of every later entry, so
 * the column numbering stays contiguous.  Used when a query was rewritten for
 * its own provenance semantics (HAVING lifting, @c provenance() resolution) but
 * the caller cannot store the provenance -- e.g. an @c INSERT @c ... @c SELECT
 * whose target table has no provsql column.
 */
static void remove_provsql_from_select(Query *q) {
  ListCell *lc;
  int removed_resno = -1;

  foreach (lc, q->targetList) {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    if (te->resname && !strcmp(te->resname, PROVSQL_COLUMN_NAME)) {
      removed_resno = te->resno;
      q->targetList = list_delete_cell(q->targetList, lc);
      break;
    }
  }

  if (removed_resno < 0)
    return;

  foreach (lc, q->targetList) {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    if (te->resno > removed_resno)
      --te->resno;
  }
}

/**
 * @brief Tree walker that detects any Var of type agg_token.
 * @param node  Current expression tree node.
 * @param data  Pointer to a @c constants_t (extension OID cache).
 * @return      @c true if an agg_token Var is found in @p node.
 */
static bool aggtoken_walker(Node *node, void *data) {
  const constants_t *constants = (const constants_t *) data;
  if (node == NULL)
    return false;

  if (IsA(node, Var)) {
    Var *v = (Var *) node;
    if(v->vartype == constants->OID_TYPE_AGG_TOKEN)
      return true;
  }

  return expression_tree_walker(node, aggtoken_walker, data);
}

/**
 * @brief Return true if @p node contains a @c Var of type @c agg_token.
 *
 * Used to detect whether a WHERE clause references an aggregate result
 * (which must be moved to HAVING).
 *
 * @param node       Expression tree to inspect.
 * @param constants  Extension OID cache.
 * @return  True if an @c agg_token @c Var is found anywhere in @p node.
 */
static bool has_aggtoken(Node *node, const constants_t *constants) {
  return expression_tree_walker(node, aggtoken_walker, (void*) constants);
}

/**
 * @brief Walker for @c needs_having_lift: detect any operand shape that
 *        the HAVING-lift rewriter (@c having_OpExpr_to_provenance_cmp)
 *        needs to handle specially.
 *
 * Returns @c true on:
 *   - a @c Var of type @c agg_token; or
 *   - a @c FuncExpr whose @c funcid is @c provenance_aggregate (the
 *     wrapper the planner-hook puts around aggregates over tracked
 *     non-RV columns -- yields @c agg_token).
 *
 * Anything else (deterministic scalars, plain @c Const, @c FuncExpr
 * over @c random_variable like @c expected / @c variance / @c moment,
 * comparisons of those) is left for PostgreSQL to evaluate natively;
 * the HAVING-lift never needs to touch it.
 */
static bool having_lift_walker(Node *node, void *data) {
  const constants_t *constants = (const constants_t *) data;
  if (node == NULL)
    return false;

  if (IsA(node, Var)) {
    Var *v = (Var *) node;
    if (v->vartype == constants->OID_TYPE_AGG_TOKEN)
      return true;
  }

  if (IsA(node, FuncExpr)) {
    FuncExpr *fe = (FuncExpr *) node;
    if (fe->funcid == constants->OID_FUNCTION_PROVENANCE_AGGREGATE)
      return true;
  }

  return expression_tree_walker(node, having_lift_walker, data);
}

/**
 * @brief Return true if @p havingQual contains anything the HAVING-lift
 *        path needs to handle (an @c agg_token Var or a
 *        @c provenance_aggregate wrapper).  A qual that returns @c false
 *        is left in place for PostgreSQL to evaluate, while the
 *        per-group provenance still gets a @c gate_delta wrapper.
 *
 * This is what lets a HAVING like @c expected(avg(rv)) > 20 work
 * directly: @c provsql.avg returns @c random_variable (not
 * @c agg_token), @c expected collapses to a scalar @c double, and the
 * surrounding comparison is a plain Boolean that PostgreSQL can filter
 * groups by without any provenance-side rewriting.
 */
static bool needs_having_lift(Node *havingQual, const constants_t *constants) {
  return expression_tree_walker(havingQual, having_lift_walker,
                                (void *) constants);
}

/**
 * @brief Rewrite an EXCEPT query into a LEFT JOIN with monus provenance.
 *
 * EXCEPT cannot be handled directly because it deduplicates.  This function
 * transforms:
 * @code
 *   SELECT … FROM A EXCEPT SELECT … FROM B
 * @endcode
 * into a LEFT JOIN of A and B on equality of all non-provenance columns,
 * clears @c setOperations, and leaves the monus token combination to
 * @c make_provenance_expression (which will see @c SR_MONUS).
 *
 * Only simple (non-chained) EXCEPT is supported; chained EXCEPT raises an
 * error.
 *
 * @param constants  Extension OID cache.
 * @param q          Query to rewrite in place.
 * @return  Always true (errors out on unsupported cases).
 */
static bool transform_except_into_join(const constants_t *constants, Query *q) {
  SetOperationStmt *setOps = (SetOperationStmt *)q->setOperations;
  RangeTblEntry *rte = makeNode(RangeTblEntry);
  FromExpr *fe = makeNode(FromExpr);
  JoinExpr *je = makeNode(JoinExpr);
  BoolExpr *expr = makeNode(BoolExpr);
  ListCell *lc;
  int attno = 1;

  if (!IsA(setOps->larg, RangeTblRef) || !IsA(setOps->rarg, RangeTblRef)) {
    provsql_error("Unsupported chain of EXCEPT operations");
  }

  expr->boolop = AND_EXPR;
  expr->location = -1;
  expr->args = NIL;

  foreach (lc, q->targetList) {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    Var *v;

    if (!IsA(te->expr, Var))
      provsql_error("EXCEPT query format not supported");

    v = (Var *)te->expr;

    if (v->vartype != constants->OID_TYPE_UUID) {
      OpExpr *oe = makeNode(OpExpr);
      Oid opno = find_equality_operator(v->vartype, v->vartype);
      Operator opInfo = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
      Form_pg_operator opform;
      Var *leftArg, *rightArg;

      if (!HeapTupleIsValid(opInfo))
        provsql_error("could not find operator with OID %u to compare variables of type %u",
                      opno, v->vartype);

      opform = (Form_pg_operator)GETSTRUCT(opInfo);
      leftArg = makeNode(Var);
      rightArg = makeNode(Var);

      oe->opno = opno;
      oe->opfuncid = opform->oprcode;
      oe->opresulttype = opform->oprresult;
      oe->opcollid = InvalidOid;
      oe->inputcollid = DEFAULT_COLLATION_OID;

      leftArg->varno = ((RangeTblRef *)setOps->larg)->rtindex;
      rightArg->varno = ((RangeTblRef *)setOps->rarg)->rtindex;
      leftArg->varattno = rightArg->varattno = attno;

#if PG_VERSION_NUM >= 130000
      leftArg->varnosyn = rightArg->varnosyn = 0;
      leftArg->varattnosyn = rightArg->varattnosyn = 0;
#else
      leftArg->varnoold = leftArg->varno;
      rightArg->varnoold = rightArg->varno;
      leftArg->varoattno = rightArg->varoattno = attno;
#endif

      leftArg->vartype = rightArg->vartype = v->vartype;
      leftArg->varcollid = rightArg->varcollid = InvalidOid;
      leftArg->vartypmod = rightArg->vartypmod = -1;
      leftArg->location = rightArg->location = -1;

      oe->args = list_make2(leftArg, rightArg);
      oe->location = -1;
      expr->args = lappend(expr->args, oe);

      ReleaseSysCache(opInfo);
    }

    ++attno;
  }

  /* Populate the JOIN RTE's eref / joinaliasvars / joinleftcols /
   * joinrightcols by walking the larg and rarg subqueries' targetLists.
   * Execution doesn't need these (outer Vars reference the input RTEs
   * directly), but PostgreSQL's ruleutils deparser walks them when
   * pg_get_querydef / EXPLAIN VERBOSE traverse the rewritten tree and
   * segfaults on NULL eref. Non-USING LEFT JOIN: joinmergedcols = 0,
   * output is left columns followed by right columns. */
  {
    RangeTblRef *larg_ref = (RangeTblRef *)setOps->larg;
    RangeTblRef *rarg_ref = (RangeTblRef *)setOps->rarg;
    RangeTblEntry *larg_rte =
      (RangeTblEntry *)list_nth(q->rtable, larg_ref->rtindex - 1);
    RangeTblEntry *rarg_rte =
      (RangeTblEntry *)list_nth(q->rtable, rarg_ref->rtindex - 1);
    List *aliasvars = NIL;
    List *leftcols = NIL;
    List *rightcols = NIL;
    List *colnames = NIL;
    ListCell *lc_te;
    int colno;

    colno = 1;
    foreach (lc_te, larg_rte->subquery->targetList) {
      TargetEntry *te = (TargetEntry *)lfirst(lc_te);
      if (te->resjunk) {
        colno++;
        continue;
      }
      aliasvars = lappend(aliasvars,
                          makeVar(larg_ref->rtindex, colno,
                                  exprType((Node *)te->expr),
                                  exprTypmod((Node *)te->expr),
                                  exprCollation((Node *)te->expr),
                                  0));
      leftcols = lappend_int(leftcols, colno);
      rightcols = lappend_int(rightcols, 0);
      colnames = lappend(colnames,
                         makeString(pstrdup(te->resname ? te->resname
                                                        : "?column?")));
      colno++;
    }
    colno = 1;
    foreach (lc_te, rarg_rte->subquery->targetList) {
      TargetEntry *te = (TargetEntry *)lfirst(lc_te);
      if (te->resjunk) {
        colno++;
        continue;
      }
      aliasvars = lappend(aliasvars,
                          makeVar(rarg_ref->rtindex, colno,
                                  exprType((Node *)te->expr),
                                  exprTypmod((Node *)te->expr),
                                  exprCollation((Node *)te->expr),
                                  0));
      leftcols = lappend_int(leftcols, 0);
      rightcols = lappend_int(rightcols, colno);
      colnames = lappend(colnames,
                         makeString(pstrdup(te->resname ? te->resname
                                                        : "?column?")));
      colno++;
    }

    rte->alias = NULL;
    rte->eref = makeAlias("unnamed_join", colnames);
    rte->joinaliasvars = aliasvars;
#if PG_VERSION_NUM >= 130000
    rte->joinleftcols = leftcols;
    rte->joinrightcols = rightcols;
    rte->joinmergedcols = 0;
#else
    (void) leftcols;
    (void) rightcols;
#endif
  }

  rte->rtekind = RTE_JOIN;
  rte->jointype = JOIN_LEFT;

  q->rtable = lappend(q->rtable, rte);

  je->jointype = JOIN_LEFT;

  je->larg = setOps->larg;
  je->rarg = setOps->rarg;
  je->quals = (Node *)expr;
  je->rtindex = list_length(q->rtable);

  fe->fromlist = list_make1(je);

  q->jointree = fe;

  // TODO: Add group by in the right-side table

  q->setOperations = 0;

  return true;
}

/* -------------------------------------------------------------------------
 * Outer-join lowering (LEFT JOIN)
 *
 * ProvSQL builds provenance by annotating the all-present instance, which is
 * sound for monotone SPJU but WRONG for the non-monotone outer join: the
 * null-padded row (r, NULL) of a LEFT JOIN appears only in the *smaller*
 * worlds where the right side has no match for r, so for a left row that does
 * match in the actual instance ProvSQL has nothing to annotate.  The
 * RTE_JOIN arm of process_query historically treats LEFT/FULL/RIGHT exactly
 * like INNER, emitting only the matched branch.
 *
 * The fix is a structural transform applied in the planner hook before
 * provenance discovery.  R ⟕_θ S is rewritten as
 *
 *     ( SELECT R.cols, S.cols FROM R JOIN S ON θ )            -- matched (⊗)
 *     UNION ALL                                               -- ⊎ (plus)
 *     ( SELECT R.cols, NULL,…,NULL
 *       FROM ( SELECT R.cols FROM R
 *              EXCEPT ALL                                     -- ProvSQL's −
 *              SELECT R.cols FROM R JOIN S ON θ ) )           --  R(r)⊗(1⊖⊕match)
 *
 * Both UNION ALL and EXCEPT ALL → − are native (process_set_operation_union /
 * transform_except_into_join), so this code is pure parse-tree construction
 * plus an outer Var remap: the recursive process_query passes over the
 * constructed subqueries do all the provenance work.  The antijoin provenance
 * R(r) ⊖ ⊕_match (R(r)⊗S(s)) that ProvSQL's EXCEPT (NOT-IN semantics) builds
 * equals R(r) ⊗ (1 ⊖ ⊕_match S(s)), exactly the paper's null-padded branch.
 * ------------------------------------------------------------------------- */

/**
 * @brief Rename the @c provsql column in @p rel's @c eref so a later
 *        @c get_provenance_attributes pass does not re-detect @p rel as a
 *        provenance source.
 *
 * Used when a relation's provenance has already been captured elsewhere -- by
 * an explode-style subquery (the aggregation rewrite) or, in the outer-join
 * lowering, by the replacement UNION subquery, leaving the original base
 * relation orphaned in the range table.  Renaming only the (unreferenced)
 * @c eref entry is enough: detection matches on the @c eref colname.
 */
static void hide_provsql_colname(RangeTblEntry *rel) {
  ListCell *lc;
  foreach (lc, rel->eref->colnames) {
    if (!strcmp(strVal(lfirst(lc)), PROVSQL_COLUMN_NAME)) {
      lfirst(lc) = makeString(pstrdup("_provsql_inner"));
      break;
    }
  }
}

/** @brief Per-relation user-column descriptor for the outer-join lowering. */
typedef struct oj_cols {
  int n;             ///< number of user (non-provsql, non-dropped) columns
  AttrNumber *attno; ///< original attribute number in the base relation
  Oid *type;         ///< column type OID
  int32 *typmod;     ///< column typmod
  Oid *coll;         ///< column collation OID
  char **name;       ///< column name
} oj_cols;

/** @brief Collect the user columns (skipping @c provsql and dropped columns)
 *         of an outer-join arm: a base relation or a subquery.  For a relation
 *         the column @c attno is its catalog attribute number; for a subquery
 *         it is the target entry's @c resno. */
static void oj_collect_cols(const constants_t *constants, RangeTblEntry *rel,
                            oj_cols *out) {
  ListCell *lc;

  if (rel->rtekind == RTE_SUBQUERY) {
    int cap = list_length(rel->subquery->targetList);
    out->attno  = (AttrNumber *)palloc(cap * sizeof(AttrNumber));
    out->type   = (Oid *)palloc(cap * sizeof(Oid));
    out->typmod = (int32 *)palloc(cap * sizeof(int32));
    out->coll   = (Oid *)palloc(cap * sizeof(Oid));
    out->name   = (char **)palloc(cap * sizeof(char *));
    out->n = 0;
    foreach (lc, rel->subquery->targetList) {
      TargetEntry *te = (TargetEntry *)lfirst(lc);
      if (te->resjunk)
        continue;
      if (te->resname && !strcmp(te->resname, PROVSQL_COLUMN_NAME))
        continue;
      out->attno[out->n]  = te->resno;
      out->type[out->n]   = exprType((Node *)te->expr);
      out->typmod[out->n] = exprTypmod((Node *)te->expr);
      out->coll[out->n]   = exprCollation((Node *)te->expr);
      out->name[out->n]   = pstrdup(te->resname ? te->resname : "?column?");
      ++out->n;
    }
    return;
  }

  {
    AttrNumber attid = 0;
    int cap = list_length(rel->eref->colnames);
    out->attno  = (AttrNumber *)palloc(cap * sizeof(AttrNumber));
    out->type   = (Oid *)palloc(cap * sizeof(Oid));
    out->typmod = (int32 *)palloc(cap * sizeof(int32));
    out->coll   = (Oid *)palloc(cap * sizeof(Oid));
    out->name   = (char **)palloc(cap * sizeof(char *));
    out->n = 0;
    foreach (lc, rel->eref->colnames) {
      const char *v = strVal(lfirst(lc));
      Oid t;
      int32 tm;
      Oid c;
      ++attid;
      if (v[0] == '\0') /* dropped column */
        continue;
      if (!strcmp(v, PROVSQL_COLUMN_NAME))
        continue;
      get_atttypetypmodcoll(rel->relid, attid, &t, &tm, &c);
      out->attno[out->n]  = attid;
      out->type[out->n]   = t;
      out->typmod[out->n] = tm;
      out->coll[out->n]   = c;
      out->name[out->n]   = pstrdup(v);
      ++out->n;
    }
  }
}

/** @brief True if @p rel contributes provenance: a base relation with a
 *         @c provsql UUID column, or a subquery over tracked relations. */
static bool oj_rte_has_provsql(const constants_t *constants,
                               RangeTblEntry *rel) {
  ListCell *lc;
  AttrNumber attid = 0;

  if (rel->rtekind == RTE_SUBQUERY) {
    if (rel->subquery == NULL)
      return false;
    if (has_provenance(constants, rel->subquery))
      return true;
    /* Also tracked if the subquery already exposes a provsql UUID column
     * (e.g. the synthetic gate_one() column the wrap adds for an untracked
     * outer). */
    foreach (lc, rel->subquery->targetList) {
      TargetEntry *te = (TargetEntry *)lfirst(lc);
      if (!te->resjunk && te->resname &&
          !strcmp(te->resname, PROVSQL_COLUMN_NAME) &&
          exprType((Node *)te->expr) == constants->OID_TYPE_UUID)
        return true;
    }
    return false;
  }

  foreach (lc, rel->eref->colnames) {
    ++attid;
    if (!strcmp(strVal(lfirst(lc)), PROVSQL_COLUMN_NAME) &&
        get_atttype(rel->relid, attid) == constants->OID_TYPE_UUID)
      return true;
  }
  return false;
}

/** @brief Wrap a constructed @c Query as an @c RTE_SUBQUERY, building its
 *         @c eref->colnames from the (non-junk) target list. */
static RangeTblEntry *oj_make_subquery_rte(Query *sub) {
  RangeTblEntry *rte = makeNode(RangeTblEntry);
  List *colnames = NIL;
  ListCell *lc;

  foreach (lc, sub->targetList) {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    if (te->resjunk)
      continue;
    colnames = lappend(colnames,
                       makeString(pstrdup(te->resname ? te->resname
                                                      : "?column?")));
  }

  rte->rtekind  = RTE_SUBQUERY;
  rte->subquery = sub;
  rte->alias    = NULL;
  rte->eref     = makeAlias("unnamed_subquery", colnames);
  rte->lateral  = false;
  rte->inFromCl = true;
#if PG_VERSION_NUM < 160000
  rte->requiredPerms = 0;
#endif
  return rte;
}

/** @brief Copy an outer-join arm RTE into the range table of subquery @p sub.
 *  A base relation carries its permission info (PG 16+); a subquery has no
 *  direct permissions (its inner query keeps its own rteperminfos). */
static RangeTblEntry *oj_copy_rel(Query *outer, Query *sub,
                                  RangeTblEntry *orig) {
  RangeTblEntry *c = copyObject(orig);
#if PG_VERSION_NUM >= 160000
  if (orig->rtekind == RTE_RELATION && orig->perminfoindex != 0) {
    RTEPermissionInfo *pi = getRTEPermissionInfo(outer->rteperminfos, orig);
    sub->rteperminfos = lappend(sub->rteperminfos, copyObject(pi));
    c->perminfoindex  = list_length(sub->rteperminfos);
  } else {
    c->perminfoindex = 0;
  }
#else
  (void)outer;
  (void)sub;
#endif
  return c;
}

/** @brief Neutralise an outer-join arm RTE left orphaned after the lowering so
 *  get_provenance_attributes does not re-pick it up as a provenance source: a
 *  base relation has its provsql column renamed; a subquery (which would still
 *  be processed) is turned into an inert RTE_RESULT. */
static void oj_neutralize_orphan_arm(RangeTblEntry *rel) {
  if (rel->rtekind == RTE_SUBQUERY) {
    rel->rtekind = RTE_RESULT;
    rel->subquery = NULL;
    rel->eref = makeAlias("*RESULT*", NIL);
#if PG_VERSION_NUM >= 160000
    rel->perminfoindex = 0;
#endif
    return;
  }
  hide_provsql_colname(rel);
}

/** @brief Var-renumber context: map @c varno @c from[i] → @c to[i]. */
typedef struct oj_renum_ctx {
  int npairs;
  Index from[2];
  Index to[2];
} oj_renum_ctx;

static Node *oj_renum_mut(Node *node, void *cx) {
  oj_renum_ctx *c = (oj_renum_ctx *)cx;
  if (node == NULL)
    return NULL;
  if (IsA(node, Var)) {
    Var *v = (Var *)node;
    if (v->varlevelsup == 0) {
      int i;
      for (i = 0; i < c->npairs; ++i)
        if (v->varno == c->from[i]) {
          v = (Var *)copyObject(v);
          v->varno = c->to[i];
          v->varnullingrels = NULL;
#if PG_VERSION_NUM >= 130000
          v->varnosyn = 0;
          v->varattnosyn = 0;
#endif
          return (Node *)v;
        }
    }
    return node;
  }
  return expression_tree_mutator(node, oj_renum_mut, cx);
}

/** @brief Build the inner-join scan subquery
 *         @c "SELECT [R.cols][, S.cols] FROM R JOIN S ON θ".
 *
 *  Projects R's columns when @p select_r and S's columns when @p select_s, in
 *  R-then-S order.  R is copied at index 1, S at index 2, the synthetic join
 *  RTE at index 3; θ is copied and its base-relation varnos remapped
 *  (R_idx→1, S_idx→2). */
static Query *oj_build_join_query(const constants_t *constants, Query *outer,
                                  RangeTblEntry *R, RangeTblEntry *S,
                                  Index R_idx, Index S_idx, oj_cols *Rc,
                                  oj_cols *Sc, Node *theta, bool select_r,
                                  bool select_s) {
  Query *sub = makeNode(Query);
  RangeTblEntry *Rcopy, *Scopy, *jrte = makeNode(RangeTblEntry);
  JoinExpr *je = makeNode(JoinExpr);
  RangeTblRef *lr = makeNode(RangeTblRef), *rr = makeNode(RangeTblRef);
  FromExpr *fe = makeNode(FromExpr);
  List *tl = NIL, *av = NIL, *lcols = NIL, *rcols = NIL, *cn = NIL;
  Node *theta2;
  oj_renum_ctx rctx;
  int i;

  sub->commandType = CMD_SELECT;
  sub->canSetTag = true;
  Rcopy = oj_copy_rel(outer, sub, R);
  Scopy = oj_copy_rel(outer, sub, S);

  /* Synthetic join RTE: eref / joinaliasvars / joinleftcols / joinrightcols
   * kept consistent so the ruleutils deparser does not segfault. */
  for (i = 0; i < Rc->n; ++i) {
    av = lappend(av, makeVar(1, Rc->attno[i], Rc->type[i], Rc->typmod[i],
                             Rc->coll[i], 0));
    lcols = lappend_int(lcols, Rc->attno[i]);
    rcols = lappend_int(rcols, 0);
    cn = lappend(cn, makeString(pstrdup(Rc->name[i])));
  }
  for (i = 0; i < Sc->n; ++i) {
    av = lappend(av, makeVar(2, Sc->attno[i], Sc->type[i], Sc->typmod[i],
                             Sc->coll[i], 0));
    lcols = lappend_int(lcols, 0);
    rcols = lappend_int(rcols, Sc->attno[i]);
    cn = lappend(cn, makeString(pstrdup(Sc->name[i])));
  }
  jrte->rtekind = RTE_JOIN;
  jrte->jointype = JOIN_INNER;
  jrte->alias = NULL;
  jrte->eref = makeAlias("unnamed_join", cn);
  jrte->joinaliasvars = av;
#if PG_VERSION_NUM >= 130000
  jrte->joinleftcols = lcols;
  jrte->joinrightcols = rcols;
  jrte->joinmergedcols = 0;
#endif
  jrte->inFromCl = true;

  sub->rtable = list_make3(Rcopy, Scopy, jrte);

  rctx.npairs = 2;
  rctx.from[0] = R_idx; rctx.to[0] = 1;
  rctx.from[1] = S_idx; rctx.to[1] = 2;
  theta2 = oj_renum_mut(copyObject(theta), &rctx);

  lr->rtindex = 1;
  rr->rtindex = 2;
  je->jointype = JOIN_INNER;
  je->larg = (Node *)lr;
  je->rarg = (Node *)rr;
  je->quals = theta2;
  je->isNatural = false;
  je->usingClause = NIL;
  je->rtindex = 3;
  fe->fromlist = list_make1(je);
  sub->jointree = fe;

  if (select_r)
    for (i = 0; i < Rc->n; ++i) {
      Var *v = makeVar(1, Rc->attno[i], Rc->type[i], Rc->typmod[i],
                       Rc->coll[i], 0);
      tl = lappend(tl, makeTargetEntry((Expr *)v, list_length(tl) + 1,
                                       pstrdup(Rc->name[i]), false));
    }
  if (select_s)
    for (i = 0; i < Sc->n; ++i) {
      Var *v = makeVar(2, Sc->attno[i], Sc->type[i], Sc->typmod[i],
                       Sc->coll[i], 0);
      tl = lappend(tl, makeTargetEntry((Expr *)v, list_length(tl) + 1,
                                       pstrdup(Sc->name[i]), false));
    }
  sub->targetList = tl;

  return sub;
}

/** @brief Build the plain-scan subquery @c "SELECT R.cols FROM R". */
static Query *oj_build_rel_query(const constants_t *constants, Query *outer,
                                 RangeTblEntry *R, oj_cols *Rc) {
  Query *sub = makeNode(Query);
  RangeTblEntry *Rcopy;
  RangeTblRef *rtr = makeNode(RangeTblRef);
  FromExpr *fe = makeNode(FromExpr);
  List *tl = NIL;
  int i;

  sub->commandType = CMD_SELECT;
  sub->canSetTag = true;
  Rcopy = oj_copy_rel(outer, sub, R);
  sub->rtable = list_make1(Rcopy);
  rtr->rtindex = 1;
  fe->fromlist = list_make1(rtr);
  sub->jointree = fe;

  for (i = 0; i < Rc->n; ++i) {
    Var *v = makeVar(1, Rc->attno[i], Rc->type[i], Rc->typmod[i], Rc->coll[i],
                     0);
    tl = lappend(tl, makeTargetEntry((Expr *)v, i + 1, pstrdup(Rc->name[i]),
                                     false));
  }
  sub->targetList = tl;
  return sub;
}

/** @brief Build the difference subquery for the kept side of an outer join:
 *  @c "SELECT X.cols FROM X EXCEPT ALL SELECT X.cols FROM R JOIN S ON θ",
 *  where X = R when @p keep_left, else S.
 *
 *  Processed natively as an EXCEPT (→ ProvSQL's −), yielding per distinct kept
 *  tuple x the monus provenance X(x) ⊖ ⊕_match (R(r)⊗S(s)) =
 *  X(x) ⊗ (1 ⊖ ⊕_match Y(y)) -- the null-padded antijoin branch of the join. */
static Query *oj_build_diff(const constants_t *constants, Query *outer,
                            RangeTblEntry *R, RangeTblEntry *S, Index R_idx,
                            Index S_idx, oj_cols *Rc, oj_cols *Sc, Node *theta,
                            bool keep_left) {
  RangeTblEntry *kept_rel = keep_left ? R : S;
  oj_cols *Kc = keep_left ? Rc : Sc;
  Query *ls = oj_build_rel_query(constants, outer, kept_rel, Kc);
  /* Matched-projection arm projects the kept side's columns only. */
  Query *mp = oj_build_join_query(constants, outer, R, S, R_idx, S_idx, Rc, Sc,
                                  theta, keep_left, !keep_left);
  RangeTblEntry *ls_rte = oj_make_subquery_rte(ls);
  RangeTblEntry *mp_rte = oj_make_subquery_rte(mp);
  Query *D = makeNode(Query);
  SetOperationStmt *so = makeNode(SetOperationStmt);
  RangeTblRef *l = makeNode(RangeTblRef), *r = makeNode(RangeTblRef);
  FromExpr *fe = makeNode(FromExpr);
  List *tl = NIL;
  int i;

  D->commandType = CMD_SELECT;
  D->canSetTag = true;
  D->rtable = list_make2(ls_rte, mp_rte);

  l->rtindex = 1;
  r->rtindex = 2;
  so->op = SETOP_EXCEPT;
  /* EXCEPT ALL = the pure multiset difference q₁−q₂ (NOT IN), which keeps every
   * kept-side row with its multiplicity -- exactly the antijoin's null-padded
   * rows.  group_set_difference_right_arm groups the matched-projection arm so
   * the monus is X(x) ⊖ ⊕(R(r)⊗S(s)) = X(x) ⊗ (1 ⊖ ⊕ Y(y)). */
  so->all = true;
  so->larg = (Node *)l;
  so->rarg = (Node *)r;
  for (i = 0; i < Kc->n; ++i) {
    so->colTypes = lappend_oid(so->colTypes, Kc->type[i]);
    so->colTypmods = lappend_int(so->colTypmods, Kc->typmod[i]);
    so->colCollations = lappend_oid(so->colCollations, Kc->coll[i]);
  }
  D->setOperations = (Node *)so;
  fe->fromlist = NIL;
  D->jointree = fe;

  for (i = 0; i < Kc->n; ++i) {
    Var *v = makeVar(1, i + 1, Kc->type[i], Kc->typmod[i], Kc->coll[i], 0);
    tl = lappend(tl, makeTargetEntry((Expr *)v, i + 1, pstrdup(Kc->name[i]),
                                     false));
  }
  D->targetList = tl;
  return D;
}

/** @brief Build a null-padded antijoin arm in R-then-S column order.
 *
 *  For @p keep_left it emits the left-unmatched rows
 *  @c "SELECT D.cols, NULL,…  FROM (R EXCEPT ALL R⋈S) D" (S columns NULL); for
 *  the right side it emits @c "SELECT NULL,…, D.cols FROM (S EXCEPT ALL R⋈S) D"
 *  (R columns NULL).  The kept side's columns come from the difference @c D
 *  (which also carries the antijoin provenance); the other side is typed NULL
 *  constants. */
static Query *oj_build_antijoin(const constants_t *constants, Query *outer,
                                RangeTblEntry *R, RangeTblEntry *S,
                                Index R_idx, Index S_idx, oj_cols *Rc,
                                oj_cols *Sc, Node *theta, bool keep_left) {
  Query *D = oj_build_diff(constants, outer, R, S, R_idx, S_idx, Rc, Sc, theta,
                           keep_left);
  RangeTblEntry *D_rte = oj_make_subquery_rte(D);
  Query *A = makeNode(Query);
  RangeTblRef *rtr = makeNode(RangeTblRef);
  FromExpr *fe = makeNode(FromExpr);
  List *tl = NIL;
  int i, kept = 0; /* next column position in the difference D */

  A->commandType = CMD_SELECT;
  A->canSetTag = true;
  A->rtable = list_make1(D_rte);
  rtr->rtindex = 1;
  fe->fromlist = list_make1(rtr);
  A->jointree = fe;

  /* R columns: from D when keep_left, else typed NULL. */
  for (i = 0; i < Rc->n; ++i) {
    Expr *e;
    if (keep_left)
      e = (Expr *)makeVar(1, ++kept, Rc->type[i], Rc->typmod[i], Rc->coll[i],
                          0);
    else
      e = (Expr *)makeNullConst(Rc->type[i], Rc->typmod[i], Rc->coll[i]);
    tl = lappend(tl, makeTargetEntry(e, list_length(tl) + 1,
                                     pstrdup(Rc->name[i]), false));
  }
  /* S columns: typed NULL when keep_left, else from D. */
  for (i = 0; i < Sc->n; ++i) {
    Expr *e;
    if (keep_left)
      e = (Expr *)makeNullConst(Sc->type[i], Sc->typmod[i], Sc->coll[i]);
    else
      e = (Expr *)makeVar(1, ++kept, Sc->type[i], Sc->typmod[i], Sc->coll[i],
                          0);
    tl = lappend(tl, makeTargetEntry(e, list_length(tl) + 1,
                                     pstrdup(Sc->name[i]), false));
  }
  A->targetList = tl;
  return A;
}

/** @brief Build the column-type lists (R-then-S, user columns only) shared by
 *         every set-operation node of the replacement union. */
static void oj_build_coltype_lists(oj_cols *Rc, oj_cols *Sc, List **types,
                                   List **typmods, List **collations) {
  int i;
  *types = *typmods = *collations = NIL;
  for (i = 0; i < Rc->n; ++i) {
    *types = lappend_oid(*types, Rc->type[i]);
    *typmods = lappend_int(*typmods, Rc->typmod[i]);
    *collations = lappend_oid(*collations, Rc->coll[i]);
  }
  for (i = 0; i < Sc->n; ++i) {
    *types = lappend_oid(*types, Sc->type[i]);
    *typmods = lappend_int(*typmods, Sc->typmod[i]);
    *collations = lappend_oid(*collations, Sc->coll[i]);
  }
}

/** @brief Build the UNION-ALL of the matched arm and the outer join's
 *         antijoin arm(s): the full outer-join relation in R-then-S column
 *         order with one combined @c provsql column.
 *
 *  @p jointype selects which null-padded antijoin branches are added:
 *  @c JOIN_LEFT adds the left (R-kept) branch, @c JOIN_RIGHT the right
 *  (S-kept) branch, @c JOIN_FULL both. */
static Query *oj_build_union(const constants_t *constants, Query *outer,
                             RangeTblEntry *R, RangeTblEntry *S, Index R_idx,
                             Index S_idx, oj_cols *Rc, oj_cols *Sc, Node *theta,
                             JoinType jointype) {
  List *arms = NIL; /* list of arm Query* */
  List *types, *typmods, *collations;
  Query *Q = makeNode(Query);
  FromExpr *fe = makeNode(FromExpr);
  List *tl = NIL;
  Node *tree;
  ListCell *lc;
  int i, pos = 0, k;

  /* Matched arm (R ⋈ S), then the requested antijoin branches. */
  arms = lappend(arms, oj_build_join_query(constants, outer, R, S, R_idx,
                                           S_idx, Rc, Sc, theta, true, true));
  if (jointype == JOIN_LEFT || jointype == JOIN_FULL)
    arms = lappend(arms, oj_build_antijoin(constants, outer, R, S, R_idx,
                                           S_idx, Rc, Sc, theta, true));
  if (jointype == JOIN_RIGHT || jointype == JOIN_FULL)
    arms = lappend(arms, oj_build_antijoin(constants, outer, R, S, R_idx,
                                           S_idx, Rc, Sc, theta, false));

  Q->commandType = CMD_SELECT;
  Q->canSetTag = true;
  foreach (lc, arms)
    Q->rtable = lappend(Q->rtable, oj_make_subquery_rte((Query *)lfirst(lc)));

  oj_build_coltype_lists(Rc, Sc, &types, &typmods, &collations);

  /* Left-deep UNION ALL tree over the arm RTEs (indices 1..n).  Every
   * SetOperationStmt node carries its own colTypes lists -- process_set_
   * operation_union appends the UUID type to each node in place. */
  {
    RangeTblRef *first = makeNode(RangeTblRef);
    first->rtindex = 1;
    tree = (Node *)first;
  }
  for (k = 2; k <= list_length(arms); ++k) {
    SetOperationStmt *so = makeNode(SetOperationStmt);
    RangeTblRef *rtr = makeNode(RangeTblRef);
    rtr->rtindex = k;
    so->op = SETOP_UNION;
    so->all = true;
    so->larg = tree;
    so->rarg = (Node *)rtr;
    so->colTypes = list_copy(types);
    so->colTypmods = list_copy(typmods);
    so->colCollations = list_copy(collations);
    tree = (Node *)so;
  }
  Q->setOperations = tree;
  fe->fromlist = NIL;
  Q->jointree = fe;

  /* Leader target list: one Var per user column, referencing the first arm. */
  for (i = 0; i < Rc->n; ++i) {
    Var *v = makeVar(1, ++pos, Rc->type[i], Rc->typmod[i], Rc->coll[i], 0);
    tl = lappend(tl, makeTargetEntry((Expr *)v, pos, pstrdup(Rc->name[i]),
                                     false));
  }
  for (i = 0; i < Sc->n; ++i) {
    Var *v = makeVar(1, ++pos, Sc->type[i], Sc->typmod[i], Sc->coll[i], 0);
    tl = lappend(tl, makeTargetEntry((Expr *)v, pos, pstrdup(Sc->name[i]),
                                     false));
  }
  Q->targetList = tl;
  return Q;
}

/** @brief Walker context: detect a Var referencing the join RTE index. */
typedef struct oj_joinref_ctx {
  Index join_idx;
} oj_joinref_ctx;

static bool oj_joinref_walker(Node *node, void *cx) {
  oj_joinref_ctx *c = (oj_joinref_ctx *)cx;
  if (node == NULL)
    return false;
  if (IsA(node, Var)) {
    Var *v = (Var *)node;
    return (v->varlevelsup == 0 && v->varno == c->join_idx);
  }
  return expression_tree_walker(node, oj_joinref_walker, cx);
}

/** @brief True if any outer Var references the join RTE directly (USING /
 *         whole-row / alias.col references the conservative remap cannot
 *         resolve through @c joinaliasvars yet). */
static bool oj_refs_join_index(Query *q, Index join_idx) {
  oj_joinref_ctx c;
  c.join_idx = join_idx;
  if (oj_joinref_walker((Node *)q->targetList, &c))
    return true;
  if (q->jointree && q->jointree->quals &&
      oj_joinref_walker(q->jointree->quals, &c))
    return true;
  if (q->havingQual && oj_joinref_walker(q->havingQual, &c))
    return true;
  return false;
}

/** @brief Outer Var remap context for the LEFT-join lowering: base-relation
 *         Vars (R_idx / S_idx) are retargeted to the new subquery (new_idx)
 *         with their attribute number mapped to the subquery column position. */
typedef struct oj_outer_ctx {
  Index R_idx, S_idx, new_idx;
  AttrNumber *R_map, *S_map;
} oj_outer_ctx;

static Node *oj_outer_remap(Node *node, void *cx) {
  oj_outer_ctx *c = (oj_outer_ctx *)cx;
  if (node == NULL)
    return NULL;
  if (IsA(node, Var)) {
    Var *v = (Var *)node;
    if (v->varlevelsup == 0 &&
        (v->varno == c->R_idx || v->varno == c->S_idx)) {
      v = (Var *)copyObject(v);
      if ((Index)((Var *)node)->varno == c->R_idx)
        v->varattno = c->R_map[v->varattno];
      else
        v->varattno = c->S_map[v->varattno];
      v->varno = c->new_idx;
      v->varnullingrels = NULL;
#if PG_VERSION_NUM >= 130000
      v->varnosyn = 0;
      v->varattnosyn = 0;
#endif
      return (Node *)v;
    }
    return node;
  }
  return expression_tree_mutator(node, oj_outer_remap, cx);
}

/**
 * @brief Lower a top-level outer @c JOIN of two base relations into the
 *        UNION-ALL of its matched and null-padded antijoin arms.
 *
 * Fires only on @c jointree->fromlist ==
 * @c [JoinExpr(JOIN_LEFT|JOIN_RIGHT|JOIN_FULL, RTR, RTR)] whose arms are
 * provenance-tracked base relations and where no outer Var references the join
 * RTE directly.  Everything else falls through unchanged.  Returns @c true if
 * the query was rewritten.
 */
static bool lower_outer_joins(const constants_t *constants, Query *q) {
  JoinExpr *je;
  RangeTblRef *lref, *rref;
  Index R_idx, S_idx, join_idx;
  RangeTblEntry *R_rte, *S_rte;
  oj_cols Rc, Sc;
  Node *theta;
  Query *Q;
  AttrNumber *R_map, *S_map;
  int ncolR, ncolS, i;
  oj_outer_ctx octx;

  if (q->commandType != CMD_SELECT)
    return false;
  if (!q->jointree || list_length(q->jointree->fromlist) != 1)
    return false;
  if (!IsA(linitial(q->jointree->fromlist), JoinExpr))
    return false;
  je = (JoinExpr *)linitial(q->jointree->fromlist);
  if (je->jointype != JOIN_LEFT && je->jointype != JOIN_RIGHT &&
      je->jointype != JOIN_FULL)
    return false;
  if (!IsA(je->larg, RangeTblRef) || !IsA(je->rarg, RangeTblRef))
    return false;

  lref = (RangeTblRef *)je->larg;
  rref = (RangeTblRef *)je->rarg;
  R_idx = lref->rtindex;
  S_idx = rref->rtindex;
  join_idx = je->rtindex;
  R_rte = list_nth_node(RangeTblEntry, q->rtable, R_idx - 1);
  S_rte = list_nth_node(RangeTblEntry, q->rtable, S_idx - 1);

  if ((R_rte->rtekind != RTE_RELATION && R_rte->rtekind != RTE_SUBQUERY) ||
      (S_rte->rtekind != RTE_RELATION && S_rte->rtekind != RTE_SUBQUERY))
    return false;
  if ((R_rte->rtekind == RTE_SUBQUERY && R_rte->lateral) ||
      (S_rte->rtekind == RTE_SUBQUERY && S_rte->lateral))
    return false;
  if (!oj_rte_has_provsql(constants, R_rte) ||
      !oj_rte_has_provsql(constants, S_rte))
    return false;
  if (oj_refs_join_index(q, join_idx))
    return false;

#if PG_VERSION_NUM >= 180000
  /* Flatten PG 18's synthetic RTE_GROUP so grouped-column Vars are base-
   * relation Vars again, which the remap below can retarget. */
  strip_group_rte_pg18(q);
#endif

  theta = je->quals;
  oj_collect_cols(constants, R_rte, &Rc);
  oj_collect_cols(constants, S_rte, &Sc);
  ncolR = (R_rte->rtekind == RTE_SUBQUERY)
            ? list_length(R_rte->subquery->targetList)
            : list_length(R_rte->eref->colnames);
  ncolS = (S_rte->rtekind == RTE_SUBQUERY)
            ? list_length(S_rte->subquery->targetList)
            : list_length(S_rte->eref->colnames);

  Q = oj_build_union(constants, q, R_rte, S_rte, R_idx, S_idx, &Rc, &Sc, theta,
                     je->jointype);

  /* The combined provenance now lives in the replacement subquery Q.  The
   * original arm RTEs are left orphaned in the outer range table; neutralise
   * them so get_provenance_attributes does not pick them up again. */
  oj_neutralize_orphan_arm(R_rte);
  oj_neutralize_orphan_arm(S_rte);

  R_map = (AttrNumber *)palloc0((ncolR + 1) * sizeof(AttrNumber));
  S_map = (AttrNumber *)palloc0((ncolS + 1) * sizeof(AttrNumber));
  for (i = 0; i < Rc.n; ++i)
    R_map[Rc.attno[i]] = i + 1;
  for (i = 0; i < Sc.n; ++i)
    S_map[Sc.attno[i]] = Rc.n + i + 1;

  /* Replace the JOIN RTE slot in place with the new subquery, reusing the
   * join's range-table index for the outer reference.  Reusing the *join*
   * slot (rather than the left relation's) leaves no orphaned RTE_JOIN in the
   * range table -- an orphaned join RTE without a matching JoinExpr trips the
   * planner ("so where are the outer joins?").  The two base-relation RTEs are
   * left orphaned, which the planner tolerates (they are simply not scanned). */
  {
    RangeTblEntry *J_rte =
      list_nth_node(RangeTblEntry, q->rtable, join_idx - 1);
    List *cn = NIL;

    J_rte->rtekind  = RTE_SUBQUERY;
    J_rte->subquery = Q;
    J_rte->jointype = JOIN_INNER;
    J_rte->joinaliasvars = NIL;
#if PG_VERSION_NUM >= 130000
    J_rte->joinleftcols = NIL;
    J_rte->joinrightcols = NIL;
    J_rte->joinmergedcols = 0;
#endif
    J_rte->relid    = InvalidOid;
    J_rte->relkind  = 0;
#if PG_VERSION_NUM >= 120000
    J_rte->rellockmode = 0;
#endif
    J_rte->inh      = false;
    J_rte->lateral  = false;
    J_rte->tablesample = NULL;
#if PG_VERSION_NUM >= 160000
    J_rte->perminfoindex = 0;
#else
    J_rte->selectedCols  = NULL;
    J_rte->insertedCols  = NULL;
    J_rte->updatedCols   = NULL;
    J_rte->requiredPerms = ACL_SELECT;
#endif
    for (i = 0; i < Rc.n; ++i)
      cn = lappend(cn, makeString(pstrdup(Rc.name[i])));
    for (i = 0; i < Sc.n; ++i)
      cn = lappend(cn, makeString(pstrdup(Sc.name[i])));
    J_rte->eref = makeAlias("unnamed_subquery", cn);
  }

  {
    RangeTblRef *newr = makeNode(RangeTblRef);
    newr->rtindex = join_idx;
    q->jointree->fromlist = list_make1(newr);
  }

  octx.R_idx = R_idx;
  octx.S_idx = S_idx;
  octx.new_idx = join_idx;
  octx.R_map = R_map;
  octx.S_map = S_map;
  q->targetList = (List *)oj_outer_remap((Node *)q->targetList, &octx);
  if (q->jointree->quals)
    q->jointree->quals = oj_outer_remap(q->jointree->quals, &octx);
  if (q->havingQual)
    q->havingQual = oj_outer_remap(q->havingQual, &octx);

  return true;
}

/* -------------------------------------------------------------------------
 * Scalar-subquery decorrelation
 *
 * A correlated scalar subquery (SELECT Q.x FROM Q WHERE corr), used as a
 * top-level target-list entry of a query whose FROM is a single tracked base
 * relation R, is decorrelated to a LEFT JOIN:
 *
 *   SELECT R.cols, choose(Q.x)
 *   FROM   R LEFT JOIN Q ON corr
 *   GROUP BY R.cols
 *   HAVING count(Q.key) <= 1
 *
 * The corrected outer-join lowering (lower_outer_joins, which runs next)
 * supplies the 0-match NULL row, choose() picks the single matched value, and
 * the count<=1 HAVING gates out the (SQL-illegal) >=2-match worlds -- no
 * gate-level special case.  Anything outside this shape returns false and the
 * caller's "Subqueries not supported" error still fires.
 * ------------------------------------------------------------------------- */

/** @brief Mutator: lift a scalar subquery's body into the outer query level.
 *  Var(level 0, varno @c q_old) -> Var(level 0, varno @c q_new) [the pulled-up
 *  Q]; the correlated outer Var(level 1) -> Var(level 0). */
typedef struct oj_decorr_ctx {
  Index q_old;
  Index q_new;
} oj_decorr_ctx;

static Node *oj_decorr_var_mut(Node *node, void *cx) {
  oj_decorr_ctx *c = (oj_decorr_ctx *)cx;
  if (node == NULL)
    return NULL;
  if (IsA(node, Var)) {
    Var *v = (Var *)node;
    if (v->varlevelsup == 1) {
      v = (Var *)copyObject(v);
      v->varlevelsup = 0;
      return (Node *)v;
    }
    if (v->varlevelsup == 0 && v->varno == c->q_old) {
      v = (Var *)copyObject(v);
      v->varno = c->q_new;
#if PG_VERSION_NUM >= 130000
      v->varnosyn = 0;
      v->varattnosyn = 0;
#endif
      return (Node *)v;
    }
    return node;
  }
  return expression_tree_mutator(node, oj_decorr_var_mut, cx);
}

/** @brief Walker: count SubLink nodes (capturing the first), and capture a Var
 *  referencing varno @p target_varno (level 0) -- used to find a Q column for
 *  the count() key. */
typedef struct oj_sublink_scan {
  int n_sublinks;
  SubLink *found_sublink;
  Index target_varno; /* find any level-0 Var on this rel */
  Var *found_var;
} oj_sublink_scan;

static bool oj_sublink_scan_walker(Node *node, void *cx) {
  oj_sublink_scan *s = (oj_sublink_scan *)cx;
  if (node == NULL)
    return false;
  if (IsA(node, SubLink)) {
    s->n_sublinks++;
    if (s->found_sublink == NULL)
      s->found_sublink = (SubLink *)node;
  }
  if (IsA(node, Var)) {
    Var *v = (Var *)node;
    if (s->found_var == NULL && v->varlevelsup == 0 &&
        v->varno == s->target_varno)
      s->found_var = v;
  }
  return expression_tree_walker(node, oj_sublink_scan_walker, cx);
}

/** @brief Mutator: replace the specific @c SubLink node @p target (by pointer)
 *  with @p replacement. */
typedef struct oj_sl_replace_ctx {
  SubLink *target;
  Node *replacement;
} oj_sl_replace_ctx;

static Node *oj_sl_replace_mut(Node *node, void *cx) {
  oj_sl_replace_ctx *c = (oj_sl_replace_ctx *)cx;
  if (node == NULL)
    return NULL;
  if (node == (Node *)c->target)
    return c->replacement;
  return expression_tree_mutator(node, oj_sl_replace_mut, cx);
}

/** @brief Walker: true if the subtree contains the specific SubLink @p cx. */
static bool oj_contains_sublink_walker(Node *node, void *cx) {
  if (node == NULL)
    return false;
  if (node == (Node *)cx)
    return true;
  return expression_tree_walker(node, oj_contains_sublink_walker, cx);
}

/** @brief Build an @c Aggref for a single-argument aggregate. */
static Aggref *oj_make_aggref(Oid aggfnoid, Oid aggtype, Oid argtype,
                              Expr *arg) {
  Aggref *agg = makeNode(Aggref);
  TargetEntry *te = makeNode(TargetEntry);
  te->resno = 1;
  te->expr = arg;
  agg->aggfnoid = aggfnoid;
  agg->aggtype = aggtype;
  agg->aggtranstype = InvalidOid;
  agg->aggargtypes = list_make1_oid(argtype);
  agg->args = list_make1(te);
  agg->aggkind = AGGKIND_NORMAL;
  agg->aggsplit = AGGSPLIT_SIMPLE;
  agg->location = -1;
#if PG_VERSION_NUM >= 140000
  agg->aggno = agg->aggtransno = -1;
#endif
  return agg;
}

/**
 * @brief Build @c "count(Q.key) <op> n" over the decorrelated LEFT-JOIN group.
 *
 * @p found_var is some Q column from the correlation (NULL on the null-padded
 * antijoin rows, so it counts only genuine matches); it is re-pointed to the
 * pulled-up Q at @p q_idx.  Used for the scalar-subquery at-most-one-row gate
 * (@c "<= 1") and the WHERE-comparison non-empty gate (@c ">= 1").
 */
static OpExpr *oj_count_cmp(Var *found_var, Index q_idx, const char *opstr,
                            int64 n) {
  Var *qkey = (Var *)copyObject((Node *)found_var);
  Aggref *cnt;
  OpExpr *op = makeNode(OpExpr);
  Oid o;

  qkey->varno = q_idx;
#if PG_VERSION_NUM >= 130000
  qkey->varnosyn = 0;
  qkey->varattnosyn = 0;
#endif
  cnt = oj_make_aggref(F_COUNT_ANY, INT8OID, qkey->vartype, (Expr *)qkey);

  o = OpernameGetOprid(list_make1(makeString((char *)opstr)), INT8OID, INT8OID);
  op->opno = o;
  op->opfuncid = get_opcode(o);
  op->opresulttype = BOOLOID;
  op->opcollid = InvalidOid;
  op->inputcollid = InvalidOid;
  op->args = list_make2(cnt, makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                       Int64GetDatum(n), false, FLOAT8PASSBYVAL));
  op->location = -1;
  return op;
}

/** @brief Var-remap context for the FROM-wrapping pre-step: a Var at
 *  @c target_level on relation @c varno / attribute @c varattno is retargeted to
 *  @c newidx column @c pos[varno][varattno].  Descent into @c skip (the
 *  SubLink) is suppressed. */
typedef struct oj_wrap_ctx {
  int target_level;
  Index newidx;
  int rtlen;
  int **pos;       /* pos[varno][varattno] -> R' column (1-based), or 0 */
  SubLink *skip;
} oj_wrap_ctx;

static Node *oj_wrap_remap_mut(Node *node, void *cx) {
  oj_wrap_ctx *c = (oj_wrap_ctx *)cx;
  if (node == NULL)
    return NULL;
  if (c->skip && node == (Node *)c->skip)
    return node;
  if (IsA(node, Var)) {
    Var *v = (Var *)node;
    if ((int)v->varlevelsup == c->target_level && (int)v->varno >= 1 &&
        (int)v->varno <= c->rtlen && c->pos[v->varno] != NULL &&
        v->varattno >= 1 && c->pos[v->varno][v->varattno] > 0) {
      Var *nv = (Var *)copyObject(v);
      nv->varno = c->newidx;
      nv->varattno = c->pos[v->varno][v->varattno];
#if PG_VERSION_NUM >= 130000
      nv->varnosyn = 0;
      nv->varattnosyn = 0;
#endif
      return (Node *)nv;
    }
    return node;
  }
  return expression_tree_mutator(node, oj_wrap_remap_mut, cx);
}

/**
 * @brief Wrap a non-single-relation outer FROM into a derived subquery R' so a
 *        scalar subquery can be decorrelated onto it.
 *
 * Builds R' = the outer FROM (all its base relations + join RTEs) with the
 * non-subquery WHERE conjuncts, exposing every base-relation user column.  The
 * outer query is rewritten to @c "FROM R'" with all references (the target
 * list, the SubLink's correlation at level 1, and -- for a WHERE SubLink -- the
 * conjunct that will move to HAVING) retargeted to R''s columns.  The FROM must
 * consist only of base relations and join RTEs (no nested subqueries / VALUES /
 * functions); returns @c false otherwise, leaving @p q untouched.
 */
static bool oj_wrap_outer_from(const constants_t *constants, Query *q,
                               SubLink *sl, bool in_where) {
  int rtlen = list_length(q->rtable);
  int **pos = (int **)palloc0((rtlen + 1) * sizeof(int *));
  Query *Rp = makeNode(Query);
  RangeTblEntry *rp_rte;
  RangeTblRef *rtr = makeNode(RangeTblRef);
  FromExpr *outer_fe = makeNode(FromExpr);
  List *rp_tl = NIL;
  Node *subquery_conj = NULL; /* the WHERE conjunct holding the SubLink */
  List *kept_conj = NIL;
  oj_wrap_ctx wc;
  ListCell *lc;
  int idx, posn = 0;
  bool any_tracked = false;

  /* Only base relations and join RTEs are supported in the wrapped FROM. */
  idx = 0;
  foreach (lc, q->rtable) {
    RangeTblEntry *r = (RangeTblEntry *)lfirst(lc);
    ++idx;
    if (r->rtekind == RTE_RELATION) {
      if (oj_rte_has_provsql(constants, r))
        any_tracked = true;
    } else if (r->rtekind != RTE_JOIN) {
      return false;
    }
  }

  /* R' exposes every base-relation user column; record (rtindex,attno)->pos. */
  idx = 0;
  foreach (lc, q->rtable) {
    RangeTblEntry *r = (RangeTblEntry *)lfirst(lc);
    oj_cols rc;
    int j;
    ++idx;
    if (r->rtekind != RTE_RELATION)
      continue;
    oj_collect_cols(constants, r, &rc);
    pos[idx] = (int *)palloc0((list_length(r->eref->colnames) + 1) * sizeof(int));
    for (j = 0; j < rc.n; ++j) {
      Var *v = makeVar(idx, rc.attno[j], rc.type[j], rc.typmod[j], rc.coll[j], 0);
      rp_tl = lappend(rp_tl, makeTargetEntry((Expr *)v, ++posn,
                                             pstrdup(rc.name[j]), false));
      pos[idx][rc.attno[j]] = posn;
    }
  }

  /* When no FROM relation is provenance-tracked, the outer tuples are certain
   * (exactly like joining an untracked table): give R' a synthetic gate_one()
   * provsql column so the decorrelation / outer-join lowering treat it as a
   * certain-provenance arm.  No warning -- no provenance is lost, the outer
   * simply contributes the identity and the subquery's provenance flows. */
  if (!any_tracked) {
    FuncExpr *one = makeNode(FuncExpr);
    one->funcid = constants->OID_FUNCTION_GATE_ONE;
    one->funcresulttype = constants->OID_TYPE_UUID;
    one->args = NIL;
    one->location = -1;
    rp_tl = lappend(rp_tl, makeTargetEntry((Expr *)one, ++posn,
                                           pstrdup(PROVSQL_COLUMN_NAME), false));
  }

  /* Split the WHERE: the conjunct holding the SubLink (for a WHERE SubLink)
   * stays in the outer query (it becomes HAVING); the rest move into R'. */
  if (q->jointree->quals) {
    Node *quals = q->jointree->quals;
    List *conjs = (IsA(quals, BoolExpr) &&
                   ((BoolExpr *)quals)->boolop == AND_EXPR)
                    ? ((BoolExpr *)quals)->args
                    : list_make1(quals);
    foreach (lc, conjs) {
      Node *cnode = (Node *)lfirst(lc);
      if (in_where && oj_contains_sublink_walker(cnode, sl))
        subquery_conj = cnode;
      else
        kept_conj = lappend(kept_conj, cnode);
    }
  }

  /* Build R'. */
  Rp->commandType = CMD_SELECT;
  Rp->canSetTag = true;
  Rp->rtable = q->rtable;
  Rp->jointree = makeNode(FromExpr);
  Rp->jointree->fromlist = q->jointree->fromlist;
  Rp->jointree->quals =
    (kept_conj == NIL)
      ? NULL
      : (list_length(kept_conj) == 1 ? (Node *)linitial(kept_conj)
                                     : (Node *)makeBoolExpr(AND_EXPR, kept_conj,
                                                            -1));
  Rp->targetList = rp_tl;
#if PG_VERSION_NUM >= 160000
  Rp->rteperminfos = q->rteperminfos;
#endif

  /* Retarget references to R': the outer target list (skipping the SubLink),
   * the SubLink body's correlation (level 1), and the retained subquery
   * conjunct (level 0). */
  wc.newidx = 1;
  wc.rtlen = rtlen;
  wc.pos = pos;

  wc.target_level = 0;
  wc.skip = sl;
  q->targetList = (List *)oj_wrap_remap_mut((Node *)q->targetList, &wc);
  if (subquery_conj)
    subquery_conj = oj_wrap_remap_mut(subquery_conj, &wc);

  /* The SubLink body's correlated (level-1) references to the FROM relations
   * become level-1 references to R'.  Walk its target list and quals directly
   * (the mutator does not descend into a Query node). */
  {
    Query *sub = (Query *)sl->subselect;
    wc.target_level = 1;
    wc.skip = NULL;
    sub->targetList = (List *)oj_wrap_remap_mut((Node *)sub->targetList, &wc);
    if (sub->jointree && sub->jointree->quals)
      sub->jointree->quals = oj_wrap_remap_mut(sub->jointree->quals, &wc);
  }

  /* Rebuild the outer query: FROM R', WHERE = the retained subquery conjunct. */
  rp_rte = oj_make_subquery_rte(Rp);
  q->rtable = list_make1(rp_rte);
#if PG_VERSION_NUM >= 160000
  q->rteperminfos = NIL;
#endif
  rtr->rtindex = 1;
  outer_fe->fromlist = list_make1(rtr);
  outer_fe->quals = subquery_conj;
  q->jointree = outer_fe;
  return true;
}

/**
 * @brief Is @p sub a subselect that the predicate-sublink rewrite can turn into
 *        a correlated @c "SELECT count(*) FROM Q WHERE corr"?
 *
 * Requires a single tracked base relation Q and a (correlated) WHERE, with none
 * of the shapes @c decorrelate_scalar_sublinks rejects downstream (aggregates,
 * grouping, set ops, LIMIT, nested sublinks, CTEs).  The targetList is replaced
 * wholesale by @c count(*), so its width is irrelevant here.  @p corr_supplied
 * is set for @c IN / @c NOT @c IN, whose correlation comes from the testexpr and
 * is ANDed into the (possibly empty) subselect WHERE by the caller.
 */
static bool predicate_subselect_decorrelatable(const constants_t *constants,
                                               Query *sub,
                                               bool corr_supplied) {
  RangeTblEntry *qrte;
  if (!IsA(sub, Query) || sub->commandType != CMD_SELECT)
    return false;
  if (sub->groupClause || sub->groupingSets || sub->hasAggs ||
      sub->distinctClause || sub->setOperations || sub->hasWindowFuncs ||
      sub->hasSubLinks || sub->limitCount || sub->limitOffset || sub->cteList ||
      list_length(sub->rtable) != 1)
    return false;
  if (!sub->jointree || (!corr_supplied && !sub->jointree->quals))
    return false; /* an uncorrelated predicate has no Q key for count() */
  qrte = list_nth_node(RangeTblEntry, sub->rtable, 0);
  return qrte->rtekind == RTE_RELATION &&
         oj_rte_has_provsql(constants, qrte);
}

/**
 * @brief Turn a predicate subselect into the boolean @c "(SELECT count(*) FROM Q
 *        WHERE corr) >= 1" (semijoin) or @c "... = 0" (antijoin).
 *
 * @c EXISTS / @c IN are existence tests (@c "⊕Q present"), so they are exactly
 * @c "count(*) >= 1"; @c NOT @c EXISTS / @c NOT @c IN are their antijoin duals,
 * @c "count(*) = 0".  Lowering them to a correlated count() comparison lets the
 * aggregate-body arm of @c decorrelate_scalar_sublinks do the rest: it rewrites
 * @c count(*) to @c count(Q.key) over the @c "R ⟕ Q" group (so the null-padded
 * antijoin row is not counted) and lifts the comparison into @c HAVING -- i.e.
 * the semijoin @c R⊗⊕Q and the antijoin @c R⊗(1⊖⊕Q) fall out of the existing
 * outer-join lowering.
 *
 * @p extra_corr (for @c IN / @c NOT @c IN) is the @c "Q.col = x" correlation
 * lifted out of the testexpr; it is ANDed into the subselect's WHERE.  @c EXISTS
 * passes @c NULL, its correlation already living in the subselect.
 */
static Node *build_count_predicate(Query *subselect, Node *extra_corr,
                                   bool antijoin) {
  Query *sq = (Query *)copyObject(subselect);
  Aggref *cnt = makeNode(Aggref);
  SubLink *sl = makeNode(SubLink);
  OpExpr *op = makeNode(OpExpr);
  Oid o;

  if (extra_corr)
    sq->jointree->quals =
      sq->jointree->quals
        ? (Node *)makeBoolExpr(AND_EXPR,
                               list_make2(sq->jointree->quals, extra_corr), -1)
        : extra_corr;

  cnt->aggfnoid = F_COUNT_; /* count(*) */
  cnt->aggtype = INT8OID;
  cnt->aggtranstype = InvalidOid;
  cnt->aggargtypes = NIL;
  cnt->args = NIL;
  cnt->aggstar = true;
  cnt->aggkind = AGGKIND_NORMAL;
  cnt->aggsplit = AGGSPLIT_SIMPLE;
  cnt->location = -1;
#if PG_VERSION_NUM >= 140000
  cnt->aggno = cnt->aggtransno = -1;
#endif
  sq->targetList =
    list_make1(makeTargetEntry((Expr *)cnt, 1, pstrdup("count"), false));
  sq->hasAggs = true;

  sl->subLinkType = EXPR_SUBLINK;
  sl->subselect = (Node *)sq;
  sl->testexpr = NULL;
  sl->operName = NIL;
  sl->location = -1;

  o = OpernameGetOprid(list_make1(makeString(antijoin ? "=" : ">=")), INT8OID,
                       INT8OID);
  op->opno = o;
  op->opfuncid = get_opcode(o);
  op->opresulttype = BOOLOID;
  op->opcollid = InvalidOid;
  op->inputcollid = InvalidOid;
  op->args = list_make2(sl, makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                      Int64GetDatum(antijoin ? 0 : 1), false,
                                      FLOAT8PASSBYVAL));
  op->location = -1;
  return (Node *)op;
}

/** @brief Context for @c oj_param_repl_mut. */
typedef struct {
  int paramid;
  Node *replacement;
} oj_param_repl_ctx;

/** @brief Replace every @c PARAM_SUBLINK with @p paramid by @p replacement. */
static Node *oj_param_repl_mut(Node *node, void *cx) {
  oj_param_repl_ctx *c = (oj_param_repl_ctx *)cx;
  if (node == NULL)
    return NULL;
  if (IsA(node, Param)) {
    Param *p = (Param *)node;
    if (p->paramkind == PARAM_SUBLINK && p->paramid == c->paramid)
      return copyObject(c->replacement);
    return node;
  }
  return expression_tree_mutator(node, oj_param_repl_mut, cx);
}

/**
 * @brief For an @c "x IN (SELECT Q.col FROM Q [WHERE w])" (@c ANY_SUBLINK with
 *        operator @c '='), build the @c "x = Q.col" correlation conjunct.
 *
 * The testexpr is @c "x = Param(subselect output)"; the subselect's single
 * output column is @c Q.col.  We copy the testexpr, sink its outer operand(s)
 * one level (@c x lives one step up from inside the subselect), then substitute
 * @c Q.col for the @c PARAM_SUBLINK placeholder -- keeping the operator and any
 * coercions (e.g. the @c varchar->text relabel around the param) intact.
 * Returns @c NULL for anything but the single-column @c '=' form (row IN,
 * @c > @c ANY, etc.).
 */
static Node *extract_in_corr(SubLink *sl) {
  OpExpr *te;
  Param *p;
  Node *rhs, *qcol, *corr;
  oj_param_repl_ctx ctx;
  Query *sub = (Query *)sl->subselect;

  if (sl->subLinkType != ANY_SUBLINK || !IsA(sl->testexpr, OpExpr))
    return NULL;
  te = (OpExpr *)sl->testexpr;
  if (list_length(te->args) != 2 || list_length(sub->targetList) != 1)
    return NULL;
  rhs = (Node *)lsecond(te->args);
  if (IsA(rhs, RelabelType))
    rhs = (Node *)((RelabelType *)rhs)->arg; /* varchar->text etc. */
  if (!IsA(rhs, Param))
    return NULL;
  p = (Param *)rhs;
  if (p->paramkind != PARAM_SUBLINK || p->paramid != 1)
    return NULL;

  qcol = copyObject((Node *)((TargetEntry *)linitial(sub->targetList))->expr);
  corr = copyObject((Node *)te);
  IncrementVarSublevelsUp(corr, 1, 0); /* outer operands -> one level deeper */
  ctx.paramid = 1;
  ctx.replacement = qcol;
  return oj_param_repl_mut(corr, &ctx);
}

/**
 * @brief Rewrite top-level @c EXISTS / @c IN WHERE conjuncts (optionally negated)
 *        over a single tracked relation into correlated @c count(*) comparisons.
 *
 * A pre-pass for @c decorrelate_scalar_sublinks: each qualifying conjunct (a
 * bare @c EXISTS / @c IN sublink, or one wrapped in a single @c NOT -- i.e.
 * @c NOT @c EXISTS / @c NOT @c IN) is replaced by the @c build_count_predicate
 * form, after which the scalar-subquery decorrelation lowers the count()
 * comparison to the @c "R ⟕ Q" semijoin / antijoin.  Conjuncts whose subselect is
 * not decorrelatable (untracked / multi-relation / uncorrelated) are left
 * untouched, so they hit the usual unsupported-subquery error.
 */
static bool rewrite_predicate_sublinks(const constants_t *constants, Query *q) {
  Node *quals;
  List *conjs, *newconjs = NIL;
  ListCell *lc;
  bool changed = false;

  if (q->commandType != CMD_SELECT || !q->hasSubLinks || !q->jointree ||
      !q->jointree->quals)
    return false;

  quals = q->jointree->quals;
  conjs = (IsA(quals, BoolExpr) && ((BoolExpr *)quals)->boolop == AND_EXPR)
            ? ((BoolExpr *)quals)->args
            : list_make1(quals);

  foreach (lc, conjs) {
    Node *c = (Node *)lfirst(lc);
    Node *inner = c, *rewritten = NULL;
    bool neg = false;
    SubLink *sl;

    if (IsA(c, BoolExpr) && ((BoolExpr *)c)->boolop == NOT_EXPR &&
        list_length(((BoolExpr *)c)->args) == 1) {
      neg = true;
      inner = (Node *)linitial(((BoolExpr *)c)->args);
    }
    if (IsA(inner, SubLink) && IsA(((SubLink *)inner)->subselect, Query)) {
      sl = (SubLink *)inner;
      if (sl->subLinkType == EXISTS_SUBLINK &&
          predicate_subselect_decorrelatable(constants,
                                             (Query *)sl->subselect, false)) {
        /* EXISTS / NOT EXISTS: correlation already in the subselect WHERE. */
        rewritten =
          build_count_predicate((Query *)sl->subselect, NULL, neg);
      } else if (sl->subLinkType == ANY_SUBLINK) {
        /* IN / NOT IN: correlation lifted from the testexpr. */
        Node *corr = extract_in_corr(sl);
        if (corr &&
            predicate_subselect_decorrelatable(constants,
                                               (Query *)sl->subselect, true))
          rewritten =
            build_count_predicate((Query *)sl->subselect, corr, neg);
      }
    }
    newconjs = lappend(newconjs, rewritten ? rewritten : c);
    if (rewritten)
      changed = true;
  }

  if (changed)
    q->jointree->quals = (list_length(newconjs) == 1)
                           ? (Node *)linitial(newconjs)
                           : (Node *)makeBoolExpr(AND_EXPR, newconjs, -1);
  return changed;
}

/**
 * @brief Rewrite a top-level @c ARRAY(SELECT Q.col FROM Q WHERE corr) target-list
 *        entry into the aggregate body @c (SELECT array_agg(Q.col) FROM Q WHERE
 *        corr).
 *
 * A pre-pass for @c decorrelate_scalar_sublinks: an @c ARRAY_SUBLINK collects the
 * correlated rows into an array, which is exactly @c array_agg over the group, so
 * mutating it into an @c EXPR_SUBLINK aggregate body lets the aggregate arm lower
 * it to @c array_agg(Q.col) over the @c "R ⟕ Q" group -- no @c count gate, since
 * an array may have zero, one, or many elements.  Subselects that are not
 * decorrelatable (untracked / multi-relation / uncorrelated) are left untouched.
 */
static bool rewrite_array_sublinks(const constants_t *constants, Query *q) {
  ListCell *lc;
  bool changed = false;

  if (q->commandType != CMD_SELECT || !q->hasSubLinks)
    return false;

  foreach (lc, q->targetList) {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    SubLink *sl;
    Query *sub;
    TargetEntry *innerte;
    Oid elemtype, arrtype;
    oj_sublink_scan scan;
    Aggref *agg;
    NullTest *nt;

    if (!IsA(te->expr, SubLink))
      continue;
    sl = (SubLink *)te->expr;
    if (sl->subLinkType != ARRAY_SUBLINK || !IsA(sl->subselect, Query))
      continue;
    sub = (Query *)sl->subselect;
    if (!predicate_subselect_decorrelatable(constants, sub, false) ||
        list_length(sub->targetList) != 1 || sub->sortClause)
      continue; /* a sortClause fixes array element order, which array_agg over
                 * the regrouped join would not preserve -- leave it unhandled */

    innerte = (TargetEntry *)linitial(sub->targetList);
    elemtype = exprType((Node *)innerte->expr);
    arrtype = get_array_type(elemtype);
    if (!OidIsValid(arrtype))
      continue; /* no array type for this element (e.g. a pseudo-type) */

    /* A Q column from the correlation, to key the null-padded-row filter. */
    scan.n_sublinks = 0;
    scan.found_sublink = NULL;
    scan.target_varno = 1; /* Q is rtindex 1 in the subselect */
    scan.found_var = NULL;
    oj_sublink_scan_walker(sub->jointree->quals, &scan);
    if (scan.found_var == NULL)
      continue; /* uncorrelated: decorrelation would bail anyway */

    agg = oj_make_aggref(F_ARRAY_AGG_ANYNONARRAY, arrtype, elemtype,
                         innerte->expr);
    /* array_agg keeps NULLs in its value, so the LEFT JOIN's null-padded
     * antijoin row (Q key IS NULL) would inject a spurious NULL element.
     * Filter it out; a genuinely-NULL matched element (Q key non-NULL) is still
     * collected.  decorrelate's Var-remap retargets this Q key to the pulled-up
     * Q just like the aggregate argument. */
    nt = makeNode(NullTest);
    nt->arg = (Expr *)copyObject((Node *)scan.found_var);
    nt->nulltesttype = IS_NOT_NULL;
    nt->argisrow = false;
    nt->location = -1;
    agg->aggfilter = (Expr *)nt;

    innerte->expr = (Expr *)agg;
    sub->hasAggs = true;
    sl->subLinkType = EXPR_SUBLINK;
    changed = true;
  }
  return changed;
}

/**
 * @brief Collapse a multi-table scalar-subquery body FROM into one derived
 *        cross-product subquery @c D, so the decorrelation can treat the body as
 *        @c "SELECT val FROM D WHERE W" with @c D a single tracked subquery.
 *
 * Mirror of @c oj_wrap_outer_from, but for the SubLink body: every body relation
 * must be a tracked base relation and the FROM a comma-join.  @c D exposes every
 * base user column (@c oj_collect_cols); the body's own (level-0) references are
 * retargeted to @c D, while the correlated level-1 references to the outer query
 * are left untouched.  The body WHERE @c W (correlation + inter-table join) stays
 * in place: it becomes the @c "R LEFT JOIN D" ON clause, and @c get_provenance_attributes
 * later processes @c D recursively, giving it the @c Q1 ⊗ … ⊗ Qn provenance.
 */
static bool oj_wrap_body_from(const constants_t *constants, Query *sub) {
  int rtlen = list_length(sub->rtable);
  int **pos;
  Query *D = makeNode(Query);
  RangeTblEntry *d_rte;
  RangeTblRef *rtr = makeNode(RangeTblRef);
  List *d_tl = NIL;
  oj_wrap_ctx wc;
  ListCell *lc;
  int idx, posn = 0;

  if (!sub->jointree || sub->jointree->fromlist == NIL)
    return false;
  foreach (lc, sub->rtable) {
    RangeTblEntry *r = (RangeTblEntry *)lfirst(lc);
    if (r->rtekind != RTE_RELATION || !oj_rte_has_provsql(constants, r))
      return false;
  }
  foreach (lc, sub->jointree->fromlist) {
    if (!IsA(lfirst(lc), RangeTblRef))
      return false; /* only a plain comma-join, no explicit JoinExprs */
  }

  /* D exposes every base user column; record (rtindex,attno) -> D column. */
  pos = (int **)palloc0((rtlen + 1) * sizeof(int *));
  idx = 0;
  foreach (lc, sub->rtable) {
    RangeTblEntry *r = (RangeTblEntry *)lfirst(lc);
    oj_cols rc;
    int j;
    ++idx;
    oj_collect_cols(constants, r, &rc);
    pos[idx] =
      (int *)palloc0((list_length(r->eref->colnames) + 1) * sizeof(int));
    for (j = 0; j < rc.n; ++j) {
      Var *v =
        makeVar(idx, rc.attno[j], rc.type[j], rc.typmod[j], rc.coll[j], 0);
      d_tl = lappend(d_tl, makeTargetEntry((Expr *)v, ++posn,
                                           pstrdup(rc.name[j]), false));
      pos[idx][rc.attno[j]] = posn;
    }
  }

  /* D = SELECT <body base user cols> FROM <body fromlist> (cross product). */
  D->commandType = CMD_SELECT;
  D->canSetTag = true;
  D->rtable = sub->rtable;
  D->jointree = makeNode(FromExpr);
  D->jointree->fromlist = sub->jointree->fromlist;
  D->jointree->quals = NULL;
  D->targetList = d_tl;
#if PG_VERSION_NUM >= 160000
  D->rteperminfos = sub->rteperminfos;
#endif

  /* Retarget the body's own (level-0) Vars to D; level-1 (outer) Vars stay. */
  wc.newidx = 1;
  wc.rtlen = rtlen;
  wc.pos = pos;
  wc.target_level = 0;
  wc.skip = NULL;
  sub->targetList = (List *)oj_wrap_remap_mut((Node *)sub->targetList, &wc);
  if (sub->jointree->quals)
    sub->jointree->quals = oj_wrap_remap_mut(sub->jointree->quals, &wc);

  /* Rebuild the body over D. */
  d_rte = oj_make_subquery_rte(D);
  sub->rtable = list_make1(d_rte);
#if PG_VERSION_NUM >= 160000
  sub->rteperminfos = NIL;
#endif
  rtr->rtindex = 1;
  sub->jointree->fromlist = list_make1(rtr);
  return true;
}

/**
 * @brief Build the derived single-row aggregate @c D for an UNcorrelated scalar
 *        subquery body, to be cross-joined into the outer FROM.
 *
 * Aggregate body @c "SELECT agg(..) FROM Q [WHERE]" -> @c D is the body itself
 * (always one row).  Value body @c "SELECT val FROM Q [WHERE]" -> @c D is
 * @c "SELECT choose(val) FROM Q [WHERE] HAVING count(*) <= 1" -- one row, with
 * the scalar subquery's at-most-one-row rule baked into the moved subquery.
 * Returns @c NULL unless the body is an uncorrelated clean SELECT over tracked
 * base relations (a comma-join is fine; @c D is then an inner join).
 *
 * Faithful to ProvSQL aggregates: an empty @c Q yields an empty group, hence a
 * @c gate_zero row that drops out -- exactly what a hand-written derived
 * aggregate does; the correlated path's 0-match NULL row is not reconstructed.
 */
static Query *oj_build_uncorrelated_from_subquery(const constants_t *constants,
                                                  Query *body) {
  Query *D;
  TargetEntry *vte;
  ListCell *lc;

  if (!IsA(body, Query) || body->commandType != CMD_SELECT)
    return NULL;
  if (body->groupClause || body->groupingSets || body->distinctClause ||
      body->setOperations || body->hasWindowFuncs || body->hasSubLinks ||
      body->limitCount || body->limitOffset || body->cteList ||
      list_length(body->targetList) != 1)
    return NULL;
  if (!body->jointree || body->jointree->fromlist == NIL)
    return NULL;
  /* Correlated bodies are the LEFT-JOIN decorrelation's job, not this one. */
  if (contain_vars_of_level((Node *)body->targetList, 1) ||
      (body->jointree->quals && contain_vars_of_level(body->jointree->quals, 1)))
    return NULL;
  /* Every FROM relation must be a tracked base relation, so D is a processable
   * tracked subquery (a comma-join is fine -- D is then an inner join). */
  foreach (lc, body->rtable) {
    RangeTblEntry *r = (RangeTblEntry *)lfirst(lc);
    if (r->rtekind != RTE_RELATION || !oj_rte_has_provsql(constants, r))
      return NULL;
  }
  foreach (lc, body->jointree->fromlist) {
    if (!IsA(lfirst(lc), RangeTblRef))
      return NULL;
  }

  vte = (TargetEntry *)linitial(body->targetList);

  if (body->hasAggs) {
    /* Aggregate body: a single bare aggregate (one row, no grouping). */
    if (!IsA(vte->expr, Aggref))
      return NULL;
    D = (Query *)copyObject(body);
  } else {
    /* Value body: pick the single value with choose(), gate the >1-row worlds
     * with HAVING count(*) <= 1. */
    Aggref *cnt;
    OpExpr *le;
    Oid le_op;

    if (!OidIsValid(constants->OID_FUNCTION_CHOOSE))
      return NULL;
    D = (Query *)copyObject(body);
    vte = (TargetEntry *)linitial(D->targetList);
    vte->expr = (Expr *)oj_make_aggref(constants->OID_FUNCTION_CHOOSE,
                                       exprType((Node *)vte->expr),
                                       exprType((Node *)vte->expr), vte->expr);
    D->hasAggs = true;

    cnt = makeNode(Aggref);
    cnt->aggfnoid = F_COUNT_; /* count(*) */
    cnt->aggtype = INT8OID;
    cnt->aggtranstype = InvalidOid;
    cnt->aggargtypes = NIL;
    cnt->args = NIL;
    cnt->aggstar = true;
    cnt->aggkind = AGGKIND_NORMAL;
    cnt->aggsplit = AGGSPLIT_SIMPLE;
    cnt->location = -1;
#if PG_VERSION_NUM >= 140000
    cnt->aggno = cnt->aggtransno = -1;
#endif
    le = makeNode(OpExpr);
    le_op = OpernameGetOprid(list_make1(makeString("<=")), INT8OID, INT8OID);
    le->opno = le_op;
    le->opfuncid = get_opcode(le_op);
    le->opresulttype = BOOLOID;
    le->opcollid = InvalidOid;
    le->inputcollid = InvalidOid;
    le->args = list_make2(cnt, makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
                                         Int64GetDatum(1), false,
                                         FLOAT8PASSBYVAL));
    le->location = -1;
    D->havingQual = (Node *)le;
  }
  return D;
}

/**
 * @brief Move uncorrelated scalar subqueries that are direct target-list entries
 *        into a cross-joined derived aggregate in the outer FROM.
 *
 * An uncorrelated @c (SELECT agg/val FROM Q …) is a single constant value: it
 * becomes a one-row derived table @c D (see @c oj_build_uncorrelated_from_subquery)
 * appended to the FROM as a cross-join, and the target entry is replaced by a Var
 * to @c D's column.  Restricted to a direct target-list entry so the (aggregate)
 * @c agg_token flows straight to the output column: nesting it inside arithmetic
 * would coerce the @c agg_token to a scalar and silently drop its provenance.
 * Runs before @c decorrelate_scalar_sublinks; correlated sublinks (and ones in
 * other positions) are left untouched for the remaining paths.
 */
static bool move_uncorrelated_sublinks_to_from(const constants_t *constants,
                                               Query *q) {
  ListCell *lc;
  bool changed = false;

  if (q->commandType != CMD_SELECT || !q->hasSubLinks || !q->jointree)
    return false;

  foreach (lc, q->targetList) {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    SubLink *sl;
    Query *D;
    RangeTblEntry *d_rte;
    RangeTblRef *rtr;
    TargetEntry *dte;
    Index d_idx;

    if (!IsA(te->expr, SubLink))
      continue;
    sl = (SubLink *)te->expr;
    if (sl->subLinkType != EXPR_SUBLINK || !IsA(sl->subselect, Query))
      continue;
    D = oj_build_uncorrelated_from_subquery(constants, (Query *)sl->subselect);
    if (D == NULL)
      continue;

    d_rte = oj_make_subquery_rte(D);
    rtr = makeNode(RangeTblRef);
    dte = (TargetEntry *)linitial(D->targetList);
    q->rtable = lappend(q->rtable, d_rte);
    d_idx = list_length(q->rtable);
    rtr->rtindex = d_idx;
    q->jointree->fromlist = lappend(q->jointree->fromlist, rtr);
    te->expr = (Expr *)makeVar(d_idx, 1, exprType((Node *)dte->expr),
                               exprTypmod((Node *)dte->expr),
                               exprCollation((Node *)dte->expr), 0);
    changed = true;
  }

  if (changed) {
    oj_sublink_scan scan;
    /* Some correlated sublinks may remain; clear hasSubLinks only if none do. */
    scan.n_sublinks = 0;
    scan.found_sublink = NULL;
    scan.target_varno = 0;
    scan.found_var = NULL;
    oj_sublink_scan_walker((Node *)q->targetList, &scan);
    if (q->jointree->quals)
      oj_sublink_scan_walker(q->jointree->quals, &scan);
    if (scan.n_sublinks == 0)
      q->hasSubLinks = false;
  }
  return changed;
}

/**
 * @brief Decorrelate a single top-level scalar subquery into a LEFT JOIN.
 *
 * Restricted (v1) to: a @c CMD_SELECT whose FROM is a single tracked base
 * relation R, with exactly one SubLink in the whole query, that SubLink being
 * an @c EXPR_SUBLINK that is the direct expression of a target-list entry,
 * whose body is @c "SELECT val FROM Q [WHERE corr]" over a single base relation
 * Q referencing only Q (level 0) and R (level 1).  Returns @c true if the
 * query was rewritten in place.
 */
static bool decorrelate_scalar_sublinks(const constants_t *constants,
                                        Query *q) {
  RangeTblRef *r_ref;
  Index R_idx, Q_idx, join_idx;
  RangeTblEntry *R_rte, *Q_rte_orig, *Q_copy, *jrte;
  Query *sub;
  SubLink *sl = NULL;
  TargetEntry *sl_te = NULL;
  Expr *valexpr;
  Node *theta;
  oj_cols Rc, Qc;
  oj_decorr_ctx dctx;
  oj_sublink_scan scan;
  ListCell *lc;
  int i, n_tl_sublinks = 0;
  bool in_where = false;
  bool is_agg_body = false;
  Expr *repl_expr; /* what replaces the SubLink: choose(val) or the aggregate */

  if (!OidIsValid(constants->OID_FUNCTION_CHOOSE))
    return false;
  if (q->commandType != CMD_SELECT || !q->hasSubLinks)
    return false;
  if (q->groupClause || q->groupingSets || q->hasAggs || q->distinctClause ||
      q->setOperations || q->havingQual || q->hasWindowFuncs)
    return false;
  if (!q->jointree || q->jointree->fromlist == NIL)
    return false;

  /* Exactly one SubLink in the whole query.  It is either the direct expr of a
   * target-list entry (its value flows to choose()), or it sits inside a WHERE
   * conjunct (a comparison that will be lifted to HAVING on choose()). */
  scan.n_sublinks = 0;
  scan.found_sublink = NULL;
  scan.target_varno = 0;
  scan.found_var = NULL;
  oj_sublink_scan_walker((Node *)q->targetList, &scan);
  n_tl_sublinks = scan.n_sublinks;
  if (q->jointree->quals)
    oj_sublink_scan_walker(q->jointree->quals, &scan);
  if (scan.n_sublinks != 1)
    return false;
  sl = scan.found_sublink;
  if (sl == NULL || sl->subLinkType != EXPR_SUBLINK ||
      !IsA(sl->subselect, Query))
    return false;

  /* Is it a direct target-list entry?  Otherwise it must be in WHERE; a
   * SubLink nested inside a target-list expression (arithmetic, comparison)
   * is not supported. */
  foreach (lc, q->targetList) {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    if (te->expr == (Expr *)sl) {
      sl_te = te;
      break;
    }
  }
  if (sl_te == NULL) {
    if (n_tl_sublinks > 0)
      return false; /* nested in a target-list expression */
    in_where = true;
  }

  /* The body must be SELECT val FROM Q [WHERE corr], Q a single base rel, where
   * val is either a plain value (decorrelated with choose() + count(...)<=1) or
   * a single bare aggregate (decorrelated to that aggregate over the LEFT-JOIN
   * group, no count gate).  LIMIT / OFFSET would pick a bounded, order-dependent
   * subset and a CTE would be dropped; reject those. */
  sub = (Query *)sl->subselect;
  if (sub->commandType != CMD_SELECT || sub->groupClause ||
      sub->groupingSets || sub->distinctClause || sub->setOperations ||
      sub->hasWindowFuncs || sub->hasSubLinks || sub->limitCount ||
      sub->limitOffset || sub->cteList || list_length(sub->targetList) != 1)
    return false;
  if (sub->hasAggs &&
      !IsA(((TargetEntry *)linitial(sub->targetList))->expr, Aggref))
    return false; /* aggregate body must be a single bare aggregate */
  is_agg_body = sub->hasAggs;
  if (!sub->jointree || sub->jointree->fromlist == NIL)
    return false;
  /* A multi-table body (Q1, Q2, … in FROM) is collapsed into a single derived
   * cross-product subquery D, after which the body is "SELECT val FROM D WHERE
   * W" -- the single-Q path below handles D exactly as it handles a subquery R. */
  if (list_length(sub->rtable) != 1 && !oj_wrap_body_from(constants, sub))
    return false;
  if (list_length(sub->jointree->fromlist) != 1 ||
      !IsA(linitial(sub->jointree->fromlist), RangeTblRef))
    return false;
  Q_rte_orig = list_nth_node(RangeTblEntry, sub->rtable, 0);
  if ((Q_rte_orig->rtekind != RTE_RELATION &&
       !(Q_rte_orig->rtekind == RTE_SUBQUERY && !Q_rte_orig->lateral)) ||
      !oj_rte_has_provsql(constants, Q_rte_orig))
    return false;

  /* Determine R.  If the outer FROM is already a single tracked relation or
   * (non-lateral) subquery, use it directly; otherwise wrap the whole FROM into
   * a derived subquery R' (the subquery-arm lowering then handles R' LEFT JOIN
   * Q either way). */
  R_rte = NULL;
  if (list_length(q->jointree->fromlist) == 1 &&
      IsA(linitial(q->jointree->fromlist), RangeTblRef)) {
    r_ref = (RangeTblRef *)linitial(q->jointree->fromlist);
    R_rte = list_nth_node(RangeTblEntry, q->rtable, r_ref->rtindex - 1);
    if ((R_rte->rtekind == RTE_RELATION ||
         (R_rte->rtekind == RTE_SUBQUERY && !R_rte->lateral)) &&
        oj_rte_has_provsql(constants, R_rte))
      R_idx = r_ref->rtindex;
    else
      R_rte = NULL;
  }
  if (R_rte == NULL) {
    if (!oj_wrap_outer_from(constants, q, sl, in_where))
      return false;
    R_idx = 1;
    R_rte = list_nth_node(RangeTblEntry, q->rtable, 0);
    sub = (Query *)sl->subselect; /* remapped in place by the wrap */
    /* The wrap rebuilt the target list (the SubLink node itself is preserved),
     * so re-find the target entry that still carries the SubLink. */
    sl_te = NULL;
    foreach (lc, q->targetList) {
      TargetEntry *te = (TargetEntry *)lfirst(lc);
      if (te->expr == (Expr *)sl) {
        sl_te = te;
        break;
      }
    }
  }

  /* The correlation must reference some Q column (so count() has a key that is
   * NULL on the null-padded antijoin rows). */
  scan.n_sublinks = 0;
  scan.target_varno = 1; /* Q is at index 1 inside the body */
  scan.found_var = NULL;
  if (sub->jointree->quals)
    oj_sublink_scan_walker(sub->jointree->quals, &scan);
  if (scan.found_var == NULL)
    return false;

  /* ---- Commit: pull Q up, build the LEFT JOIN, choose() + GROUP BY + count.
   * R stays at R_idx, Q is appended (Q_idx), join RTE appended (join_idx). ---*/
  oj_collect_cols(constants, R_rte, &Rc);
  oj_collect_cols(constants, Q_rte_orig, &Qc);

  Q_copy = copyObject(Q_rte_orig);
#if PG_VERSION_NUM >= 160000
  if (Q_rte_orig->perminfoindex != 0) {
    RTEPermissionInfo *pi =
      getRTEPermissionInfo(sub->rteperminfos, Q_rte_orig);
    q->rteperminfos = lappend(q->rteperminfos, copyObject(pi));
    Q_copy->perminfoindex = list_length(q->rteperminfos);
  }
#endif
  q->rtable = lappend(q->rtable, Q_copy);
  Q_idx = list_length(q->rtable);

  /* Move the body's Vars to the outer level: Q(level0,1) -> (level0, Q_idx);
   * correlated R(level1) -> (level0). */
  dctx.q_old = 1;
  dctx.q_new = Q_idx;
  theta = oj_decorr_var_mut(copyObject(sub->jointree->quals), &dctx);
  valexpr = (Expr *)oj_decorr_var_mut(
    copyObject((Node *)((TargetEntry *)linitial(sub->targetList))->expr),
    &dctx);

  /* Build the synthetic join RTE (eref / joinaliasvars / left/right cols), the
   * same bookkeeping the deparser needs as in oj_build_join_query. */
  {
    List *av = NIL, *lcols = NIL, *rcols = NIL, *cn = NIL;
    jrte = makeNode(RangeTblEntry);
    for (i = 0; i < Rc.n; ++i) {
      av = lappend(av, makeVar(R_idx, Rc.attno[i], Rc.type[i], Rc.typmod[i],
                               Rc.coll[i], 0));
      lcols = lappend_int(lcols, Rc.attno[i]);
      rcols = lappend_int(rcols, 0);
      cn = lappend(cn, makeString(pstrdup(Rc.name[i])));
    }
    for (i = 0; i < Qc.n; ++i) {
      av = lappend(av, makeVar(Q_idx, Qc.attno[i], Qc.type[i], Qc.typmod[i],
                               Qc.coll[i], 0));
      lcols = lappend_int(lcols, 0);
      rcols = lappend_int(rcols, Qc.attno[i]);
      cn = lappend(cn, makeString(pstrdup(Qc.name[i])));
    }
    jrte->rtekind = RTE_JOIN;
    jrte->jointype = JOIN_LEFT;
    jrte->alias = NULL;
    jrte->eref = makeAlias("unnamed_join", cn);
    jrte->joinaliasvars = av;
#if PG_VERSION_NUM >= 130000
    jrte->joinleftcols = lcols;
    jrte->joinrightcols = rcols;
    jrte->joinmergedcols = 0;
#endif
    jrte->inFromCl = true;
    q->rtable = lappend(q->rtable, jrte);
    join_idx = list_length(q->rtable);
  }

  {
    JoinExpr *je = makeNode(JoinExpr);
    RangeTblRef *lr = makeNode(RangeTblRef), *rr = makeNode(RangeTblRef);
    lr->rtindex = R_idx;
    rr->rtindex = Q_idx;
    je->jointype = JOIN_LEFT;
    je->larg = (Node *)lr;
    je->rarg = (Node *)rr;
    je->quals = theta;
    je->isNatural = false;
    je->usingClause = NIL;
    je->rtindex = join_idx;
    q->jointree->fromlist = list_make1(je);
    /* Any pre-existing outer WHERE (over R, level 0) stays in jointree->quals.*/
  }

  /* What replaces the SubLink, over the LEFT-JOIN group:
   *  - value body  -> choose(val) picks the single matched value;
   *  - aggregate body -> the aggregate itself.  count(*) is rewritten to
   *    count(Q.key) so the null-padded antijoin row (Q.key IS NULL) is not
   *    counted -- an empty correlated group must give 0, not 1.
   * For a target-list SubLink it replaces the entry directly; for a WHERE
   * SubLink it is substituted into the conjunct, which moves to HAVING. */
  if (is_agg_body) {
    Aggref *agg = (Aggref *)valexpr; /* the remapped body aggregate */
    if (agg->aggstar) {
      Var *qkey = (Var *)copyObject(scan.found_var);
      qkey->varno = Q_idx;
#if PG_VERSION_NUM >= 130000
      qkey->varnosyn = 0;
      qkey->varattnosyn = 0;
#endif
      repl_expr = (Expr *)oj_make_aggref(F_COUNT_ANY, INT8OID, qkey->vartype,
                                         (Expr *)qkey);
    } else {
      repl_expr = valexpr;
    }
  } else {
    repl_expr = (Expr *)oj_make_aggref(constants->OID_FUNCTION_CHOOSE,
                                       exprType((Node *)valexpr),
                                       exprType((Node *)valexpr), valexpr);
  }
  if (!in_where)
    sl_te->expr = repl_expr;

  /* GROUP BY every R user column: each needs a target-list entry carrying a
   * ressortgroupref plus a SortGroupClause. */
  {
    int sgref = 0;
    /* Highest existing ressortgroupref, so new ones do not collide. */
    foreach (lc, q->targetList) {
      TargetEntry *te = (TargetEntry *)lfirst(lc);
      if (te->ressortgroupref > sgref)
        sgref = te->ressortgroupref;
    }
    for (i = 0; i < Rc.n; ++i) {
      TargetEntry *gte = NULL;
      SortGroupClause *sgc;
      ListCell *lc2;

      /* Reuse an existing target entry that already projects this R column. */
      foreach (lc2, q->targetList) {
        TargetEntry *te = (TargetEntry *)lfirst(lc2);
        if (IsA(te->expr, Var)) {
          Var *v = (Var *)te->expr;
          if (v->varlevelsup == 0 && v->varno == R_idx &&
              v->varattno == Rc.attno[i]) {
            gte = te;
            break;
          }
        }
      }
      if (gte == NULL) {
        Var *v = makeVar(R_idx, Rc.attno[i], Rc.type[i], Rc.typmod[i],
                         Rc.coll[i], 0);
        gte = makeTargetEntry((Expr *)v, list_length(q->targetList) + 1,
                              pstrdup(Rc.name[i]), true /* resjunk */);
        q->targetList = lappend(q->targetList, gte);
      }
      if (gte->ressortgroupref == 0)
        gte->ressortgroupref = ++sgref;
      sgc = makeNode(SortGroupClause);
      sgc->tleSortGroupRef = gte->ressortgroupref;
      get_sort_group_operators(Rc.type[i], false, true, false, &sgc->sortop,
                               &sgc->eqop, NULL, &sgc->hashable);
      q->groupClause = lappend(q->groupClause, sgc);
    }
  }

  /* HAVING.  A value body adds count(Q.key) <= 1 (Q.key NULL on the null-padded
   * antijoin rows), enforcing the scalar subquery's at-most-one-row rule; an
   * aggregate body needs no such gate.  When the SubLink came from a WHERE
   * comparison, that conjunct (with the SubLink replaced) is ANDed in -- a
   * comparison on the aggregated value belongs in HAVING. */
  {
    List *having_conjuncts = NIL;

    if (!is_agg_body) {
      having_conjuncts =
        list_make1(oj_count_cmp((Var *)scan.found_var, Q_idx, "<=", 1));
      /* A WHERE comparison must test an actual subquery value, so the correlated
       * group has to be non-empty: count(Q.key) = 1, not merely <= 1.  An empty
       * group would give a NULL comparison (the row is excluded), but the value
       * gate over the all-NULL aggregate does not encode that, so the >= 1 gate
       * supplies it.  (A target-list subquery keeps <= 1 only: zero matches is a
       * legal NULL value, and the row still exists.) */
      if (in_where)
        having_conjuncts = lappend(
          having_conjuncts, oj_count_cmp((Var *)scan.found_var, Q_idx, ">=", 1));
    }

    if (in_where) {
      /* Split the WHERE AND-list: the conjunct holding the SubLink (with the
       * SubLink -> repl_expr substitution) moves to HAVING; the rest stay. */
      oj_sl_replace_ctx rc;
      Node *quals = q->jointree->quals;
      List *conjs =
        (quals && IsA(quals, BoolExpr) &&
         ((BoolExpr *)quals)->boolop == AND_EXPR)
          ? ((BoolExpr *)quals)->args
          : (quals ? list_make1(quals) : NIL);
      List *kept = NIL;
      ListCell *lc2;

      rc.target = sl;
      rc.replacement = (Node *)repl_expr;
      foreach (lc2, conjs) {
        Node *c = (Node *)lfirst(lc2);
        if (oj_contains_sublink_walker(c, sl))
          having_conjuncts =
            lappend(having_conjuncts, oj_sl_replace_mut(c, &rc));
        else
          kept = lappend(kept, c);
      }
      q->jointree->quals =
        (kept == NIL) ? NULL
                      : (list_length(kept) == 1 ? (Node *)linitial(kept)
                                                : (Node *)makeBoolExpr(
                                                    AND_EXPR, kept, -1));
    }

    q->havingQual =
      (having_conjuncts == NIL)
        ? NULL
        : (list_length(having_conjuncts) == 1
             ? (Node *)linitial(having_conjuncts)
             : (Node *)makeBoolExpr(AND_EXPR, having_conjuncts, -1));
  }

  q->hasAggs = true;
  q->hasSubLinks = false;
  return true;
}

/**
 * @brief Group the right-hand arm of a set difference by all its columns so
 *        the per-tuple right provenances ⊕-combine before the monus.
 *
 * ProvSQL's multiset difference implements the NOT-IN semantics of the ICDE
 * 2026 paper (§IV-B):
 * @code
 *   ⟪q₁ − q₂⟫ = {{ (u, α ⊖ ⊕_{β : (u,β)∈q₂} β)  |  (u,α) ∈ q₁ }}
 * @endcode
 * The sum @c ⊕β ranges over ALL right tuples equal to @c u, so the right arm
 * must be grouped by its columns first.  Without that,
 * @c transform_except_into_join's bare @c LEFT @c JOIN emits one monus per
 * matching right tuple (yielding @c ⊕(α⊖βᵢ) instead of @c α⊖⊕β) and inflates
 * the result multiplicity -- the long-standing "add group by in the right-side
 * table" gap.  Wrapping the still-raw right arm in
 * @code
 *   SELECT cols FROM (rarg) GROUP BY cols
 * @endcode
 * makes the later @c get_provenance_attributes / group-by pass build @c ⊕β per
 * group and gives the right arm exactly one row per distinct @c u.
 *
 * Runs before provenance discovery, on the @c SETOP_EXCEPT query (for the
 * non-ALL case, on the @c all=true inner set operation that
 * @c rewrite_non_all_into_external_group_by leaves behind).  It applies equally
 * to @c EXCEPT (@c ε(q₁−q₂)) and @c EXCEPT @c ALL (@c q₁−q₂): the only
 * difference between them, duplicate elimination of the left arm, is handled
 * separately by the non-ALL outer GROUP BY.
 */
static void group_set_difference_right_arm(const constants_t *constants,
                                           Query *q) {
  SetOperationStmt *so;
  RangeTblRef *rarg_ref;
  RangeTblEntry *rarg_rte;
  Query *origB, *G;
  RangeTblEntry *w_rte;
  RangeTblRef *rtr;
  FromExpr *fe;
  List *tl = NIL;
  ListCell *lc;
  int colno = 0, sgref = 0;
  bool any_group = false;

  (void)constants;

  if (q->setOperations == NULL || !IsA(q->setOperations, SetOperationStmt))
    return;
  so = (SetOperationStmt *)q->setOperations;
  if (so->op != SETOP_EXCEPT)
    return;
  /* Chained difference (rarg is itself a SetOperationStmt) is rejected later
   * by transform_except_into_join; leave it untouched here. */
  if (!IsA(so->rarg, RangeTblRef))
    return;
  rarg_ref = (RangeTblRef *)so->rarg;
  rarg_rte = list_nth_node(RangeTblEntry, q->rtable, rarg_ref->rtindex - 1);
  if (rarg_rte->rtekind != RTE_SUBQUERY || rarg_rte->subquery == NULL)
    return;
  origB = rarg_rte->subquery;

  /* Already a single-row-per-group shape?  (Our own outer-join antijoin builds
   * the right arm pre-grouped.)  Re-grouping is harmless but pointless, so skip
   * when the arm already carries a groupClause. */
  if (origB->groupClause != NIL || origB->groupingSets != NIL)
    return;

  G = makeNode(Query);
  G->commandType = CMD_SELECT;
  G->canSetTag = true;
  w_rte = oj_make_subquery_rte(origB);
  G->rtable = list_make1(w_rte);
  rtr = makeNode(RangeTblRef);
  rtr->rtindex = 1;
  fe = makeNode(FromExpr);
  fe->fromlist = list_make1(rtr);
  G->jointree = fe;

  colno = 0;
  foreach (lc, origB->targetList) {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    Var *v;
    TargetEntry *nte;
    SortGroupClause *sgc;
    Oid coltype;

    ++colno;
    if (te->resjunk)
      continue;

    coltype = exprType((Node *)te->expr);
    v = makeVar(1, colno, coltype, exprTypmod((Node *)te->expr),
                exprCollation((Node *)te->expr), 0);
    nte = makeTargetEntry((Expr *)v, list_length(tl) + 1,
                          te->resname ? pstrdup(te->resname) : NULL, false);

    /* Group by every column (a UUID provsql column would already have been
     * rejected upstream; raw arms expose only value columns here). */
    sgc = makeNode(SortGroupClause);
    sgc->tleSortGroupRef = nte->ressortgroupref = ++sgref;
    get_sort_group_operators(coltype, false, true, false, &sgc->sortop,
                             &sgc->eqop, NULL, &sgc->hashable);
    G->groupClause = lappend(G->groupClause, sgc);
    any_group = true;

    tl = lappend(tl, nte);
  }
  G->targetList = tl;

  if (!any_group)
    return; /* nothing to group on; leave the arm unchanged */

  rarg_rte->subquery = G;
}

/**
 * @brief Recursively annotate a UNION tree with the provenance UUID type.
 *
 * Walks the @c SetOperationStmt tree of a UNION and appends the UUID type
 * to @c colTypes / @c colTypmods / @c colCollations on every node, and sets
 * @c all = true so that PostgreSQL does not deduplicate the combined stream.
 * The non-ALL deduplication has already been moved to an outer GROUP BY by
 * @c rewrite_non_all_into_external_group_by before this is called.
 *
 * @param constants  Extension OID cache.
 * @param stmt       Root (or subtree) of the UNION @c SetOperationStmt.
 * @param q          Outer query (to look up subquery RTEs for agg_token type updates).
 */
static void process_set_operation_union(const constants_t *constants,
                                        SetOperationStmt *stmt,
                                        Query *q) {
  if (stmt->op != SETOP_UNION) {
    provsql_error("Unsupported mixed set operations");
  }
  if (IsA(stmt->larg, SetOperationStmt)) {
    process_set_operation_union(constants, (SetOperationStmt *)(stmt->larg), q);
  }
  if (IsA(stmt->rarg, SetOperationStmt)) {
    process_set_operation_union(constants, (SetOperationStmt *)(stmt->rarg), q);
  }

  /* Update colTypes for columns that became agg_token after rewriting.
   * Use the left branch's subquery to detect agg_token columns. */
  if (IsA(stmt->larg, RangeTblRef)) {
    Index rtindex = ((RangeTblRef *)stmt->larg)->rtindex;
    RangeTblEntry *rte = list_nth_node(RangeTblEntry, q->rtable, rtindex - 1);
    if (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL) {
      ListCell *lc_type = list_head(stmt->colTypes);
      ListCell *lc_te = list_head(rte->subquery->targetList);
      while (lc_type != NULL && lc_te != NULL) {
        TargetEntry *te = (TargetEntry *)lfirst(lc_te);
        if (exprType((Node *)te->expr) == constants->OID_TYPE_AGG_TOKEN) {
          lfirst_oid(lc_type) = constants->OID_TYPE_AGG_TOKEN;
        }
        lc_type = my_lnext(stmt->colTypes, lc_type);
        lc_te = my_lnext(rte->subquery->targetList, lc_te);
      }
    }
  }

  stmt->colTypes = lappend_oid(stmt->colTypes, constants->OID_TYPE_UUID);
  stmt->colTypmods = lappend_int(stmt->colTypmods, -1);
  stmt->colCollations = lappend_int(stmt->colCollations, 0);
  stmt->all = true;
}

/**
 * @brief Add a WHERE condition filtering out zero-provenance tuples.
 *
 * For EXCEPT queries, tuples whose provenance evaluates to zero (i.e., the
 * right-hand side fully subsumes the left-hand side) must be excluded from
 * the result.  This function appends @c provsql <> gate_zero() to
 * @p q->jointree->quals, ANDing with any existing WHERE condition.
 *
 * @param constants  Extension OID cache.
 * @param q          Query to modify in place.
 * @param provsql    Provenance expression that was added to the SELECT list.
 */
static void add_select_non_zero(const constants_t *constants, Query *q,
                                Expr *provsql) {
  FuncExpr *gate_zero = makeNode(FuncExpr);
  OpExpr *oe = makeNode(OpExpr);

  gate_zero->funcid = constants->OID_FUNCTION_GATE_ZERO;
  gate_zero->funcresulttype = constants->OID_TYPE_UUID;

  oe->opno = constants->OID_OPERATOR_NOT_EQUAL_UUID;
  oe->opfuncid = constants->OID_FUNCTION_NOT_EQUAL_UUID;
  oe->opresulttype = BOOLOID;
  oe->args = list_make2(provsql, gate_zero);
  oe->location = -1;

  if (q->jointree->quals != NULL) {
    BoolExpr *be = makeNode(BoolExpr);

    be->boolop = AND_EXPR;
    be->args = list_make2(oe, q->jointree->quals);
    be->location = -1;

    q->jointree->quals = (Node *)be;
  } else
    q->jointree->quals = (Node *)oe;
}

/**
 * @brief Append @p expr to @p havingQual with an AND, creating one if needed.
 *
 * If @p havingQual is NULL, returns @p expr directly.  If it is already an
 * AND @c BoolExpr, appends to its argument list.  Otherwise wraps both in a
 * new AND node.
 *
 * @param havingQual  Existing HAVING qualifier, or NULL.
 * @param expr        Expression to conjoin.
 * @return  The updated HAVING qualifier.
 */
static Node *add_to_havingQual(Node *havingQual, Expr *expr)
{
  if(!havingQual) {
    havingQual = (Node*) expr;
  } else if(IsA(havingQual, BoolExpr) && ((BoolExpr*)havingQual)->boolop==AND_EXPR) {
    BoolExpr *be = (BoolExpr*)havingQual;
    be->args = lappend(be->args, expr);
  } else if(IsA(havingQual, OpExpr) || IsA(havingQual, BoolExpr)) {
    /* BoolExpr that is not an AND (OR/NOT): wrap with a new AND node. */
    BoolExpr *be = makeNode(BoolExpr);
    be->boolop=AND_EXPR;
    be->location=-1;
    be->args = list_make2(havingQual, expr);
    havingQual = (Node*) be;
  } else
    provsql_error("Unknown structure within Boolean expression");

  return havingQual;
}

/**
 * @brief Check whether @p op is a supported comparison on an aggregate result.
 *
 * Returns true iff @p op is a two-argument operator where at least one
 * argument is a @c Var of type @c agg_token (or an implicit-cast wrapper
 * thereof) and the other is a @c Const (possibly cast).  This is the set
 * of WHERE-on-aggregate patterns that ProvSQL can safely move to a HAVING
 * clause.
 *
 * @param op         The @c OpExpr to inspect.
 * @param constants  Extension OID cache.
 * @return  True if the pattern is supported, false otherwise.
 */
static bool check_selection_on_aggregate(OpExpr *op, const constants_t *constants)
{
  bool ok=true;
  bool found_agg_token=false;

  if(op->args->length != 2)
    return false;

  for(unsigned i=0; i<2; ++i) {
    Node *arg = lfirst(list_nth_cell(op->args, i));

    // Check both arguments are either an aggtoken or a constant
    // (possibly after a cast)
    if((IsA(arg, Var) && ((Var*)arg)->vartype==constants->OID_TYPE_AGG_TOKEN)) {
      found_agg_token=true;
    } else if(IsA(arg, Const)) {
    } else if(IsA(arg, FuncExpr)) {
      FuncExpr *fe = (FuncExpr*) arg;
      if(fe->funcformat != COERCE_IMPLICIT_CAST && fe->funcformat != COERCE_EXPLICIT_CAST) {
        ok=false;
        break;
      }
      if(fe->args->length != 1) {
        ok=false;
        break;
      }
      if(!IsA(lfirst(list_head(fe->args)), Const)) {
        ok=false;
        break;
      }
    } else {
      ok=false;
      break;
    }
  }

  return ok && found_agg_token;
}

/**
 * @brief Check whether every leaf of a Boolean expression is a supported
 *        comparison on an aggregate result.
 *
 * Recursively validates @c OpExpr leaves via @c check_selection_on_aggregate
 * and descends into nested @c BoolExpr nodes.
 *
 * @param be         The Boolean expression to validate.
 * @param constants  Extension OID cache.
 * @return  True if all leaves are supported, false if any is not.
 */
static bool check_boolexpr_on_aggregate(BoolExpr *be, const constants_t *constants)
{
  ListCell *lc;

  foreach (lc, be->args) {
    Node *n=lfirst(lc);
    if(IsA(n, OpExpr)) {
      if(!check_selection_on_aggregate((OpExpr*) n, constants))
        return false;
    } else if(IsA(n, BoolExpr)) {
      if(!check_boolexpr_on_aggregate((BoolExpr*) n, constants))
        return false;
    } else
      return false;
  }

  return true;
}

/**
 * @brief Top-level dispatcher for supported WHERE-on-aggregate patterns.
 *
 * @param expr       Expression to validate (@c OpExpr or @c BoolExpr).
 * @param constants  Extension OID cache.
 * @return  True if ProvSQL can handle this expression.
 */
static bool check_expr_on_aggregate(Expr *expr, const constants_t *constants) {
  switch(expr->type) {
  case T_BoolExpr:
    return check_boolexpr_on_aggregate((BoolExpr*) expr, constants);
  case T_OpExpr:
    return check_selection_on_aggregate((OpExpr*) expr, constants);
  default:
    provsql_error("Unknown structure within Boolean expression");
  }
}

/* -------------------------------------------------------------------------
 * Main query transformation
 * ------------------------------------------------------------------------- */

/**
 * @brief Build the per-RTE column-numbering map used by where-provenance.
 *
 * Assigns a sequential position (1, 2, 3, …) to every non-provenance,
 * non-join, non-empty column across all RTEs in @p q->rtable.  The
 * @c provsql column is assigned -1 so callers can detect provenance-tracked
 * RTEs.  Join-RTE columns and empty-named columns (used for anonymous GROUP
 * BY keys) are assigned 0.
 *
 * @note For @c RTE_RELATION entries that are provenance-tracked, the
 *       sequential numbers produced here must @b not be used as PROJECT gate
 *       positions.  Because numbering is query-order-dependent, the sequential
 *       number for a column of a provenance table that is not the first RTE
 *       will exceed @c nb_columns of that table's IN gate, causing
 *       @c WhereCircuit::evaluate() to return an empty locator set.  Instead,
 *       callers should use @c varattno directly (see
 *       @c make_provenance_expression()).  The -1 sentinel is the reliable
 *       way to identify a provenance-tracked RTE.
 *
 * @param q         Query whose range table is mapped.
 * @param columns   Pre-allocated array of length @p q->rtable->length.
 *                  Each element is allocated and filled by this function.
 * @param nbcols    Out-param: total number of non-provenance output columns.
 */
static void build_column_map(Query *q, int **columns, int *nbcols) {
  unsigned i = 0;
  ListCell *l;

  *nbcols = 0;

  foreach (l, q->rtable) {
    RangeTblEntry *r = (RangeTblEntry *)lfirst(l);
    ListCell *lc;

    columns[i] = 0;
    if (r->eref && r->eref->colnames != NIL) {
      unsigned j = 0;

      columns[i] = (int *)palloc(list_length(r->eref->colnames) * sizeof(int));

      foreach (lc, r->eref->colnames) {
        if (!lfirst(lc)) {
          /* Column without name — used e.g. when grouping by a discarded column */
          columns[i][j] = ++(*nbcols);
        } else {
          const char *v = strVal(lfirst(lc));

          if (strcmp(v, "") && r->rtekind != RTE_JOIN) { /* join RTE columns ignored */
            if (!strcmp(v, PROVSQL_COLUMN_NAME))
              columns[i][j] = -1;
            else
              columns[i][j] = ++(*nbcols);
          } else {
            columns[i][j] = 0;
          }
        }

        ++j;
      }
    }

    ++i;
  }
}

/**
 * @brief Categorisation of a top-level WHERE conjunct.
 *
 * Drives the unified WHERE classifier that replaced the original pair
 * @c migrate_aggtoken_quals_to_having + @c extract_rv_cmps_from_quals.
 * Both probabilistic flavours (agg_token's "moved to HAVING" world and
 * random_variable's "lifted to provenance" world) are special cases of
 * "this conjunct involves a probabilistic value the executor cannot
 * evaluate as a Boolean directly, so the planner has to route it to a
 * different evaluation site".  The classifier reports which site, or
 * (for unsupported mixes) errors.
 */
typedef enum {
  QUAL_DETERMINISTIC,    /**< no probabilistic value; stays in WHERE        */
  QUAL_PURE_AGG,         /**< pure agg_token expression; route to HAVING    */
  QUAL_PURE_RV,          /**< pure random_variable expression; lift to provenance */
  QUAL_MIXED_AGG_DET,    /**< agg_token mixed with non-agg leaves; error    */
  QUAL_MIXED_RV_DET,     /**< random_variable mixed with non-RV leaves; error */
  QUAL_MIXED_AGG_RV      /**< agg_token and random_variable in the same expr; error */
} qual_class;

/**
 * @brief Classify @p expr along the @c qual_class axis.
 *
 * Decision table (the predicates @c has_aggtoken,
 * @c expr_contains_rv_cmp, @c check_expr_on_aggregate, and
 * @c check_expr_on_rv each return whether the expression "contains" or
 * "is purely" the corresponding flavour):
 *
 * | aggtoken | rv_cmp | check_agg | check_rv | classification        |
 * |----------|--------|-----------|----------|-----------------------|
 * | yes      | yes    |    -      |    -     | QUAL_MIXED_AGG_RV     |
 * | yes      | no     |   true    |    -     | QUAL_PURE_AGG         |
 * | yes      | no     |   false   |    -     | QUAL_MIXED_AGG_DET    |
 * | no       | yes    |    -      |   true   | QUAL_PURE_RV          |
 * | no       | yes    |    -      |   false  | QUAL_MIXED_RV_DET     |
 * | no       | no     |    -      |    -     | QUAL_DETERMINISTIC    |
 */
static qual_class classify_qual(Expr *expr, const constants_t *constants)
{
  bool has_agg = has_aggtoken((Node *)expr, constants);
  bool has_rv  = expr_contains_rv_cmp((Node *)expr, constants);

  if (has_agg && has_rv)
    return QUAL_MIXED_AGG_RV;
  if (has_agg) {
    if (check_expr_on_aggregate(expr, constants))
      return QUAL_PURE_AGG;
    return QUAL_MIXED_AGG_DET;
  }
  if (has_rv) {
    if (check_expr_on_rv(expr, constants))
      return QUAL_PURE_RV;
    return QUAL_MIXED_RV_DET;
  }
  return QUAL_DETERMINISTIC;
}

/** @brief Raise the user-facing error appropriate to a mixed @p c.
 *
 * Each @c provsql_error call is @c ereport(ERROR), which does not
 * return; the explicit @c break statements below are present only to
 * keep @c -Wimplicit-fallthrough happy (PostgreSQL's @c elog macro is
 * not marked @c noreturn for the compiler's flow analysis). */
static void error_for_mixed_qual(qual_class c)
{
  switch (c) {
    case QUAL_MIXED_AGG_DET:
      provsql_error("Complex selection on aggregation results not supported");
      break;
    case QUAL_MIXED_RV_DET:
      provsql_error("WHERE clause mixes random_variable comparisons with "
                    "other predicates inside the same Boolean expression; "
                    "split the non-RV part into its own AND conjunct");
      break;
    case QUAL_MIXED_AGG_RV:
      provsql_error("WHERE clause mixes agg_token (HAVING-style) and "
                    "random_variable (per-tuple) comparisons inside the "
                    "same Boolean expression; this combination is not "
                    "supported");
      break;
    default:
      /* QUAL_DETERMINISTIC / QUAL_PURE_AGG / QUAL_PURE_RV: not a mixed case. */
      break;
  }
}

/**
 * @brief Unified WHERE classifier &ndash; routes each top-level conjunct
 *        to the right evaluation site in a single pass.
 *
 * Replaces and consolidates the original
 * @c migrate_aggtoken_quals_to_having (agg-only) and
 * @c extract_rv_cmps_from_quals (rv-only).  The two old functions were
 * structurally isomorphic: each walked the WHERE clause, classified
 * each top-level conjunct, and routed pure-X conjuncts somewhere
 * semantic (HAVING vs the returned rv_cmps list); the deterministic
 * conjuncts stayed in WHERE.  Doing it in one pass means the rare
 * conjunct that mixes agg_token and random_variable (which neither old
 * function would have caught cleanly) gets a deterministic, useful
 * error message.
 *
 * Supported shapes mirror the union of the two predecessors:
 * - Whole WHERE is a single conjunct: classify and route or error.
 * - Top-level AND of conjuncts: classify each, route, and (after
 *   walking) collapse the AND if it has zero or one remaining children
 *   so downstream code does not see a degenerate Boolean node.
 * - Top-level OR / NOT containing both deterministic and probabilistic
 *   leaves: error.
 *
 * @param constants  Extension OID cache.
 * @param q          Query whose @c jointree->quals and @c havingQual
 *                   may both be mutated in place.
 * @return List of @c FuncExpr nodes (one per lifted RV conjunct), each
 *         producing a @c UUID.  The caller conjoins these into
 *         @c prov_atts before @c make_provenance_expression.
 */
static List *
migrate_probabilistic_quals(const constants_t *constants, Query *q)
{
  List *rv_cmps = NIL;
  Node *quals;

  if (!q->jointree || !q->jointree->quals)
    return NIL;

  quals = q->jointree->quals;

  /* Whole WHERE is one conjunct (single OpExpr, or non-AND BoolExpr
   * which we treat opaquely &ndash; the per-flavour pure checks
   * @c check_expr_on_aggregate / @c check_expr_on_rv recurse through
   * the BoolExpr structure themselves). */
  if (!IsA(quals, BoolExpr) || ((BoolExpr *)quals)->boolop != AND_EXPR) {
    qual_class c = classify_qual((Expr *)quals, constants);
    error_for_mixed_qual(c);

    switch (c) {
      case QUAL_PURE_AGG:
        q->havingQual = add_to_havingQual(q->havingQual, (Expr *)quals);
        q->jointree->quals = NULL;
        break;
      case QUAL_PURE_RV:
        rv_cmps = lappend(rv_cmps,
                          rv_Expr_to_provenance((Expr *)quals,
                                                constants, false));
        q->jointree->quals = NULL;
        break;
      case QUAL_DETERMINISTIC:
        /* Leave WHERE alone. */
        break;
      default:
        /* Errors handled by error_for_mixed_qual. */
        break;
    }
    return rv_cmps;
  }

  /* Top-level AND: walk conjuncts. */
  {
    BoolExpr *be = (BoolExpr *)quals;
    ListCell *cell, *prev;

    for (cell = list_head(be->args), prev = NULL; cell != NULL;) {
      Expr *conjunct = (Expr *)lfirst(cell);
      qual_class c = classify_qual(conjunct, constants);

      error_for_mixed_qual(c);

      switch (c) {
        case QUAL_PURE_AGG:
          q->havingQual = add_to_havingQual(q->havingQual, conjunct);
          be->args = my_list_delete_cell(be->args, cell, prev);
          if (prev)
            cell = my_lnext(be->args, prev);
          else
            cell = list_head(be->args);
          break;
        case QUAL_PURE_RV:
          rv_cmps = lappend(rv_cmps,
                            rv_Expr_to_provenance(conjunct,
                                                  constants, false));
          be->args = my_list_delete_cell(be->args, cell, prev);
          if (prev)
            cell = my_lnext(be->args, prev);
          else
            cell = list_head(be->args);
          break;
        case QUAL_DETERMINISTIC:
          prev = cell;
          cell = my_lnext(be->args, cell);
          break;
        default:
          /* Errors handled by error_for_mixed_qual. */
          break;
      }
    }

    /* Collapse degenerate ANDs so downstream code sees a tidy WHERE. */
    if (be->args == NIL)
      q->jointree->quals = NULL;
    else if (list_length(be->args) == 1)
      q->jointree->quals = (Node *)linitial(be->args);
  }

  return rv_cmps;
}

/** @brief Context for the @c insert_agg_token_casts_mutator. */
typedef struct insert_agg_token_casts_context {
  Query *query;               ///< Outer query (to look up subquery RTEs)
  const constants_t *constants; ///< Extension OID cache
} insert_agg_token_casts_context;

/**
 * @brief Look up the original aggregate return type for an agg_token Var.
 *
 * Navigates from the Var's varno/varattno to the subquery's target list,
 * finds the provenance_aggregate() FuncExpr, and extracts the type OID
 * from its second argument (aggtype).
 */
static Oid get_agg_token_orig_type(Var *v, insert_agg_token_casts_context *ctx) {
  RangeTblEntry *rte;
  TargetEntry *te;

  if (v->varno < 1 || v->varno > list_length(ctx->query->rtable))
    return InvalidOid;

  rte = list_nth_node(RangeTblEntry, ctx->query->rtable, v->varno - 1);
  if (rte->rtekind != RTE_SUBQUERY || rte->subquery == NULL)
    return InvalidOid;

  if (v->varattno < 1 || v->varattno > list_length(rte->subquery->targetList))
    return InvalidOid;

  te = list_nth_node(TargetEntry, rte->subquery->targetList, v->varattno - 1);
  if (IsA(te->expr, FuncExpr)) {
    FuncExpr *f = (FuncExpr *)te->expr;
    if (f->funcid == ctx->constants->OID_FUNCTION_PROVENANCE_AGGREGATE) {
      Const *aggtype_const = (Const *)lsecond(f->args);
      return DatumGetObjectId(aggtype_const->constvalue);
    }
  }
  return InvalidOid;
}

/**
 * @brief Wrap an agg_token Var in a cast to its original type, in place.
 */
static void cast_agg_token_in_list(ListCell *lc,
                                   insert_agg_token_casts_context *ctx) {
  Var *v = (Var *)lfirst(lc);
  Oid target = get_agg_token_orig_type(v, ctx);
  HeapTuple castTuple;

  if (!OidIsValid(target))
    return;

  castTuple = SearchSysCache2(CASTSOURCETARGET,
                              ObjectIdGetDatum(ctx->constants->OID_TYPE_AGG_TOKEN),
                              ObjectIdGetDatum(target));
  if (HeapTupleIsValid(castTuple)) {
    Form_pg_cast castForm = (Form_pg_cast)GETSTRUCT(castTuple);
    if (OidIsValid(castForm->castfunc)) {
      FuncExpr *fc = makeNode(FuncExpr);
      fc->funcid = castForm->castfunc;
      fc->funcresulttype = target;
      fc->funcretset = false;
      fc->funcvariadic = false;
      fc->funcformat = COERCE_IMPLICIT_CAST;
      fc->funccollid = InvalidOid;
      fc->inputcollid = InvalidOid;
      fc->args = list_make1(v);
      fc->location = -1;
      lfirst(lc) = fc;
    }
    ReleaseSysCache(castTuple);
  }
}

/**
 * @brief Wrap any agg_token Vars in an argument list.
 */
static void cast_agg_token_args(List *args,
                                insert_agg_token_casts_context *ctx) {
  ListCell *lc;
  foreach (lc, args) {
    if (IsA(lfirst(lc), Var) &&
        ((Var *)lfirst(lc))->vartype == ctx->constants->OID_TYPE_AGG_TOKEN)
      cast_agg_token_in_list(lc, ctx);
  }
}

/**
 * @brief Insert agg_token casts for Vars used in expressions.
 *
 * After the WHERE-to-HAVING migration, agg_token Vars remaining in
 * expression nodes (OpExpr, WindowFunc, CoalesceExpr, MinMaxExpr, etc.)
 * need explicit casts to their original type so that operators and
 * functions receive correct values.  The original type is looked up
 * from the provenance_aggregate() call in the subquery.
 */
static Node *
insert_agg_token_casts_mutator(Node *node, void *data) {
  insert_agg_token_casts_context *ctx = (insert_agg_token_casts_context *)data;

  if (node == NULL)
    return NULL;

  if (IsA(node, OpExpr)) {
    cast_agg_token_args(((OpExpr *)node)->args, ctx);
    return (Node *)node;
  }
  if (IsA(node, WindowFunc)) {
    cast_agg_token_args(((WindowFunc *)node)->args, ctx);
    return (Node *)node;
  }
  if (IsA(node, CoalesceExpr)) {
    cast_agg_token_args(((CoalesceExpr *)node)->args, ctx);
    return (Node *)node;
  }
  if (IsA(node, MinMaxExpr)) {
    cast_agg_token_args(((MinMaxExpr *)node)->args, ctx);
    return (Node *)node;
  }
  if (IsA(node, NullIfExpr)) {
    cast_agg_token_args(((NullIfExpr *)node)->args, ctx);
    return (Node *)node;
  }

  return expression_tree_mutator(node, insert_agg_token_casts_mutator, data);
}

/**
 * @brief Walk query and insert agg_token casts where needed.
 */
static void insert_agg_token_casts(const constants_t *constants, Query *q) {
  insert_agg_token_casts_context ctx = {q, constants};
  query_tree_mutator(q, insert_agg_token_casts_mutator, &ctx,
                     QTW_DONT_COPY_QUERY | QTW_IGNORE_RC_SUBQUERIES);
}

/** @brief Context for @c join_qual_has_agg_token_walker. */
typedef struct join_qual_agg_token_ctx {
  const constants_t *constants; ///< Extension OID cache
  Index *rteid;                 ///< Out: varno of the agg_token Var
  AttrNumber *join_attno;       ///< Out: attno of the agg_token Var
} join_qual_agg_token_ctx;

static bool join_qual_has_agg_token_walker(Node *node,
                                           join_qual_agg_token_ctx *ctx)
{
  if (node == NULL)
    return false;
  if (IsA(node, OpExpr)) {
    OpExpr *oe = (OpExpr *) node;
    Node *left = (Node *) linitial(oe->args);
    Node *right = (Node *) lsecond(oe->args);

    /* Unwrap casts */
    if (IsA(left, FuncExpr) &&
        (((FuncExpr *)left)->funcformat == COERCE_IMPLICIT_CAST ||
         ((FuncExpr *)left)->funcformat == COERCE_EXPLICIT_CAST) &&
        list_length(((FuncExpr *)left)->args) == 1)
      left = linitial(((FuncExpr *)left)->args);
    if (IsA(right, FuncExpr) &&
        (((FuncExpr *)right)->funcformat == COERCE_IMPLICIT_CAST ||
         ((FuncExpr *)right)->funcformat == COERCE_EXPLICIT_CAST) &&
        list_length(((FuncExpr *)right)->args) == 1)
      right = linitial(((FuncExpr *)right)->args);

    if (IsA(left, Var) && IsA(right, Var)) {
      Var *left_var = (Var *)left;
      Var *right_var = (Var *)right;
      if (left_var->vartype == ctx->constants->OID_TYPE_AGG_TOKEN &&
          right_var->vartype != ctx->constants->OID_TYPE_AGG_TOKEN) {
        *ctx->rteid = left_var->varno;
        *ctx->join_attno = left_var->varattno;
        return true;
      }
      if (right_var->vartype == ctx->constants->OID_TYPE_AGG_TOKEN &&
          left_var->vartype != ctx->constants->OID_TYPE_AGG_TOKEN) {
        *ctx->rteid = right_var->varno;
        *ctx->join_attno = right_var->varattno;
        return true;
      }
    }
  }
  return expression_tree_walker(node, join_qual_has_agg_token_walker,
                                (void *) ctx);
}

/**
 * @brief Return true if @p node contains an @c OpExpr that equates an
 *        @c agg_token @c Var with a non-@c agg_token @c Var.
 *
 * On a match, writes the agg_token Var's @c varno and @c varattno to
 * @p *rteid and @p *join_attno.  Used to detect JOIN conditions that
 * require the @c rewrite_join_agg_token rewrite.
 *
 * @param node        Expression tree to inspect.
 * @param constants   Extension OID cache.
 * @param rteid       Out: varno of the agg_token Var (unchanged on miss).
 * @param join_attno  Out: attno of the agg_token Var (unchanged on miss).
 * @return            True iff such an @c OpExpr was found.
 */
static bool join_qual_has_agg_token(Node *node, const constants_t *constants,
                                    Index *rteid, AttrNumber *join_attno)
{
  join_qual_agg_token_ctx ctx;
  ctx.constants   = constants;
  ctx.rteid       = rteid;
  ctx.join_attno  = join_attno;
  return join_qual_has_agg_token_walker(node, &ctx);
}

/**
 * @brief Build an AST node for <tt>arr[idx]</tt> on a uuid[] expression.
 *
 * Wraps the version rename between @c ArrayRef (PG < 12) and
 * @c SubscriptingRef (PG 12+), and the addition of @c refrestype (PG 14+).
 *
 * @param arr_expr   Expression evaluating to @c uuid[].
 * @param index      1-based element position.
 * @param constants  Extension OID cache.
 * @return           Subscripting node with result type @c uuid.
 */
static Node *make_uuid_array_subscript(Node *arr_expr, int index,
                                       const constants_t *constants)
{
  Const *idx = makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                         Int32GetDatum(index), false, true);
#if PG_VERSION_NUM >= 120000
  SubscriptingRef *sub = makeNode(SubscriptingRef);
  sub->refcontainertype = constants->OID_TYPE_UUID_ARRAY;
  sub->refelemtype      = constants->OID_TYPE_UUID;
#if PG_VERSION_NUM >= 140000
  sub->refrestype       = constants->OID_TYPE_UUID;
#endif
  sub->reftypmod        = -1;
  sub->refcollid        = InvalidOid;
  sub->refupperindexpr  = list_make1(idx);
  sub->reflowerindexpr  = NIL;
  sub->refexpr          = (Expr *)arr_expr;
  sub->refassgnexpr     = NULL;
  return (Node *)sub;
#else
  ArrayRef *sub = makeNode(ArrayRef);
  sub->refarraytype     = constants->OID_TYPE_UUID_ARRAY;
  sub->refelemtype      = constants->OID_TYPE_UUID;
  sub->reftypmod        = -1;
  sub->refcollid        = InvalidOid;
  sub->refupperindexpr  = list_make1(idx);
  sub->reflowerindexpr  = NIL;
  sub->refexpr          = (Expr *)arr_expr;
  sub->refassgnexpr     = NULL;
  return (Node *)sub;
#endif
}

/**
 * @brief Context for @c retype_agg_var_walker.
 *
 * Identifies the Var location whose type must flip from @c agg_token
 * to @c text after the source relation has been replaced by an
 * explode-style subquery.
 */
typedef struct retype_agg_var_ctx {
  Index rteid;                 ///< Varno of the replaced RTE
  AttrNumber join_attno;       ///< Attno of the former agg_token column
  const constants_t *constants;///< Extension OID cache
} retype_agg_var_ctx;

/**
 * @brief Walker that retypes agg_token Vars to text and rewrites the
 *        equality OpExpr to @c text = text with the non-agg side cast via I/O.
 *
 * Only affects Vars with @c varlevelsup == 0 matching @c (rteid, join_attno).
 * Sibling-query subqueries are left untouched via @c QTW_IGNORE_RT_SUBQUERIES
 * at the top-level call.
 */
static bool retype_agg_var_walker(Node *node, retype_agg_var_ctx *ctx)
{
  if (node == NULL)
    return false;

  if (IsA(node, OpExpr)) {
    OpExpr *oe = (OpExpr *)node;
    if (list_length(oe->args) == 2) {
      Node *left  = (Node *)linitial(oe->args);
      Node *right = (Node *)lsecond(oe->args);
      Var  *agg_v = NULL;
      bool agg_on_left = false;

      if (IsA(left, Var)) {
        Var *v = (Var *)left;
        if (v->varlevelsup == 0 && v->varno == ctx->rteid &&
            v->varattno == ctx->join_attno &&
            v->vartype == ctx->constants->OID_TYPE_AGG_TOKEN) {
          agg_v = v;
          agg_on_left = true;
        }
      }
      if (agg_v == NULL && IsA(right, Var)) {
        Var *v = (Var *)right;
        if (v->varlevelsup == 0 && v->varno == ctx->rteid &&
            v->varattno == ctx->join_attno &&
            v->vartype == ctx->constants->OID_TYPE_AGG_TOKEN) {
          agg_v = v;
        }
      }

      if (agg_v != NULL) {
        Node *other = agg_on_left ? right : left;
        Oid text_eq;
        Operator opInfo;
        Form_pg_operator opform;

        agg_v->vartype   = TEXTOID;
        agg_v->varcollid = DEFAULT_COLLATION_OID;

        if (exprType(other) != TEXTOID) {
          CoerceViaIO *c = makeNode(CoerceViaIO);
          c->arg          = (Expr *)other;
          c->resulttype   = TEXTOID;
          c->resultcollid = DEFAULT_COLLATION_OID;
          c->coerceformat = COERCE_EXPLICIT_CAST;
          c->location     = -1;
          other = (Node *)c;
        }

        if (agg_on_left)
          oe->args = list_make2(agg_v, other);
        else
          oe->args = list_make2(other, agg_v);

        text_eq = find_equality_operator(TEXTOID, TEXTOID);
        if (!OidIsValid(text_eq))
          provsql_error("rewrite_join_agg_token: text = text operator "
                        "not found");
        opInfo = SearchSysCache1(OPEROID, ObjectIdGetDatum(text_eq));
        if (!HeapTupleIsValid(opInfo))
          provsql_error("rewrite_join_agg_token: could not look up "
                        "text equality operator");
        opform = (Form_pg_operator)GETSTRUCT(opInfo);
        oe->opno         = text_eq;
        oe->opfuncid     = opform->oprcode;
        oe->opresulttype = opform->oprresult;
        oe->inputcollid  = DEFAULT_COLLATION_OID;
        ReleaseSysCache(opInfo);

        /* Args handled; skip their subtree walk */
        return false;
      }
    }
  }

  if (IsA(node, Var)) {
    Var *v = (Var *)node;
    if (v->varlevelsup == 0 && v->varno == ctx->rteid &&
        v->varattno == ctx->join_attno &&
        v->vartype == ctx->constants->OID_TYPE_AGG_TOKEN) {
      v->vartype   = TEXTOID;
      v->varcollid = DEFAULT_COLLATION_OID;
    }
    return false;
  }

  if (IsA(node, Query)) {
    /* Nested queries address a different rtable; do not descend. */
    return false;
  }

  return expression_tree_walker(node, retype_agg_var_walker, (void *)ctx);
}

/**
 * @brief Replace the source relation of an agg_token JOIN with an
 *        explode-style subquery.
 *
 * Given a JOIN qual of the form @c rteid.join_attno = other where
 * @c rteid.join_attno is of type @c agg_token, replaces the RTE at @p rteid
 * in place with a subquery:
 *
 * @code{.sql}
 *   SELECT t.col_1, ..., t.col_{join_attno-1},
 *          get_extra(get_children(sm)[2])              AS <agg_col>,
 *          ...,
 *          provenance_times(get_children(sm)[1], t.provsql) AS provsql
 *   FROM <t>, LATERAL unnest(get_children(t.<agg_col>)) AS sm
 * @endcode
 *
 * The subquery preserves the original column order, so outer Vars still
 * address the same attnos.  The outer query is then walked to retype Vars
 * at (@p rteid, @p join_attno) from @c agg_token to @c text and rewrite the
 * equality @c OpExpr to @c text = text (casting the other side via I/O).
 *
 * The copy of the source RTE inside the subquery has its @c provsql column
 * renamed so the recursive @c process_query pass does not re-detect it as a
 * provenance source — the combined provenance is already captured by the
 * subquery's exposed @c provsql target entry.
 *
 * @param q          Query to rewrite (modified in place).
 * @param constants  Extension OID cache.
 * @param rteid      1-based varno of the RTE owning the agg_token column.
 * @param join_attno 1-based attno of the agg_token column in that RTE.
 * @return           The modified query.
 */
static Query *rewrite_join_agg_token(Query *q, const constants_t *constants,
                                     Index rteid, AttrNumber join_attno)
{
  RangeTblEntry *src_rte = (RangeTblEntry *)list_nth(q->rtable, rteid - 1);
  AttrNumber provsql_attno = 0;
  AttrNumber attno;
  ListCell *lc;
  Query *inner;
  RangeTblEntry *inner_src, *sm_rte;
  RangeTblFunction *rtfunc;
  FuncExpr *unnest_call, *get_children_of_agg, *agg_to_uuid;
  Var *agg_var_in_inner;
  Alias *sm_alias, *sm_eref;
  RangeTblRef *inner_rtr1, *inner_rtr2;
  FromExpr *inner_jt;
  List *inner_tl = NIL;

  if (src_rte->rtekind != RTE_RELATION && src_rte->rtekind != RTE_SUBQUERY)
    provsql_error("rewrite_join_agg_token: source RTE kind %d not supported",
                  (int)src_rte->rtekind);

  /* Locate the provsql column of the source RTE. */
  attno = 1;
  foreach (lc, src_rte->eref->colnames) {
    if (!strcmp(strVal(lfirst(lc)), PROVSQL_COLUMN_NAME)) {
      provsql_attno = attno;
      break;
    }
    ++attno;
  }
  if (provsql_attno == 0)
    provsql_error("rewrite_join_agg_token: source relation has no "
                  "provsql column");

  /* --- Build the lateral RTE: unnest(get_children(agg_token_uuid(agg_var))) --- */

  agg_var_in_inner = makeNode(Var);
  agg_var_in_inner->varno     = 1;
  agg_var_in_inner->varattno  = join_attno;
  agg_var_in_inner->vartype   = constants->OID_TYPE_AGG_TOKEN;
  agg_var_in_inner->varcollid = InvalidOid;
  agg_var_in_inner->vartypmod = -1;
  agg_var_in_inner->location  = -1;

  agg_to_uuid = makeNode(FuncExpr);
  agg_to_uuid->funcid         = constants->OID_FUNCTION_AGG_TOKEN_UUID;
  agg_to_uuid->funcresulttype = constants->OID_TYPE_UUID;
  agg_to_uuid->funcretset     = false;
  agg_to_uuid->funcvariadic   = false;
  agg_to_uuid->funcformat     = COERCE_IMPLICIT_CAST;
  agg_to_uuid->funccollid     = InvalidOid;
  agg_to_uuid->inputcollid    = InvalidOid;
  agg_to_uuid->args           = list_make1(agg_var_in_inner);
  agg_to_uuid->location       = -1;

  get_children_of_agg = makeNode(FuncExpr);
  get_children_of_agg->funcid         = constants->OID_FUNCTION_GET_CHILDREN;
  get_children_of_agg->funcresulttype = constants->OID_TYPE_UUID_ARRAY;
  get_children_of_agg->funcretset     = false;
  get_children_of_agg->funcvariadic   = false;
  get_children_of_agg->funcformat     = COERCE_EXPLICIT_CALL;
  get_children_of_agg->funccollid     = InvalidOid;
  get_children_of_agg->inputcollid    = InvalidOid;
  get_children_of_agg->args           = list_make1(agg_to_uuid);
  get_children_of_agg->location       = -1;

  unnest_call = makeNode(FuncExpr);
  unnest_call->funcid         = constants->OID_UNNEST;
  unnest_call->funcresulttype = constants->OID_TYPE_UUID;
  unnest_call->funcretset     = true;
  unnest_call->funcvariadic   = false;
  unnest_call->funcformat     = COERCE_EXPLICIT_CALL;
  unnest_call->funccollid     = InvalidOid;
  unnest_call->inputcollid    = InvalidOid;
  unnest_call->args           = list_make1(get_children_of_agg);
  unnest_call->location       = -1;

  rtfunc = makeNode(RangeTblFunction);
  rtfunc->funcexpr          = (Node *)unnest_call;
  rtfunc->funccolcount      = 1;
  rtfunc->funccolnames      = NIL;
  rtfunc->funccoltypes      = NIL;
  rtfunc->funccoltypmods    = NIL;
  rtfunc->funccolcollations = NIL;
  rtfunc->funcparams        = NULL;

  sm_alias = makeNode(Alias);
  sm_eref  = makeNode(Alias);
  sm_alias->aliasname = "sm";
  sm_eref->aliasname  = "sm";
  sm_eref->colnames   = list_make1(makeString("sm"));

  sm_rte = makeNode(RangeTblEntry);
  sm_rte->rtekind        = RTE_FUNCTION;
  sm_rte->functions      = list_make1(rtfunc);
  sm_rte->funcordinality = false;
  sm_rte->alias          = sm_alias;
  sm_rte->eref           = sm_eref;
  sm_rte->lateral        = true;
  sm_rte->inFromCl       = true;
#if PG_VERSION_NUM < 160000
  sm_rte->requiredPerms = 0;
#endif

  /* --- Inner rtable RTE 1: the source relation (deep copy). --- */

  inner_src = copyObject(src_rte);

  /* Rename the provsql column in the inner RTE's eref so the recursive
   * process_query pass does not re-detect it as a provenance source.  The
   * combined provenance is already captured by the subquery's exposed
   * provsql TargetEntry below. */
  hide_provsql_colname(inner_src);

  inner_rtr1 = makeNode(RangeTblRef);
  inner_rtr1->rtindex = 1;
  inner_rtr2 = makeNode(RangeTblRef);
  inner_rtr2->rtindex = 2;
  inner_jt = makeNode(FromExpr);
  inner_jt->fromlist = list_make2(inner_rtr1, inner_rtr2);
  inner_jt->quals    = NULL;

  /* --- Target list of the inner subquery, preserving original column order. --- */

  attno = 1;
  foreach (lc, src_rte->eref->colnames) {
    const char *colname = strVal(lfirst(lc));
    TargetEntry *te = makeNode(TargetEntry);
    te->resno   = attno;
    te->resname = pstrdup(colname);
    te->resjunk = false;

    if (attno == join_attno) {
      /* get_extra(get_children(sm)[2]) */
      Var *sm_var = makeNode(Var);
      FuncExpr *gch, *ge;
      Node *subscript;

      sm_var->varno     = 2;
      sm_var->varattno  = 1;
      sm_var->vartype   = constants->OID_TYPE_UUID;
      sm_var->varcollid = InvalidOid;
      sm_var->vartypmod = -1;
      sm_var->location  = -1;

      gch = makeNode(FuncExpr);
      gch->funcid         = constants->OID_FUNCTION_GET_CHILDREN;
      gch->funcresulttype = constants->OID_TYPE_UUID_ARRAY;
      gch->funcretset     = false;
      gch->funcvariadic   = false;
      gch->funcformat     = COERCE_EXPLICIT_CALL;
      gch->funccollid     = InvalidOid;
      gch->inputcollid    = InvalidOid;
      gch->args           = list_make1(sm_var);
      gch->location       = -1;

      subscript = make_uuid_array_subscript((Node *)gch, 2, constants);

      ge = makeNode(FuncExpr);
      ge->funcid         = constants->OID_FUNCTION_GET_EXTRA;
      ge->funcresulttype = TEXTOID;
      ge->funcretset     = false;
      ge->funcvariadic   = false;
      ge->funcformat     = COERCE_EXPLICIT_CALL;
      ge->funccollid     = DEFAULT_COLLATION_OID;
      ge->inputcollid    = InvalidOid;
      ge->args           = list_make1(subscript);
      ge->location       = -1;

      te->expr = (Expr *)ge;
    } else if (attno == provsql_attno) {
      /* provenance_times(get_children(sm)[1], t.provsql) — VARIADIC uuid[] */
      Var *sm_var = makeNode(Var);
      Var *prov_var = makeNode(Var);
      FuncExpr *gch, *pt;
      ArrayExpr *arr;
      Node *subscript;

      sm_var->varno     = 2;
      sm_var->varattno  = 1;
      sm_var->vartype   = constants->OID_TYPE_UUID;
      sm_var->varcollid = InvalidOid;
      sm_var->vartypmod = -1;
      sm_var->location  = -1;

      gch = makeNode(FuncExpr);
      gch->funcid         = constants->OID_FUNCTION_GET_CHILDREN;
      gch->funcresulttype = constants->OID_TYPE_UUID_ARRAY;
      gch->funcretset     = false;
      gch->funcvariadic   = false;
      gch->funcformat     = COERCE_EXPLICIT_CALL;
      gch->funccollid     = InvalidOid;
      gch->inputcollid    = InvalidOid;
      gch->args           = list_make1(sm_var);
      gch->location       = -1;

      subscript = make_uuid_array_subscript((Node *)gch, 1, constants);

      prov_var->varno     = 1;
      prov_var->varattno  = provsql_attno;
      prov_var->vartype   = constants->OID_TYPE_UUID;
      prov_var->varcollid = InvalidOid;
      prov_var->vartypmod = -1;
      prov_var->location  = -1;

      arr = makeNode(ArrayExpr);
      arr->array_typeid   = constants->OID_TYPE_UUID_ARRAY;
      arr->element_typeid = constants->OID_TYPE_UUID;
      arr->elements       = list_make2(subscript, prov_var);
      arr->location       = -1;

      pt = makeNode(FuncExpr);
      pt->funcid         = constants->OID_FUNCTION_PROVENANCE_TIMES;
      pt->funcresulttype = constants->OID_TYPE_UUID;
      pt->funcretset     = false;
      pt->funcvariadic   = true;
      pt->funcformat     = COERCE_EXPLICIT_CALL;
      pt->funccollid     = InvalidOid;
      pt->inputcollid    = InvalidOid;
      pt->args           = list_make1(arr);
      pt->location       = -1;

      te->expr = (Expr *)pt;
    } else {
      /* Passthrough Var(1, attno). */
      Var *v = makeNode(Var);
      Oid vtype   = InvalidOid;
      int32 vtypmod = -1;
      Oid vcoll   = InvalidOid;

      if (src_rte->rtekind == RTE_RELATION) {
        get_atttypetypmodcoll(src_rte->relid, attno, &vtype, &vtypmod, &vcoll);
      } else { /* RTE_SUBQUERY */
        TargetEntry *sub_te =
          (TargetEntry *)list_nth(src_rte->subquery->targetList, attno - 1);
        vtype   = exprType((Node *)sub_te->expr);
        vtypmod = exprTypmod((Node *)sub_te->expr);
        vcoll   = exprCollation((Node *)sub_te->expr);
      }

      v->varno     = 1;
      v->varattno  = attno;
      v->vartype   = vtype;
      v->varcollid = vcoll;
      v->vartypmod = vtypmod;
      v->location  = -1;
      te->expr = (Expr *)v;
    }

    inner_tl = lappend(inner_tl, te);
    ++attno;
  }

  inner = makeNode(Query);
  inner->commandType = CMD_SELECT;
  inner->canSetTag   = true;
  inner->rtable      = list_make2(inner_src, sm_rte);
  inner->jointree    = inner_jt;
  inner->targetList  = inner_tl;
  inner->hasAggs     = false;
  inner->hasSubLinks = false;

#if PG_VERSION_NUM >= 160000
  /* PG 16+ moved permission info from RangeTblEntry into a separate
   * Query.rteperminfos list, indexed by RangeTblEntry.perminfoindex.
   * Our copy of src_rte kept its original perminfoindex, so the inner
   * query needs a matching rteperminfos entry — without it, perminfoindex
   * dangles and the planner short-circuits the subquery. */
  if (inner_src->perminfoindex != 0) {
    RTEPermissionInfo *perminfo =
      getRTEPermissionInfo(q->rteperminfos, src_rte);
    inner->rteperminfos = list_make1(copyObject(perminfo));
    inner_src->perminfoindex = 1;
  }
#endif

  /* --- Replace src_rte in place with the subquery; outer varnos unchanged. --- */

  src_rte->rtekind     = RTE_SUBQUERY;
  src_rte->subquery    = inner;
  src_rte->relid       = InvalidOid;
  src_rte->relkind     = 0;
#if PG_VERSION_NUM >= 120000
  src_rte->rellockmode = 0; /* field added in PG 12 */
#endif
  src_rte->inh         = false;
  src_rte->lateral     = false;
#if PG_VERSION_NUM >= 160000
  src_rte->perminfoindex = 0;
#else
  src_rte->selectedCols  = NULL;
  src_rte->insertedCols  = NULL;
  src_rte->updatedCols   = NULL;
  src_rte->requiredPerms = ACL_SELECT;
#endif

  /* Drop the "provsql" entry from the outer RTE's eref->colnames.
   * get_provenance_attributes will scan the subquery's target list for
   * a "provsql" TE and reinsert the colname at the matching position,
   * keeping eref->colnames length in sync with the subquery's target
   * list.  Without this, the pre-existing "provsql" entry (inherited
   * from the original relation) plus the reinsertion would produce a
   * 5-colname list for a 4-column subquery, which PostgreSQL rejects. */
  {
    ListCell *cell, *prev;
    AttrNumber i;

    prev = NULL;
    i = 1;
    for (cell = list_head(src_rte->eref->colnames); cell != NULL; ) {
      if (i == provsql_attno) {
        src_rte->eref->colnames =
          my_list_delete_cell(src_rte->eref->colnames, cell, prev);
        break;
      }
      prev = cell;
      cell = my_lnext(src_rte->eref->colnames, cell);
      ++i;
    }
  }

  /* --- Retype outer Vars (rteid, join_attno) from agg_token to text and
   *     rewrite the equality OpExpr to text = text. --- */
  {
    retype_agg_var_ctx ctx;
    ctx.rteid      = rteid;
    ctx.join_attno = join_attno;
    ctx.constants  = constants;
    query_tree_walker(q, retype_agg_var_walker, (void *)&ctx,
                      QTW_IGNORE_RT_SUBQUERIES);
  }

  return q;
}

/**
 * @brief Wrap @p expr in a @c provsql.assume_boolean FuncExpr.
 *
 * Used by @c make_provenance_expression when its caller (the
 * safe-query rewrite path in @c process_query) flagged the result
 * as needing the @c gate_assumed_boolean structural marker.
 * Wrapping at expression-build time rather than at splice time
 * means @c add_to_select and
 * @c replace_provenance_function_by_expression both consume the
 * already-wrapped expression, so every per-row root occurrence in
 * the final target list -- the auto-added @c provsql column and
 * every substituted user-side @c provenance() call -- carries the
 * wrapper uniformly.
 *
 * @param constants  Extension OID cache.
 * @param expr       Provenance expression to wrap.
 * @return  A @c FuncExpr applying @c provsql.assume_boolean to @p expr.
 */
static Expr *wrap_in_assume_boolean(const constants_t *constants,
                                    Expr *expr) {
  FuncExpr *wrap = makeNode(FuncExpr);
  wrap->funcid = constants->OID_FUNCTION_ASSUME_BOOLEAN;
  wrap->funcresulttype = constants->OID_TYPE_UUID;
  wrap->funcretset = false;
  wrap->funcvariadic = false;
  wrap->funcformat = COERCE_EXPLICIT_CALL;
  wrap->funccollid = InvalidOid;
  wrap->inputcollid = InvalidOid;
  wrap->args = list_make1(expr);
  wrap->location = -1;
  return (Expr *) wrap;
}

/**
 * @brief Wrap @p expr in a @c provsql.annotate(uuid, text) FuncExpr carrying
 *        @p cert.
 *
 * Used by @c make_provenance_expression to attach the inversion-free
 * tractability certificate to the per-row provenance root: the resulting
 * annotation gate is transparent for every evaluator and carries @p cert in
 * its @c extra (and folded into its UUID).  @p cert is copied into a text
 * @c Const.
 */
static Expr *wrap_in_annotate(const constants_t *constants, Expr *expr,
                              const char *cert) {
  FuncExpr *wrap = makeNode(FuncExpr);
  Const *ce = makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                        CStringGetTextDatum(cert), false, false);
  wrap->funcid = constants->OID_FUNCTION_ANNOTATE;
  wrap->funcresulttype = constants->OID_TYPE_UUID;
  wrap->funcretset = false;
  wrap->funcvariadic = false;
  wrap->funcformat = COERCE_EXPLICIT_CALL;
  wrap->funccollid = InvalidOid;
  wrap->inputcollid = DEFAULT_COLLATION_OID;
  wrap->args = list_make2(expr, (Expr *) ce);
  wrap->location = -1;
  return (Expr *) wrap;
}

/** @brief Mark column @p attno of RTE @p r as selected (read permission). */
static void mark_col_selected(Query *q, RangeTblEntry *r, AttrNumber attno) {
#if PG_VERSION_NUM >= 160000
  if (r->perminfoindex != 0) {
    RTEPermissionInfo *rpi =
      list_nth_node(RTEPermissionInfo, q->rteperminfos, r->perminfoindex - 1);
    rpi->selectedCols = bms_add_member(
      rpi->selectedCols, attno - FirstLowInvalidHeapAttributeNumber);
  }
#else
  r->selectedCols = bms_add_member(r->selectedCols,
                                   attno - FirstLowInvalidHeapAttributeNumber);
#endif
}

/** @brief A @c Var for column @p attno of RTE @p relid, with the column's
 *         actual type/typmod/collation, marking the column selected. */
static Var *make_column_var(Query *q, RangeTblEntry *r, Index relid,
                            AttrNumber attno) {
  Oid typid; int32 typmod; Oid coll;
  Var *v;
  get_atttypetypmodcoll(r->relid, attno, &typid, &typmod, &coll);
  v = makeVar(relid, attno, typid, typmod, coll, 0);
  v->location = -1;
  mark_col_selected(q, r, attno);
  return v;
}

/** @brief Coerce @p arg to @c text via its output function (any type -> text). */
static Expr *coerce_via_io_to_text(Expr *arg) {
  CoerceViaIO *c = makeNode(CoerceViaIO);
  c->arg = arg;
  c->resulttype = TEXTOID;
  c->resultcollid = DEFAULT_COLLATION_OID;
  c->coerceformat = COERCE_IMPLICIT_CAST;
  c->location = -1;
  return (Expr *) c;
}

/**
 * @brief Wrap an atom's provenance @c Var in the inversion-free per-input
 *        order marker: @c annotate(prov, inversion_free_key(root, sec, factor)).
 *
 * @p prov_var is a @c Var on the atom's provsql column (its @c varno is the
 * range-table index of the atom); @p m gives the root- and secondary-class
 * columns and the factor for that atom.
 */
static Expr *build_inversion_free_marker(const constants_t *constants, Query *q,
                                         Var *prov_var, const InvFreeMarker *m) {
  Index relid = prov_var->varno;
  RangeTblEntry *r = list_nth_node(RangeTblEntry, q->rtable, relid - 1);
  Var *rootv = make_column_var(q, r, relid, m->root_col);
  Const *factorc = makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
                             Int32GetDatum(m->factor), false, true);
  FuncExpr *keyf = makeNode(FuncExpr);
  FuncExpr *ann = makeNode(FuncExpr);
  Expr *secarg;

  /* A root-only atom (no secondary class, e.g. a self-join-free hierarchical
   * query's atoms all binding only the head variable) carries a constant
   * secondary key: every such input shares the single tile of its block. */
  if (m->sec_col == 0)
    secarg = (Expr *) makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
                                CStringGetTextDatum("0"), false, false);
  else
    secarg = coerce_via_io_to_text(
               (Expr *) make_column_var(q, r, relid, m->sec_col));

  keyf->funcid = constants->OID_FUNCTION_INVERSION_FREE_KEY;
  keyf->funcresulttype = TEXTOID;
  keyf->funcretset = false;
  keyf->funcvariadic = false;
  keyf->funcformat = COERCE_EXPLICIT_CALL;
  keyf->funccollid = DEFAULT_COLLATION_OID;
  keyf->inputcollid = DEFAULT_COLLATION_OID;
  keyf->args = list_make3(coerce_via_io_to_text((Expr *) rootv),
                          secarg,
                          (Expr *) factorc);
  keyf->location = -1;

  ann->funcid = constants->OID_FUNCTION_ANNOTATE;
  ann->funcresulttype = constants->OID_TYPE_UUID;
  ann->funcretset = false;
  ann->funcvariadic = false;
  ann->funcformat = COERCE_EXPLICIT_CALL;
  ann->funccollid = InvalidOid;
  ann->inputcollid = DEFAULT_COLLATION_OID;
  ann->args = list_make2((Expr *) prov_var, (Expr *) keyf);
  ann->location = -1;
  return (Expr *) ann;
}

/**
 * @brief Replace each certified atom's provenance @c Var in @p prov_atts with
 *        its per-input-marker-wrapped form (in place).
 */
static void wrap_inversion_free_markers(const constants_t *constants, Query *q,
                                        List *prov_atts,
                                        const InvFreeMarker *markers,
                                        int natoms) {
  ListCell *lc;
  foreach (lc, prov_atts) {
    Node *n = (Node *) lfirst(lc);
    if (IsA(n, Var)) {
      Var *pv = (Var *) n;
      if (pv->varno >= 1 && (int) pv->varno <= natoms
          && markers[pv->varno - 1].valid)
        lfirst(lc) = build_inversion_free_marker(constants, q, pv,
                                                 &markers[pv->varno - 1]);
    }
  }
}

/* -------------------------------------------------------------------------
 * Inversion-free: conjunctive flattening of SPJ subqueries/views
 * ------------------------------------------------------------------------- */

/**
 * @brief Where a flattened base atom came from, for mapping markers back.
 *
 * A slot @em path from the top lineage query down to the base relation: each
 * element is a 1-based range-table slot, and the last element is the base's
 * position within the innermost subquery.  @c depth @c == @c 1 (@c path @c ==
 * @c [s]) is a base/kept relation directly at top slot @c s; deeper paths step
 * through one nested SPJ subquery per element, so views-over-views map back to
 * the right input through the recursive subquery rewrite.
 */
typedef struct FlatAtomOrigin {
  int  depth;
  int *path;         /* palloc'd, length depth (1-based slot indices) */
} FlatAtomOrigin;

/** @brief Context for @c flatten_mut (a multi-relation conjunctive inliner). */
typedef struct flatten_ctx {
  int     N;               /* original parent range-table length */
  bool   *slot_flat;       /* [1..N]: parent slot is an inlined subquery */
  int    *parent_newpos;   /* [1..N]: new varno of a kept (non-inlined) slot */
  int   **sub_newpos;      /* [1..N] -> [1..sub_rtlen]: new varno of a subquery base */
  int    *sub_rtlen;       /* [1..N]: that subquery's range-table length */
  Var  ***sub_tl;          /* [1..N] -> [1..sub_tl_n]: subquery TL base Var by resno */
  int    *sub_tl_n;        /* [1..N] */
  bool    quals_mode;      /* true while remapping a subquery's pulled-up WHERE */
  int     quals_slot;      /* the inlined parent slot whose WHERE is being remapped */
} flatten_ctx;

/**
 * @brief Tree mutator implementing the conjunctive inlining of SPJ subqueries.
 *
 * Parent mode (@c quals_mode false): a @c Var on an inlined subquery slot is
 * replaced by the base @c Var its target list maps the column to, renumbered to
 * that base's new flat position; a @c Var on a kept slot is renumbered to the
 * slot's new position.  Subquery-WHERE mode (@c quals_mode true): a base @c Var
 * inside subquery @c quals_slot is renumbered to its new flat position.  Outer
 * references (@c varlevelsup > 0) are never touched.
 */
static Node *flatten_mut(Node *node, void *cp) {
  flatten_ctx *c = (flatten_ctx *) cp;
  if (node == NULL)
    return NULL;
  if (IsA(node, Var)) {
    Var *v = (Var *) node;
    if (v->varlevelsup == 0) {
      if (c->quals_mode) {
        int i = c->quals_slot;
        if ((int) v->varno >= 1 && (int) v->varno <= c->sub_rtlen[i]
            && c->sub_newpos[i][v->varno] > 0) {
          Var *nv = (Var *) copyObject(v);
          nv->varno = c->sub_newpos[i][v->varno];
          return (Node *) nv;
        }
      } else if ((int) v->varno >= 1 && (int) v->varno <= c->N) {
        int i = (int) v->varno;
        if (c->slot_flat[i]) {
          if (v->varattno >= 1 && v->varattno <= c->sub_tl_n[i]
              && c->sub_tl[i][v->varattno] != NULL) {
            Var *base = c->sub_tl[i][v->varattno];
            Var *nv = (Var *) copyObject(base);
            nv->varno = c->sub_newpos[i][base->varno];
            nv->varlevelsup = 0;
            return (Node *) nv;
          }
        } else {
          Var *nv = (Var *) copyObject(v);
          nv->varno = c->parent_newpos[i];
          return (Node *) nv;
        }
      }
    }
    return (Node *) copyObject(v);
  }
  return expression_tree_mutator(node, flatten_mut, cp);
}

/** @brief A depth-1 origin path @c [slot]. */
static FlatAtomOrigin *flat_origin1(int slot) {
  FlatAtomOrigin *o = (FlatAtomOrigin *) palloc(sizeof(FlatAtomOrigin));
  o->depth = 1;
  o->path = (int *) palloc(sizeof(int));
  o->path[0] = slot;
  return o;
}

/** @brief Prepend @p slot to @p sub's path, for an atom inlined one level up. */
static FlatAtomOrigin *flat_origin_prepend(int slot, const FlatAtomOrigin *sub) {
  FlatAtomOrigin *o = (FlatAtomOrigin *) palloc(sizeof(FlatAtomOrigin));
  int d;
  o->depth = sub->depth + 1;
  o->path = (int *) palloc(o->depth * sizeof(int));
  o->path[0] = slot;
  for (d = 0; d < sub->depth; d++)
    o->path[d + 1] = sub->path[d];
  return o;
}

/* Forward declaration: the flattener recurses into nested subqueries. */
static FlatAtomOrigin *flatten_spj_subqueries(Query *probe, int *nflat_out);

/**
 * @brief In place, inline every SPJ subquery/view of @p probe into its base
 *        relations, flattening to one conjunction of base atoms.
 *
 * A range-table slot is inlined when it is a non-lateral @c RTE_SUBQUERY whose
 * subquery is a plain SELECT (no aggregation, grouping, DISTINCT, set
 * operation, sublink, CTE or LIMIT), whose @c FROM is flat @c RangeTblRefs over
 * base @c RTE_RELATIONs (PG 14/15 view OLD/NEW placeholders ignored; one or
 * more bases -- a view with a join inside is fine), and whose non-junk target
 * list entries are all plain @c Vars on those bases.  Such a subquery is a pure
 * SPJ over base relations: its bases are appended in place of the slot, the
 * parent's column references are substituted by the corresponding base columns,
 * and the subquery's WHERE is pulled up, yielding an equivalent flat
 * conjunction.  The parent's own @c FROM must already be flat @c RangeTblRefs
 * (the detector requires this too); an explicit @c JoinExpr there carries
 * ON-conditions a fromlist rebuild would drop, so flattening is declined.
 *
 * @param probe      the (throwaway) query copy to flatten in place.
 * @param nflat_out  set to the flattened range-table length.
 * @return a palloc'd @c FlatAtomOrigin per flattened position, mapping it back
 * to the parent slot (and, for an inlined subquery, the base position within
 * it) so the detector's per-atom markers can be threaded to the right input.
 */
static FlatAtomOrigin *flatten_spj_subqueries(Query *probe, int *nflat_out) {
  int             N = list_length(probe->rtable);
  flatten_ctx     c;
  List           *new_rtable = NIL;
  List           *origins_l = NIL;
  List           *merged_quals = NIL;
  bool            any_flat = false, parent_flat = (probe->jointree != NULL);
  int             i, newpos = 0;
  ListCell       *lc;
  FlatAtomOrigin *origins;

  c.N            = N;
  c.slot_flat     = (bool *)  palloc0((N + 1) * sizeof(bool));
  c.parent_newpos = (int *)   palloc0((N + 1) * sizeof(int));
  c.sub_newpos    = (int **)  palloc0((N + 1) * sizeof(int *));
  c.sub_rtlen     = (int *)   palloc0((N + 1) * sizeof(int));
  c.sub_tl        = (Var ***) palloc0((N + 1) * sizeof(Var **));
  c.sub_tl_n      = (int *)   palloc0((N + 1) * sizeof(int));
  c.quals_mode    = false;
  c.quals_slot    = 0;

  if (parent_flat)
    foreach (lc, probe->jointree->fromlist)
      if (!IsA((Node *) lfirst(lc), RangeTblRef)) { parent_flat = false; break; }

  /* Layout pass: decide which slots inline, append base atoms / kept slots to
   * new_rtable, and record each new position's origin. */
  for (i = 1; parent_flat && i <= N; i++) {
    RangeTblEntry  *rte = list_nth_node(RangeTblEntry, probe->rtable, i - 1);
    Query          *sq;
    bool            ok;
    int             maxres = 0, b;
    ListCell       *lc2;
    Var           **tl;
    FlatAtomOrigin *sub_origins = NULL;
    int             sub_n = 0;

    if (!(rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL && !rte->lateral)) {
      newpos++;
      new_rtable = lappend(new_rtable, rte);
      c.parent_newpos[i] = newpos;
      origins_l = lappend(origins_l, flat_origin1(i));
      continue;
    }
    sq = rte->subquery;
    ok = !(sq->commandType != CMD_SELECT
           || sq->setOperations || sq->hasAggs || sq->hasWindowFuncs
           || sq->groupingSets || sq->groupClause || sq->havingQual
           || sq->distinctClause || sq->hasDistinctOn || sq->hasSubLinks
           || sq->limitCount || sq->limitOffset || sq->cteList
           || sq->jointree == NULL);
    if (ok)
      foreach (lc2, sq->jointree->fromlist)
        if (!IsA((Node *) lfirst(lc2), RangeTblRef)) { ok = false; break; }
    /* Recursively flatten this subquery's own SPJ subqueries first, so a
     * view-over-views collapses to base atoms before we inline it.  Mutates
     * sq (a node in the throwaway probe) in place; sub_origins maps sq's
     * flattened positions back to paths within sq, which we prepend our slot to
     * so the marker reaches the right base input through the nested rewrite. */
    if (ok)
      sub_origins = flatten_spj_subqueries(sq, &sub_n);
    /* every range-table entry a real base relation or a view artifact */
    if (ok) {
      int realbase = 0;
      foreach (lc2, sq->rtable) {
        RangeTblEntry *br = (RangeTblEntry *) lfirst(lc2);
        if (br->rtekind == RTE_RELATION && br->relkind == RELKIND_VIEW)
          continue;                          /* OLD/NEW placeholder */
        else if (br->rtekind == RTE_RELATION) realbase++;
        else { ok = false; break; }          /* join / nested subquery inside */
      }
      if (realbase < 1) ok = false;
    }
    /* non-junk target list entries all plain Vars on a base relation */
    if (ok)
      foreach (lc2, sq->targetList) {
        TargetEntry *te = (TargetEntry *) lfirst(lc2);
        if (!te->resjunk && te->resno > maxres) maxres = te->resno;
      }
    tl = ok ? (Var **) palloc0((maxres + 1) * sizeof(Var *)) : NULL;
    if (ok) {
      foreach (lc2, sq->targetList) {
        TargetEntry   *te = (TargetEntry *) lfirst(lc2);
        Var           *v;
        RangeTblEntry *br;
        if (te->resjunk) continue;
        if (!IsA(te->expr, Var)) { ok = false; break; }
        v = (Var *) te->expr;
        if (v->varlevelsup != 0
            || (int) v->varno < 1 || (int) v->varno > list_length(sq->rtable)) {
          ok = false; break;
        }
        br = list_nth_node(RangeTblEntry, sq->rtable, v->varno - 1);
        if (!(br->rtekind == RTE_RELATION && br->relkind != RELKIND_VIEW)) {
          ok = false; break;
        }
        tl[te->resno] = v;
      }
    }

    if (!ok) {
      /* not flattenable: keep the slot as-is (detector will reject it) */
      if (tl) pfree(tl);
      newpos++;
      new_rtable = lappend(new_rtable, rte);
      c.parent_newpos[i] = newpos;
      origins_l = lappend(origins_l, flat_origin1(i));
      continue;
    }

    /* inline: append each real base, assigning it a new flat position */
    c.slot_flat[i]  = true;
    c.sub_rtlen[i]  = list_length(sq->rtable);
    c.sub_newpos[i] = (int *) palloc0((c.sub_rtlen[i] + 1) * sizeof(int));
    c.sub_tl[i]     = tl;
    c.sub_tl_n[i]   = maxres;
    b = 0;
    foreach (lc2, sq->rtable) {
      RangeTblEntry *br = (RangeTblEntry *) lfirst(lc2);
      ++b;
      if (br->rtekind == RTE_RELATION && br->relkind != RELKIND_VIEW) {
        newpos++;
        new_rtable = lappend(new_rtable, copyObject(br));
        c.sub_newpos[i][b] = newpos;
        /* compose: our slot, then the base's path within the (already
         * recursively flattened) subquery -- so nested views map all the way
         * down to the base input. */
        origins_l = lappend(origins_l,
                            (sub_origins != NULL && b - 1 < sub_n)
                              ? flat_origin_prepend(i, &sub_origins[b - 1])
                              : flat_origin1(i));
      }
    }
    any_flat = true;
  }

  if (parent_flat && any_flat) {
    /* (1) remap the parent's target list and WHERE */
    probe->targetList = (List *) flatten_mut((Node *) probe->targetList, &c);
    if (probe->jointree->quals)
      merged_quals = lappend(merged_quals, flatten_mut(probe->jointree->quals, &c));
    /* (2) pull every inlined subquery's WHERE up, remapping base varnos */
    for (i = 1; i <= N; i++) {
      RangeTblEntry *rte;
      if (!c.slot_flat[i]) continue;
      rte = list_nth_node(RangeTblEntry, probe->rtable, i - 1);
      if (rte->subquery->jointree && rte->subquery->jointree->quals) {
        c.quals_mode = true; c.quals_slot = i;
        merged_quals =
          lappend(merged_quals,
                  flatten_mut((Node *) copyObject(rte->subquery->jointree->quals),
                              &c));
        c.quals_mode = false;
      }
    }
    /* (3) commit the flattened range table, a flat fromlist and combined WHERE */
    probe->rtable = new_rtable;
    {
      List *fl = NIL;
      for (i = 1; i <= newpos; i++) {
        RangeTblRef *r = makeNode(RangeTblRef);
        r->rtindex = i;
        fl = lappend(fl, r);
      }
      probe->jointree->fromlist = fl;
    }
    probe->jointree->quals =
      (merged_quals == NIL) ? NULL
      : (list_length(merged_quals) == 1) ? (Node *) linitial(merged_quals)
      : (Node *) makeBoolExpr(AND_EXPR, merged_quals, -1);

    *nflat_out = newpos;
    origins = (FlatAtomOrigin *) palloc(newpos * sizeof(FlatAtomOrigin));
    i = 0;
    foreach (lc, origins_l)
      origins[i++] = *(FlatAtomOrigin *) lfirst(lc);
    return origins;
  }

  /* Nothing flattened (no flattenable subquery, or a non-flat parent FROM):
   * leave probe untouched and return an identity map (one depth-1 path per slot). */
  *nflat_out = N;
  origins = (FlatAtomOrigin *) palloc(N * sizeof(FlatAtomOrigin));
  for (i = 0; i < N; i++) {
    origins[i].depth = 1;
    origins[i].path = (int *) palloc(sizeof(int));
    origins[i].path[0] = i + 1;
  }
  return origins;
}

/**
 * @brief Build the inversion-free marker context for top-level query @p q.
 *
 * Runs the detector on a flattened, group-RTE-stripped copy of @p q so that
 * single-base SPJ subqueries/views are recognised as base atoms.  On success
 * sets @p *cert_out to the serialised root certificate and returns a context
 * tree mirroring @p q's range table: a direct base atom's marker at its slot,
 * a flattened subquery's marker in a one-entry child context at its slot.
 * Returns NULL (declining) when @p q is not certified or carries no markers;
 * @p *cert_out may still be set (the cert attaches even without markers, and
 * the path then declines at evaluation and falls back).
 */
static InvFreeMarkerCtx *build_inversion_free_ctx(const constants_t *constants,
                                                  Query *q, char **cert_out) {
  bool            has_subq = false, has_group = false;
  Query          *probe;
  FlatAtomOrigin *origins = NULL;
  InvFreeMarker  *flat = NULL;
  int             nflat = 0, norigins = 0, N, p;
  char           *cert = NULL;
  InvFreeMarkerCtx *ctx;
  ListCell       *lc;

  foreach (lc, q->rtable)
    if (((RangeTblEntry *) lfirst(lc))->rtekind == RTE_SUBQUERY) has_subq = true;
#if PG_VERSION_NUM >= 180000
  has_group = q->hasGroupRTE;
#endif

  /* No subqueries and no synthetic group RTE: analyse q in place (read-only),
   * positions equal q's slots.  Otherwise work on a copy: strip the PG 18 group
   * RTE, then flatten SPJ subqueries (origins map flattened positions back). */
  if (!has_subq && !has_group) {
    probe = q;
  } else {
    probe = (Query *) copyObject(q);
#if PG_VERSION_NUM >= 180000
    if (has_group)
      strip_group_rte_pg18(probe);
#endif
    if (has_subq)
      origins = flatten_spj_subqueries(probe, &norigins);
  }

  if (!inversion_free_analyze(constants, probe, &cert, &flat, &nflat))
    return NULL;
  if (cert_out)
    *cert_out = cert;
  if (flat == NULL)              /* certified but no marker model: decline */
    return NULL;

  N = list_length(q->rtable);
  ctx = (InvFreeMarkerCtx *) palloc0(sizeof(InvFreeMarkerCtx));
  ctx->natoms  = N;
  ctx->markers = (InvFreeMarker *) palloc0((size_t) N * sizeof(InvFreeMarker));
  ctx->sub     = (InvFreeMarkerCtx **) palloc0((size_t) N * sizeof(InvFreeMarkerCtx *));
  /* Map each flattened atom's marker back to q by walking its origin slot path
   * down the *original* (un-flattened) query tree, creating/sizing a nested
   * child context at each subquery hop, so the recursive subquery rewrite later
   * threads the marker to the right base input.  With no flattening, position
   * == q slot (the synthetic group RTE, if any, sits after the base atoms, so
   * the prefix aligns), i.e. an implicit depth-1 path. */
  for (p = 0; p < nflat; p++) {
    int               tmp_path[1];
    int              *path;
    int               depth, d, base;
    InvFreeMarkerCtx *cur = ctx;
    Query            *qcur = q;
    if (!flat[p].valid)
      continue;
    if (origins != NULL) {
      if (p >= norigins) continue;
      path  = origins[p].path;
      depth = origins[p].depth;
    } else {
      tmp_path[0] = p + 1;
      path  = tmp_path;
      depth = 1;
    }
    /* descend all but the last path element (the nested subquery slots) */
    for (d = 0; d + 1 < depth && cur != NULL; d++) {
      int               slot = path[d];
      RangeTblEntry    *rte;
      InvFreeMarkerCtx *child;
      int               sublen;
      if (slot < 1 || slot > qcur->rtable->length) { cur = NULL; break; }
      rte = list_nth_node(RangeTblEntry, qcur->rtable, slot - 1);
      if (rte->rtekind != RTE_SUBQUERY || rte->subquery == NULL) { cur = NULL; break; }
      sublen = list_length(rte->subquery->rtable);
      child = cur->sub[slot - 1];
      if (child == NULL) {
        child = (InvFreeMarkerCtx *) palloc0(sizeof(InvFreeMarkerCtx));
        child->natoms  = sublen;
        child->markers = (InvFreeMarker *) palloc0((size_t) sublen * sizeof(InvFreeMarker));
        child->sub     = (InvFreeMarkerCtx **) palloc0((size_t) sublen * sizeof(InvFreeMarkerCtx *));
        cur->sub[slot - 1] = child;
      }
      cur  = child;
      qcur = rte->subquery;
    }
    if (cur == NULL)
      continue;
    base = path[depth - 1];                              /* base slot at the leaf */
    if (base >= 1 && base - 1 < cur->natoms)
      cur->markers[base - 1] = flat[p];
  }
  return ctx;
}

/**
 * @brief Rewrite a single SELECT query to carry provenance.
 *
 * This is the recursive entry point for the provenance rewriter.  It is
 * called from @c provsql_planner for top-level queries and re-entered from
 * @c get_provenance_attributes for subqueries in FROM.
 *
 * High-level steps:
 *  1. Strip any @c provsql column propagated into this query's target list.
 *  2. Detect and rewrite structural forms requiring pre-processing:
 *     non-ALL set operations (wrap in outer GROUP BY), AGG DISTINCT (push
 *     into a subquery), DISTINCT (convert to GROUP BY).
 *  3. Collect provenance attributes via @c get_provenance_attributes.
 *  4. Build a column-numbering map for where-provenance (@c build_column_map).
 *  5. Handle aggregates, migrate WHERE-on-aggregate to HAVING, and set ops.
 *  6. Build and splice the combined provenance expression.
 *
 * @param constants  Extension OID cache.
 * @param q          Query to rewrite (modified in place).
 * @param removed    Out-param: boolean array indicating which original target
 *                   list entries were provenance columns and were removed.
 *                   May be @c NULL if the caller does not need this info.
 * @param wrap_root  If true, mark this query's provenance expression as a
 *                   safe-query root that must be wrapped in
 *                   @c provsql.assume_boolean before splicing.
 * @param top_level  True for the outermost query the user evaluates; gates the
 *                   inversion-free analysis (run only at the top).
 * @param inv_ctx    Inversion-free marker context supplied by a parent that
 *                   flattened this query as a subquery, or @c NULL; when set,
 *                   this query applies the supplied per-input markers instead of
 *                   running its own analysis or read-once rewrite.
 * @return  The (possibly restructured) rewritten query, or @c NULL if the
 *          query has no FROM clause and can be skipped.
 */
static Query *process_query(const constants_t *constants, Query *q,
                            bool **removed, bool wrap_root, bool top_level,
                            const InvFreeMarkerCtx *inv_ctx) {
  List *prov_atts;
  bool has_union = false;
  bool has_difference = false;
  bool supported = true;
  bool group_by_rewrite = false;
  int nbcols = 0;
  int **columns;
  unsigned i = 0;
  char *inv_cert = NULL;            /* serialised inversion-free certificate (root) */
  const InvFreeMarkerCtx *local_inv_ctx = NULL; /* this query's marker context */
  if (provsql_verbose >= 50)
    elog_node_display(NOTICE, "ProvSQL: Before query rewriting", q, true);

  if (q->rtable == NULL) {
    /* FROM-less SELECT: the rest of the rewriter indexes into
     * q->rtable, so it can't process anything tied to a base relation.
     * But a WHERE-on-RV is still meaningful in this shape (e.g.
     *   SELECT 1 WHERE normal(0,1) > 2)
     * since the comparison produces a pure-rv gate that's lifted into
     * a synthesised provsql column on the single result row.  Run only
     * the qual migration + targetList splice and return; everything
     * else this function does (column mapping, set-ops, aggregation
     * rewriting, ...) assumes a non-empty rtable. */
    List *rv_cmps = migrate_probabilistic_quals(constants, q);
    if (rv_cmps != NIL) {
      Expr *provenance;
      RangeTblEntry *values_rte;
      RangeTblRef *rtr;
      Var *v;

      if (list_length(rv_cmps) == 1) {
        provenance = (Expr *)linitial(rv_cmps);
      } else {
        /* Multiple rv conjuncts: combine via provenance_times. */
        FuncExpr *times = makeNode(FuncExpr);
        ArrayExpr *array = makeNode(ArrayExpr);
        times->funcid = constants->OID_FUNCTION_PROVENANCE_TIMES;
        times->funcresulttype = constants->OID_TYPE_UUID;
        times->funcvariadic = true;
        times->location = -1;
        array->array_typeid = constants->OID_TYPE_UUID_ARRAY;
        array->element_typeid = constants->OID_TYPE_UUID;
        array->elements = rv_cmps;
        array->location = -1;
        times->args = list_make1(array);
        provenance = (Expr *)times;
      }

      /* Bind the lifted expression to a single evaluation by wrapping
       * it in a synthesized FROM (VALUES (<expr>)) AS _prov_(provsql).
       * Without this, multiple references to the same provenance
       * expression in the outer targetList (the user's provenance()
       * call, plus the auto-added provsql column) each re-invoke any
       * rv constructor inside, producing distinct UUIDs per call
       * because uniform / normal / ... mint a fresh leaf gate each
       * time.  Wrapping in VALUES gives one evaluation site that all
       * outer references read from. */
      values_rte = makeNode(RangeTblEntry);
      values_rte->rtekind = RTE_VALUES;
      values_rte->values_lists = list_make1(list_make1(provenance));
      values_rte->coltypes = list_make1_oid(constants->OID_TYPE_UUID);
      values_rte->coltypmods = list_make1_int(-1);
      values_rte->colcollations = list_make1_oid(InvalidOid);
      values_rte->eref = makeAlias(
        "_prov_",
        list_make1(makeString(pstrdup(PROVSQL_COLUMN_NAME))));
      values_rte->inh = false;
      values_rte->inFromCl = true;
#if PG_VERSION_NUM < 160000
      values_rte->requiredPerms = 0;
#endif
      q->rtable = list_make1(values_rte);

      rtr = makeNode(RangeTblRef);
      rtr->rtindex = 1;
      if (q->jointree == NULL) {
        q->jointree = makeNode(FromExpr);
      }
      q->jointree->fromlist = list_make1(rtr);

      v = makeVar(1, 1, constants->OID_TYPE_UUID, -1, InvalidOid, 0);

      /* Substitute any provenance() FuncExpr in the targetList with
       * a reference to the bound expression. */
      replace_provenance_function_by_expression(constants, q, (Expr *)v);

      /* Append a provsql column reading the same Var so callers that
       * expect the auto-added column find it. */
      {
        TargetEntry *te = makeTargetEntry(
          (Expr *)copyObject(v),
          list_length(q->targetList) + 1,
          pstrdup(PROVSQL_COLUMN_NAME),
          false);
        q->targetList = lappend(q->targetList, te);
      }
    }
    return q;
  }

  /* Inline non-recursive CTE references as subqueries so we can track
   * provenance through them. Must happen before set operation handling
   * since UNION/EXCEPT branches may reference CTEs.  Gated on
   * provsql.active: when provenance tracking is off the hook must stand
   * back and let the query plan as ordinary SQL -- it must not, in
   * particular, drive the recursive-CTE fixpoint (eval_recursive), which
   * runs SPI and creates temp tables at plan time. */
  if (provsql_active)
    inline_ctes(q);

  /* Decorrelate a top-level scalar subquery into a LEFT JOIN + choose() +
   * GROUP BY + count<=1 HAVING.  Runs before lower_outer_joins so the LEFT JOIN
   * it produces is lowered with correct outer-join provenance, and before the
   * "Subqueries not supported" guard further down. */
  if (provsql_active) {
    rewrite_array_sublinks(constants, q);
    rewrite_predicate_sublinks(constants, q);
    move_uncorrelated_sublinks_to_from(constants, q);
    decorrelate_scalar_sublinks(constants, q);
  }

  /* Lower a top-level outer JOIN (LEFT / RIGHT / FULL) of two base relations
   * into the UNION-ALL of its matched and null-padded antijoin arms, so the
   * non-monotone outer-join provenance (the 0-match world) is captured.  No-op
   * on every other shape.  Runs before provenance discovery / set-op handling
   * so the constructed UNION / EXCEPT subqueries are processed by the recursive
   * passes. */
  if (provsql_active)
    lower_outer_joins(constants, q);

  {
    Bitmapset *removed_sortgrouprefs = NULL;

    if (q->targetList) {
      removed_sortgrouprefs =
        remove_provenance_attributes_select(constants, q, removed);
      if (removed_sortgrouprefs != NULL)
        remove_provenance_attribute_groupref(q, removed_sortgrouprefs);
      if (q->setOperations)
        remove_provenance_attribute_setoperations(q, *removed);
    }
  }

  if(provsql_active) {
    columns = (int **)palloc(q->rtable->length * sizeof(int *));

    if (q->setOperations) {
      // TODO: Nest set operations as subqueries in FROM,
      // so that we only do set operations on base tables

      SetOperationStmt *stmt = (SetOperationStmt *)q->setOperations;
      if (!stmt->all) {
        /* Check if any branch has aggregates — non-ALL set operations
         * on aggregate results are not supported because agg_token
         * lacks comparison operators for deduplication */
        ListCell *lc_rte;
        foreach (lc_rte, q->rtable) {
          RangeTblEntry *rte = (RangeTblEntry *)lfirst(lc_rte);
          if (rte->rtekind == RTE_SUBQUERY && rte->subquery &&
              rte->subquery->hasAggs)
            provsql_error("Non-ALL set operations (UNION, EXCEPT) on "
                          "aggregate results not supported");
        }
        q = rewrite_non_all_into_external_group_by(q);
        return process_query(constants, q, removed, wrap_root, top_level, inv_ctx);
      }
    }

    if (q->hasAggs) {
      Query *rewritten = rewrite_agg_distinct(q, constants);
      if (rewritten)
        return process_query(constants, rewritten, removed, wrap_root, top_level,
                             inv_ctx);
    }

    /* Rewrite any JOIN on an agg_token column before provenance
     * discovery, so get_provenance_attributes sees the already-correct
     * subquery with a proper provsql column. */
    {
      Index rteid;
      AttrNumber join_attno;

      if (join_qual_has_agg_token((Node *)q->jointree, constants, &rteid,
                                  &join_attno))
      {
        Query *rewritten = rewrite_join_agg_token(q, constants, rteid, join_attno);
        if (rewritten)
          return process_query(constants, rewritten, removed, wrap_root,
                               top_level, inv_ctx);
      }
    }

    /* Opt-in safe-query optimisation slot: when on, try to rewrite
     * hierarchical conjunctive queries to a read-once form whose
     * probability is computable in linear time via independent
     * evaluation.  See try_safe_query_rewrite().
     *
     * The rewriter is gated on the presence of the assume_boolean()
     * helper (installed by the 1.6.0 upgrade script).  Without it we
     * cannot wrap the per-row root in a gate_assumed_boolean, which is
     * what downstream evaluators inspect to refuse unsound evaluation,
     * so we refuse to rewrite on schemas that still predate the
     * helper. */
    if (provsql_boolean_provenance && inv_ctx == NULL &&
        OidIsValid(constants->OID_FUNCTION_ASSUME_BOOLEAN)) {
      /* Read-once rewrite (an operation-mode change: it rewrites the query and
       * changes the produced circuit), so it is gated on boolean_provenance.
       * Skipped when @c inv_ctx is supplied: this query is an inlined subquery
       * whose base inputs must receive the parent's transparent order markers
       * (a no-op rewrite for the single-base projection it then is), not a
       * circuit-changing read-once rewrite that would bypass them. */
      Query *rewritten = try_safe_query_rewrite(constants, q);
      if (rewritten)
        return process_query(constants, rewritten, removed, true, top_level,
                             inv_ctx);
    }

    /* Inversion-free analysis is *not* an operation-mode change: it leaves the
     * lineage intact and only attaches a transparent certificate + per-input
     * order markers, read back at probability evaluation.  So it is decoupled
     * from boolean_provenance and gated on its own knob (provsql.inversion_free,
     * default on), run on THIS query — the one whose lineage we build — so the
     * certificate and markers align with the lineage by construction.  Only at
     * the outermost (top-level) root the user evaluates; never when the
     * read-once rewrite above already fired (that path returns early). */
    if (inv_ctx != NULL) {
      /* This query is an inlined subquery: the parent's flattened analysis
       * already produced our base-atom markers.  Apply them as-is; attach no
       * certificate here (the cert lives on the parent's per-row root). */
      local_inv_ctx = inv_ctx;
    } else if (top_level && provsql_inversion_free
               && OidIsValid(constants->OID_FUNCTION_ANNOTATE)) {
      /* Build the inversion-free marker context tree.  The detector runs on a
       * flattened copy (single-base SPJ subqueries / views inlined to their
       * base relation in place; on PG 18 the synthetic RTE_GROUP is stripped),
       * so the certificate and per-input order markers align with the lineage
       * by construction; the original q is left intact (only transparent
       * markers + a root certificate are added, read back at probability
       * evaluation).  The evaluator's size-bounded mismatch backstop declines
       * if any marker fails to land on its input. */
      local_inv_ctx = build_inversion_free_ctx(constants, q, &inv_cert);
    }

    /* Set difference (EXCEPT / EXCEPT ALL): group the right arm so the per-row
     * right provenances ⊕-combine before the monus, giving the paper's NOT-IN
     * semantics α ⊖ ⊕β.  Must run before get_provenance_attributes processes
     * the arms. */
    group_set_difference_right_arm(constants, q);

    // get_provenance_attributes will also recursively process subqueries
    // by calling process_query (threading each subquery's marker sub-context)
    prov_atts = get_provenance_attributes(constants, q, local_inv_ctx);

    /* Inversion-free path: wrap each certified atom's provenance token in its
     * per-input order marker.  prov_atts are base-relation Vars (the certified
     * class has only RTE_RELATION atoms and no agg/distinct/set-op restructuring,
     * so each Var's varno is still the atom's range-table index). */
    if (local_inv_ctx != NULL && local_inv_ctx->markers != NULL)
      wrap_inversion_free_markers(constants, q, prov_atts,
                                  local_inv_ctx->markers, local_inv_ctx->natoms);

    if (prov_atts == NIL) {
      /* If the WHERE clause contains a random_variable comparison, we
       * still need to take the rewriting path so the result tuple
       * carries the comparator's gate_cmp UUID as its provenance.
       * Synthesize a single gate_one() prov_att; the combination
       * provenance_times(one, rv_cmp) collapses to rv_cmp downstream
       * because gate_one is the multiplicative identity. */
      if (q->jointree && q->jointree->quals &&
          expr_contains_rv_cmp(q->jointree->quals, constants)) {
        FuncExpr *one_expr = makeNode(FuncExpr);
        one_expr->funcid = constants->OID_FUNCTION_GATE_ONE;
        one_expr->funcresulttype = constants->OID_TYPE_UUID;
        one_expr->args = NIL;
        one_expr->location = -1;
        prov_atts = list_make1(one_expr);
      } else {
        return q;
      }
    }

    if (q->hasSubLinks) {
      provsql_error("Subqueries (EXISTS, IN, scalar subquery) not supported");
      supported = false;
    }

    if (supported && q->distinctClause) {
      if (q->hasDistinctOn) {
        provsql_error("DISTINCT ON not supported");
        supported = false;
      } else if (q->hasAggs) {
        provsql_error("DISTINCT on aggregate results not supported");
      } else if (list_length(q->distinctClause) < list_length(q->targetList)) {
        provsql_error("Inconsistent DISTINCT and GROUP BY clauses not "
                      "supported");
        supported = false;
      } else {
        transform_distinct_into_group_by(q);
      }
    }

    if (supported && q->setOperations) {
      SetOperationStmt *stmt = (SetOperationStmt *)q->setOperations;

      if (stmt->op == SETOP_UNION) {
        process_set_operation_union(constants, stmt, q);
        has_union = true;
      } else if (stmt->op == SETOP_EXCEPT) {
        if (!transform_except_into_join(constants, q))
          supported = false;
        has_difference = true;
      } else {
        provsql_error("Set operations other than UNION and EXCEPT not "
                      "supported");
        supported = false;
      }
    }

    if (supported && q->groupClause &&
        !provenance_function_in_group_by(constants, q)) {
      group_by_rewrite = true;
    }

    if (supported && q->groupingSets) {
      if (q->groupClause || list_length(q->groupingSets) > 1 ||
          ((GroupingSet *)linitial(q->groupingSets))->kind !=
          GROUPING_SET_EMPTY) {
        provsql_error("GROUPING SETS, CUBE, and ROLLUP not supported");
        supported = false;
      } else {
        // Simple GROUP BY ()
        group_by_rewrite = true;
      }
    }

    if (supported)
      build_column_map(q, columns, &nbcols);

    if (supported) {
      Expr *provenance;
      List *rv_cmps;

      /* Window functions are not supported: their per-row result has no
       * aggregate-provenance semantics.  The query still executes and each
       * output row carries its input row's tuple provenance, but the
       * windowed computation itself (e.g. SUM() OVER ...) is an opaque
       * scalar, not an agg_token.  Warn once per rewritten query level that
       * actually involves provenance-tracked relations. */
      if (q->hasWindowFuncs)
        provsql_warning("window functions are not supported; provenance is "
                        "tracked per input row only, and the windowed "
                        "computation is treated as an opaque scalar");

      /* Single unified pass over WHERE: each top-level conjunct is
       * routed to the right evaluation site (HAVING for agg_token,
       * the returned rv_cmps list for random_variable, left in WHERE
       * otherwise).  Mixed shapes raise a clear error.  Replaces the
       * historical pair migrate_aggtoken_quals_to_having +
       * extract_rv_cmps_from_quals; see the qual_class doc above for
       * the routing matrix.
       *
       * Must run before replace_aggregations_by_provenance_aggregate
       * so the lifted RV cmps factor into each row's contribution to
       * any surrounding agg_token: otherwise the cmp lands at group
       * level with row-typed Vars the executor cannot resolve, or
       * gets discarded by the HAVING-replaces-result branch of
       * make_provenance_expression.
       *
       * Skipped for SR_PLUS / SR_MONUS (UNION / EXCEPT outer level):
       * each branch is rewritten by its own recursive process_query
       * call, so an outer-level WHERE on RV here is exotic; the
       * fallback after make_provenance_expression handles it. */
      rv_cmps = migrate_probabilistic_quals(constants, q);
      if (rv_cmps != NIL && !has_union && !has_difference) {
        prov_atts = list_concat(prov_atts, rv_cmps);
        rv_cmps = NIL;
      }

      if (q->hasAggs) {
        ListCell *lc_sort;

        // Compute aggregation expressions
        replace_aggregations_by_provenance_aggregate(
          constants, q, prov_atts,
          has_union ? SR_PLUS : (has_difference ? SR_MONUS : SR_TIMES));

        // If there are any sort clauses on something whose type is now
        // aggregate token, we throw an error: sorting aggregation values
        // when provenance is captured is ill-defined
        foreach (lc_sort, q->sortClause) {
          SortGroupClause *sort = (SortGroupClause *)lfirst(lc_sort);
          ListCell *lc_te;
          foreach (lc_te, q->targetList) {
            TargetEntry *te = (TargetEntry *)lfirst(lc_te);
            if (sort->tleSortGroupRef == te->ressortgroupref) {
              if (exprType((Node *)te->expr) == constants->OID_TYPE_AGG_TOKEN)
                provsql_error("ORDER BY on the result of an aggregate function is "
                              "not supported");
              break;
            }
          }
        }
      }

      /* Insert casts for agg_token Vars used in arithmetic or window
       * functions, now that WHERE-to-HAVING migration is done */
      insert_agg_token_casts(constants, q);

      provenance = make_provenance_expression(
        constants, q, prov_atts, q->hasAggs, group_by_rewrite,
        has_union ? SR_PLUS : (has_difference ? SR_MONUS : SR_TIMES), columns,
        nbcols, wrap_root, inv_cert);

      /* Fallback for the rare set-op outer WHERE case: conjoin via
       * provenance_times after the aggregation wrappers.  Correct only
       * when no aggregation collapses rows above this point. */
      if (rv_cmps != NIL) {
        FuncExpr *times = makeNode(FuncExpr);
        ArrayExpr *array = makeNode(ArrayExpr);
        times->funcid = constants->OID_FUNCTION_PROVENANCE_TIMES;
        times->funcresulttype = constants->OID_TYPE_UUID;
        times->funcvariadic = true;
        times->location = -1;
        array->array_typeid = constants->OID_TYPE_UUID_ARRAY;
        array->element_typeid = constants->OID_TYPE_UUID;
        array->elements = lcons(provenance, rv_cmps);
        array->location = -1;
        times->args = list_make1(array);
        provenance = (Expr *)times;
      }

      add_to_select(q, provenance);
      replace_provenance_function_by_expression(constants, q, provenance);

      if (has_difference)
        add_select_non_zero(constants, q, provenance);
    }

    for (i = 0; i < q->rtable->length; ++i) {
      if (columns[i])
        pfree(columns[i]);
    }
  }

  if (provsql_verbose >= 50)
    elog_node_display(NOTICE, "ProvSQL: After query rewriting", q, true);

  return q;
}

/* -------------------------------------------------------------------------
 * INSERT ... SELECT provenance propagation
 * ------------------------------------------------------------------------- */

/**
 * @brief Propagate provenance through INSERT ... SELECT.
 *
 * If the source SELECT involves provenance-tracked tables and the target
 * table has a provsql column, rewrites the source SELECT to carry
 * provenance and maps its provsql output to the target's provsql column,
 * replacing the default uuid_generate_v4().
 *
 * If the target has no provsql column, emits a warning instead.
 */
static void process_insert_select(const constants_t *constants, Query *q) {
  ListCell *lc;
  Index src_rteid = 0;
  RangeTblEntry *src_rte = NULL;
  RangeTblEntry *tgt_rte;
  AttrNumber provsql_attno = 0;
  TargetEntry *provsql_te = NULL;
  bool provsql_te_is_new = false;

  /* Find the source SELECT subquery with provenance */
  foreach (lc, q->rtable) {
    RangeTblEntry *r = (RangeTblEntry *)lfirst(lc);
    ++src_rteid;
    if (r->rtekind == RTE_SUBQUERY && r->subquery &&
        has_provenance(constants, r->subquery)) {
      src_rte = r;
      break;
    }
  }

  if (src_rte == NULL)
    return;

  /* Rewrite the source SELECT so its own provenance semantics -- HAVING
   * lifting, provenance() resolution -- take effect.  This must run whether or
   * not the target table is provenance-tracked: the old code returned early
   * (warning, below) when the target had no provsql column, which left the
   * SELECT's HAVING on the physical rows and provenance() unresolved, so the
   * INSERT saw zero rows. */
  {
    bool *removed = NULL;
    Query *new_subquery =
      process_query(constants, src_rte->subquery, &removed, false, false, NULL);
    if (new_subquery == NULL)
      return;
    src_rte->subquery = new_subquery;
  }

  /* Check if the target table has a provsql column */
  tgt_rte = list_nth_node(RangeTblEntry, q->rtable, q->resultRelation - 1);
  if (tgt_rte->rtekind == RTE_RELATION) {
    AttrNumber attid = 1;
    foreach (lc, tgt_rte->eref->colnames) {
      if (!strcmp(strVal(lfirst(lc)), PROVSQL_COLUMN_NAME) &&
          get_atttype(tgt_rte->relid, attid) == constants->OID_TYPE_UUID)
        provsql_attno = attid;
      ++attid;
    }
  }

  if (provsql_attno == 0) {
    /* The target cannot store provenance.  The source SELECT was rewritten
     * above (so it returns the right rows), but its auto-added provsql column
     * has no target column to land in -- drop it so the INSERT's column
     * mapping stays consistent, and warn that provenance is not propagated. */
    remove_provsql_from_select(src_rte->subquery);
    provsql_warning("INSERT ... SELECT on provenance-tracked "
                    "tables: source provenance is not propagated "
                    "to inserted rows");
    return;
  }

  /* Find the provsql target entry and verify it's a UUID default */
  foreach (lc, q->targetList) {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    if (te->resno == provsql_attno &&
        exprType((Node *)te->expr) == constants->OID_TYPE_UUID) {
      provsql_te = te;
      break;
    }
  }

  if (provsql_te == NULL) {
    /* The target's provsql column is not in the INSERT's targetList
     * (no DEFAULT on the column since 1.6.0; the user did not name
     * the column either).  Synthesise a TE so we have something to
     * substitute the source provsql Var into below. */
    provsql_te = makeNode(TargetEntry);
    provsql_te->resno = provsql_attno;
    provsql_te->resname = pstrdup(PROVSQL_COLUMN_NAME);
    provsql_te_is_new = true;
  }

  /* Map the source's provsql column into the target's provsql column. */
  {
    AttrNumber src_provsql_attno = 0;

    foreach (lc, src_rte->subquery->targetList) {
      TargetEntry *te = (TargetEntry *)lfirst(lc);
      if (te->resname && !strcmp(te->resname, PROVSQL_COLUMN_NAME) &&
          exprType((Node *)te->expr) == constants->OID_TYPE_UUID) {
        src_provsql_attno = te->resno;
        break;
      }
    }

    if (src_provsql_attno == 0)
      return;

    /* Replace the target's provsql default with a Var from the source */
    {
      Var *v = makeNode(Var);
      v->varno = src_rteid;
      v->varattno = src_provsql_attno;
      v->vartype = constants->OID_TYPE_UUID;
      v->vartypmod = -1;
      v->varcollid = InvalidOid;
      v->location = -1;
      provsql_te->expr = (Expr *)v;
    }

    /* Now that its expr is set, splice a freshly synthesised provsql
     * target entry into the INSERT's targetList. */
    if (provsql_te_is_new)
      q->targetList = lappend(q->targetList, provsql_te);

    /* Update the subquery RTE's column names to include provsql */
    src_rte->eref->colnames = lappend(src_rte->eref->colnames,
                                      makeString(pstrdup(PROVSQL_COLUMN_NAME)));
  }
}

/* -------------------------------------------------------------------------
 * Planner hook & extension lifecycle
 * ------------------------------------------------------------------------- */

/**
 * @brief PostgreSQL planner hook — entry point for provenance rewriting.
 *
 * Replaces (or chains after) the standard planner.  For every CMD_SELECT
 * that involves at least one provenance-bearing relation or an explicit
 * @c provenance() call, rewrites the query via @c process_query before
 * handing the result to the standard planner.  Non-SELECT commands and
 * queries without provenance are passed through unchanged.
 * @param q              The query to plan.
 * @param cursorOptions  Cursor options bitmask.
 * @param boundParams    Pre-bound parameter values.
 * @return               The planned statement.
 */
/**
 * @brief Walker: true if any @c Query in the tree defines a @c provsql column
 *        by hand.
 *
 * A non-junk target entry resnamed @c provsql whose expression is not a
 * legitimate uuid-typed @c Var (the passthrough of a tracked relation's
 * provsql column, which @c remove_provenance_attributes_select strips) is a
 * hand-made provenance column -- e.g. @c "provenance() AS provsql" or
 * @c "expr AS provsql".  It collides with the provenance column ProvSQL adds
 * itself: the output column count desyncs and a later @c Var mis-binds to a
 * non-uuid column, crashing @c get_gate_type when it dereferences the value as
 * a pointer.
 *
 * Run once on the user's ORIGINAL query in the planner hook, before any
 * rewriting, so the intermediate queries ProvSQL builds (which legitimately
 * carry a provsql column) are never visited.
 */
static bool query_defines_handmade_provsql(Node *node, void *cx) {
  const constants_t *constants = (const constants_t *)cx;
  if (node == NULL)
    return false;
  if (IsA(node, Query)) {
    Query *q = (Query *)node;
    ListCell *lc;
    foreach (lc, q->targetList) {
      TargetEntry *te = (TargetEntry *)lfirst(lc);
      if (te->resjunk || te->resname == NULL ||
          strcmp(te->resname, PROVSQL_COLUMN_NAME))
        continue;
      if (IsA(te->expr, Var) &&
          ((Var *)te->expr)->vartype == constants->OID_TYPE_UUID)
        continue; /* legitimate passthrough of a real provsql column */
      return true;
    }
    return query_tree_walker(q, query_defines_handmade_provsql, cx, 0);
  }
  return expression_tree_walker(node, query_defines_handmade_provsql, cx);
}

/** @brief Executor nesting depth.
 *
 * Tracks how deep we are inside @c Executor invocations.  Incremented
 * in @c provsql_executor_start, decremented in @c provsql_executor_end.
 * The classifier @c NOTICE only fires when this is zero, which
 * corresponds to the user's outermost statement being planned (before
 * any executor entry).  Plans built for PL/pgSQL function bodies that
 * the rewriter inserts -- @c provenance_times, @c provenance_plus,
 * @c provenance_aggregate, ... -- happen during execution of the
 * user's plan, so they see depth >= 1 and skip the NOTICE. */
static int provsql_executor_depth = 0;

static PlannedStmt *provsql_planner(Query *q,
#if PG_VERSION_NUM >= 130000
                                    const char *query_string,
#endif
                                    int cursorOptions,
                                    ParamListInfo boundParams) {
  if (q->commandType == CMD_INSERT && q->rtable && provsql_active) {
    const constants_t constants = get_constants(false);
    if (constants.ok) {
      if (provenance_in_sublink_walker((Node *)q, (void *)&constants))
        provsql_error("a subquery over a provenance-tracked relation cannot be "
                      "used as a scalar subquery / IN / EXISTS expression; put "
                      "it in the FROM clause instead");
      process_insert_select(&constants, q);
    }
  } else if (q->commandType == CMD_SELECT) {
    /* No rtable check here: a FROM-less SELECT (e.g.
     *   SELECT 1 WHERE normal(0,1) > 2)
     * still needs the hook to engage when the WHERE contains an
     * rv_cmp.  has_provenance walks the tree and returns false fast
     * on FROM-less queries that have neither rv_cmp nor provenance(),
     * so widening the gate costs nothing in the common case. */
    const constants_t constants = get_constants(false);

    /* A subquery over a provenance-tracked relation used in an expression
     * context (scalar subquery / IN / EXISTS) is not supported -- and would
     * otherwise slip past has_provenance() (which does not descend into
     * SubLinks) and leave provenance() to fail at runtime.  Flag it clearly. */
    if (provsql_active && constants.ok &&
        provenance_in_sublink_walker((Node *)q, (void *)&constants))
      provsql_error("a subquery over a provenance-tracked relation cannot be "
                    "used as a scalar subquery / IN / EXISTS expression; put "
                    "it in the FROM clause instead");

    /* Query-time TID / BID / OPAQUE classifier.  Emits a NOTICE for
     * the user's outermost SELECT when the GUC is on.  Runs on the
     * user's original Query before any provsql rewriting so the
     * reported kind reflects the SQL the user wrote.  Gating on
     * @c provsql_executor_depth @c == @c 0 skips the spurious extra
     * planning calls triggered by PL/pgSQL function bodies the
     * rewriter inserts (@c provenance_times, ...), whose internal
     * SELECTs go through the planner hook during execution of the
     * user's plan. */
    if (provsql_executor_depth == 0
        && provsql_classify_top_level
        && q->rtable != NIL) {
      ProvSQLClassification cls;
      provsql_classify_query(q, &cls);
      provsql_classify_emit_notice(&cls);
      list_free(cls.source_relids);
    }

    /* HAVING-trichotomy classifier: a NOTICE per HAVING aggregate
     * comparison labelling it safe / apx-safe / #P-hard / open.  Same
     * read-only, top-level-only gating as classify_top_level. */
    if (provsql_executor_depth == 0
        && provsql_classify_having
        && constants.ok)
      provsql_emit_having_classification(q, &constants);

    if (constants.ok && has_provenance(&constants, q)) {
      bool *removed = NULL;
      Query *new_query;
      clock_t begin = 0;

      /* A user query may not define its own provsql column by hand; ProvSQL
       * manages the provenance column itself.  Checked here, once, on the
       * original query before any rewriting -- so the intermediate queries the
       * rewriter builds (which legitimately carry a provsql column) are not
       * flagged. */
      if (provsql_active &&
          query_defines_handmade_provsql((Node *)q, (void *)&constants))
        provsql_error("a query may not define a column named \"%s\" by hand; "
                      "ProvSQL manages the provenance column itself",
                      PROVSQL_COLUMN_NAME);

#if PG_VERSION_NUM >= 150000
      if (provsql_verbose >= 20)
        provsql_notice("Main query before query rewriting:\n%s\n",
                       pg_get_querydef(q, true));
#endif

      if (provsql_verbose >= 40)
        begin = clock();

      new_query = process_query(&constants, q, &removed, false, true, NULL);

      if (provsql_verbose >= 40)
        provsql_notice("planner time spent=%f",
                       (double)(clock() - begin) / CLOCKS_PER_SEC);

      if (new_query != NULL)
        q = new_query;

#if PG_VERSION_NUM >= 150000
      if (provsql_verbose >= 20)
        provsql_notice("Main query after query rewriting:\n%s\n",
                       pg_get_querydef(q, true));
#endif
    }
  }

  if (prev_planner)
    return prev_planner(q,
#if PG_VERSION_NUM >= 130000
                        query_string,
#endif
                        cursorOptions, boundParams);
  else
    return standard_planner(q,
#if PG_VERSION_NUM >= 130000
                            query_string,
#endif
                            cursorOptions, boundParams);
}

/* -------------------------------------------------------------------------
 * Executor hooks (depth tracking only)
 *
 * We install ExecutorStart / ExecutorEnd hooks solely to maintain
 * @c provsql_executor_depth, which the classifier in @c provsql_planner
 * consults to distinguish the user's outermost statement from nested
 * PL/pgSQL bodies the rewriter calls into.  No other behaviour changes.
 * ------------------------------------------------------------------------- */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type   prev_ExecutorEnd   = NULL;

static void provsql_executor_start(QueryDesc *queryDesc, int eflags) {
  provsql_executor_depth++;
  PG_TRY();
  {
    if (prev_ExecutorStart)
      prev_ExecutorStart(queryDesc, eflags);
    else
      standard_ExecutorStart(queryDesc, eflags);
  }
  PG_CATCH();
  {
    provsql_executor_depth--;
    PG_RE_THROW();
  }
  PG_END_TRY();
}

static void provsql_executor_end(QueryDesc *queryDesc) {
#if PG_VERSION_NUM >= 130000
  PG_TRY();
  {
    if (prev_ExecutorEnd)
      prev_ExecutorEnd(queryDesc);
    else
      standard_ExecutorEnd(queryDesc);
  }
  PG_FINALLY();
  {
    provsql_executor_depth--;
  }
  PG_END_TRY();
#else
  /* PG < 13 lacks PG_FINALLY: emulate by running the cleanup on the
   * error path (via PG_CATCH + PG_RE_THROW) and on the success path
   * (after PG_END_TRY).  Functionally equivalent. */
  PG_TRY();
  {
    if (prev_ExecutorEnd)
      prev_ExecutorEnd(queryDesc);
    else
      standard_ExecutorEnd(queryDesc);
  }
  PG_CATCH();
  {
    provsql_executor_depth--;
    PG_RE_THROW();
  }
  PG_END_TRY();
  provsql_executor_depth--;
#endif
}

/* -------------------------------------------------------------------------
 * ProcessUtility hook: CTAS lineage inheritance.
 *
 * When a @c CREATE @c TABLE @c AS (or @c CREATE @c MATERIALIZED @c VIEW,
 * or @c SELECT @c INTO -- PG's parser transforms all three into
 * @c CreateTableAsStmt) projects a @c provsql column lifted verbatim
 * from a tracked source, the resulting relation's atoms are not freshly
 * minted UUIDs but lineage tokens of one or more base @c
 * add_provenance / @c repair_key relations.  The hook intercepts the
 * utility statement, classifies the inner @c SELECT via
 * @c provsql_classify_query, lets PG run the CTAS, then populates
 * @c provsql_table_info (with the inherited @c kind / BID @c block_key)
 * and the ancestor registry (with the transitive union of source
 * ancestor sets) on the just-created relation.  A @c provenance_guard
 * trigger is installed on the new table so any subsequent INSERT /
 * UPDATE that supplies a non-NULL @c provsql still flips the table to
 * OPAQUE the standard way.
 *
 * The hook deliberately fires only when the inner @c SELECT projects
 * a @c provsql column from a tracked source -- otherwise the new
 * relation has no @c provsql column and the lineage metadata would be
 * operationally pointless.  Users who want a tracked CTAS-derived
 * table without inherited lineage still call @c add_provenance on it
 * afterwards (that path seeds @c {self} and overrides whatever this
 * hook may have recorded).
 * ------------------------------------------------------------------------- */

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/** @brief State captured by the pre-execution pass for the post-execution one. */
typedef struct ProvSQLCtasCapture {
  bool                fire;             ///< true when the post-pass should run
  Query              *inner_query;      ///< cloned for safety; freed by pfree on completion
  provsql_table_kind  inherited_kind;
  uint16              ancestor_n;
  Oid                 ancestors[PROVSQL_TABLE_INFO_MAX_ANCESTORS];
  Oid                 source_relid;     ///< Single source whose block_key we want to align (BID only)
  uint16              source_block_key_n;
  AttrNumber          source_block_key[PROVSQL_TABLE_INFO_MAX_BLOCK_KEY];
} ProvSQLCtasCapture;

/**
 * @brief Decide whether @p parsetree is a CTAS that should trigger
 *        the ancestry hook, and if so populate @p cap with the inner
 *        classification, the (single) source's block-key columns, and
 *        the transitive ancestor union.
 *
 * Fires only when the inner @c SELECT's target list projects a base-
 * level @c Var (possibly through @c RelabelType wrappers) that
 * resolves to the @c provsql column of an @c RTE_RELATION whose
 * metadata is non-OPAQUE.  Anything else (no @c provsql in the
 * projection, classifier says OPAQUE, the projected source is itself
 * OPAQUE) leaves @c cap->fire false and the post-pass becomes a
 * no-op.
 */
static void provsql_ProcessUtility_capture(Node *parsetree,
                                           ProvSQLCtasCapture *cap) {
  CreateTableAsStmt    *stmt;
  Query                *qry;
  ProvSQLClassification cls;
  ListCell             *lc;
  AttrNumber            prov_resno = InvalidAttrNumber;
  Oid                   source_relid = InvalidOid;
  ProvenanceTableInfo   source_info;
  Bitmapset            *ancestor_bms = NULL;
  int                   bms_member;
  uint16                ancestor_n;

  cap->fire = false;
  if (!provsql_active)
    return;
  if (parsetree == NULL || !IsA(parsetree, CreateTableAsStmt))
    return;
  stmt = (CreateTableAsStmt *) parsetree;
  if (stmt->query == NULL || !IsA(stmt->query, Query))
    return;
  qry = (Query *) stmt->query;
  if (qry->commandType != CMD_SELECT)
    return;

  provsql_classify_query(qry, &cls);
  if (cls.kind == PROVSQL_TABLE_OPAQUE) {
    list_free(cls.source_relids);
    return;
  }

  /* Walk the inner target list for a TLE whose Var resolves to the
   * provsql column of a tracked, non-OPAQUE source.  First match wins
   * (CTAS preserves the TLE's column name verbatim in the new
   * table, so a single provsql TLE is the normal case). */
  foreach (lc, qry->targetList) {
    TargetEntry   *te = (TargetEntry *) lfirst(lc);
    Node          *e  = (Node *) te->expr;
    Var           *v;
    RangeTblEntry *rte;
    AttrNumber     prov_attno;

    if (te->resjunk)
      continue;
    while (e != NULL && IsA(e, RelabelType))
      e = (Node *) ((RelabelType *) e)->arg;
    if (e == NULL || !IsA(e, Var))
      continue;
    v = (Var *) e;
    if (v->varlevelsup != 0)
      continue;
    if (v->varno < 1 || (int) v->varno > list_length(qry->rtable))
      continue;
    rte = (RangeTblEntry *) list_nth(qry->rtable, v->varno - 1);
    if (rte->rtekind != RTE_RELATION)
      continue;
    prov_attno = get_attnum(rte->relid, PROVSQL_COLUMN_NAME);
    if (prov_attno == InvalidAttrNumber || v->varattno != prov_attno)
      continue;
    if (!provsql_lookup_table_info(rte->relid, &source_info))
      continue;
    if (source_info.kind == PROVSQL_TABLE_OPAQUE)
      continue;
    prov_resno   = te->resno;
    source_relid = rte->relid;
    break;
  }
  if (prov_resno == InvalidAttrNumber) {
    list_free(cls.source_relids);
    return;
  }

  /* Transitive ancestor union: lookup each classifier-reported source's
   * registered ancestry, fall back to {source} when none recorded
   * (defensive: the SQL add_provenance / repair_key seed should always
   * give us a non-empty set).  Bitmapset dedupes; we then walk it in
   * ascending order to get the sorted Oid array the registry stores. */
  foreach (lc, cls.source_relids) {
    Oid src_relid = lfirst_oid(lc);
    uint16 src_n;
    Oid    src_ancestors[PROVSQL_TABLE_INFO_MAX_ANCESTORS];
    if (provsql_lookup_ancestry(src_relid, &src_n, src_ancestors)) {
      for (uint16 i = 0; i < src_n; ++i)
        ancestor_bms = bms_add_member(ancestor_bms, (int) src_ancestors[i]);
    } else {
      ancestor_bms = bms_add_member(ancestor_bms, (int) src_relid);
    }
  }
  list_free(cls.source_relids);

  ancestor_n = 0;
  bms_member = -1;
  while ((bms_member = bms_next_member(ancestor_bms, bms_member)) >= 0) {
    if (ancestor_n >= PROVSQL_TABLE_INFO_MAX_ANCESTORS) {
      /* Cap exceeded: refuse to fire rather than truncate the ancestor
       * set silently (a partial set would let the safe-query
       * disjointness check accept a join that shouldn't be safe). */
      bms_free(ancestor_bms);
      return;
    }
    cap->ancestors[ancestor_n++] = (Oid) bms_member;
  }
  bms_free(ancestor_bms);

  cap->fire               = true;
  cap->inner_query        = qry;
  cap->inherited_kind     = (provsql_table_kind) cls.kind;
  cap->ancestor_n         = ancestor_n;
  cap->source_relid       = source_relid;
  cap->source_block_key_n = source_info.block_key_n;
  memcpy(cap->source_block_key, source_info.block_key,
         source_info.block_key_n * sizeof(AttrNumber));
}

/** @brief Map @c provsql_table_kind to its textual label
 *  (@c set_table_info accepts text). */
static const char *provsql_ctas_kind_label(provsql_table_kind k) {
  switch (k) {
    case PROVSQL_TABLE_TID:    return "tid";
    case PROVSQL_TABLE_BID:    return "bid";
    case PROVSQL_TABLE_OPAQUE: return "opaque";
  }
  return "opaque";
}

/** @brief Forward declaration of the C SQL entry points. */
extern Datum set_table_info(PG_FUNCTION_ARGS);
extern Datum set_ancestors(PG_FUNCTION_ARGS);

/**
 * @brief Apply @p cap to the freshly-created relation @c stmt->into->rel.
 *
 * For BID sources: walks the inner query's target list to align each
 * source block-key column to its output @c resno.  If any block-key
 * column is missing from the projection (the CTAS dropped it), the
 * new relation cannot honour the BID invariant under that column --
 * the hook demotes to TID rather than asserting a now-stale block
 * key.
 *
 * Installs @c provenance_guard via SPI so subsequent INSERT /
 * UPDATE OF provsql on the new relation flip its kind to OPAQUE
 * through the standard guard path.
 */
static void provsql_ProcessUtility_apply(Node *parsetree,
                                         ProvSQLCtasCapture *cap) {
  CreateTableAsStmt *stmt;
  Oid                new_relid;
  AttrNumber         prov_attno;
  provsql_table_kind eff_kind;
  uint16             eff_block_key_n = 0;
  AttrNumber         eff_block_key[PROVSQL_TABLE_INFO_MAX_BLOCK_KEY];
  Datum              kind_datum;
  Datum              block_key_datum;
  Datum              ancestors_datum;
  Datum             *block_key_elems;
  Datum             *ancestor_elems;
  ArrayType         *block_key_arr;
  ArrayType         *ancestors_arr;
  const char        *nspname;
  const char        *relname;
  StringInfoData     trigger_sql;

  if (!cap->fire)
    return;
  stmt = (CreateTableAsStmt *) parsetree;

  new_relid = RangeVarGetRelid(stmt->into->rel, NoLock, true);
  if (new_relid == InvalidOid)
    return;

  /* Confirm the new relation actually has a @c provsql @c uuid column.
   * CTAS preserves TLE column names, so this is essentially the
   * post-execution verification of what the pre-pass already
   * required. */
  prov_attno = get_attnum(new_relid, PROVSQL_COLUMN_NAME);
  if (prov_attno == InvalidAttrNumber)
    return;
  if (get_atttype(new_relid, prov_attno) != UUIDOID)
    return;

  /* BID block-key alignment: each source block-key column must
   * survive in the inner-query target list.  When all do, the new
   * relation's effective block key is the corresponding output
   * resno.  When any is missing, demote to TID (a partial block
   * key would falsely advertise mutual exclusion the rows no
   * longer have). */
  eff_kind = cap->inherited_kind;
  if (eff_kind == PROVSQL_TABLE_BID) {
    bool ok = true;
    for (uint16 i = 0; i < cap->source_block_key_n; ++i) {
      AttrNumber src_attno = cap->source_block_key[i];
      ListCell  *lc;
      bool       found = false;
      foreach (lc, cap->inner_query->targetList) {
        TargetEntry   *te = (TargetEntry *) lfirst(lc);
        Node          *e  = (Node *) te->expr;
        Var           *v;
        RangeTblEntry *rte;
        if (te->resjunk)
          continue;
        while (e != NULL && IsA(e, RelabelType))
          e = (Node *) ((RelabelType *) e)->arg;
        if (e == NULL || !IsA(e, Var))
          continue;
        v = (Var *) e;
        if (v->varlevelsup != 0)
          continue;
        if (v->varno < 1
            || (int) v->varno > list_length(cap->inner_query->rtable))
          continue;
        rte = (RangeTblEntry *)
            list_nth(cap->inner_query->rtable, v->varno - 1);
        if (rte->rtekind != RTE_RELATION)
          continue;
        if (rte->relid == cap->source_relid
            && v->varattno == src_attno) {
          if (eff_block_key_n >= PROVSQL_TABLE_INFO_MAX_BLOCK_KEY) {
            ok = false;
            break;
          }
          eff_block_key[eff_block_key_n++] = te->resno;
          found = true;
          break;
        }
      }
      if (!found) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      eff_kind        = PROVSQL_TABLE_TID;
      eff_block_key_n = 0;
    }
  }

  /* Marshal arguments and invoke the SQL-level helpers via
   * DirectFunctionCall: this reaches the worker through the same IPC
   * path the user-facing SQL functions use, including the relcache
   * invalidation broadcast on the way out. */
  kind_datum = CStringGetTextDatum(provsql_ctas_kind_label(eff_kind));
  if (eff_block_key_n == 0) {
    block_key_arr = construct_empty_array(INT2OID);
  } else {
    block_key_elems = palloc(eff_block_key_n * sizeof(Datum));
    for (uint16 i = 0; i < eff_block_key_n; ++i)
      block_key_elems[i] = Int16GetDatum(eff_block_key[i]);
    block_key_arr = construct_array(block_key_elems, eff_block_key_n,
                                    INT2OID, 2, true, 's');
    pfree(block_key_elems);
  }
  block_key_datum = PointerGetDatum(block_key_arr);
  DirectFunctionCall3(set_table_info,
                      ObjectIdGetDatum(new_relid),
                      kind_datum,
                      block_key_datum);

  if (cap->ancestor_n == 0) {
    ancestors_arr = construct_empty_array(OIDOID);
  } else {
    ancestor_elems = palloc(cap->ancestor_n * sizeof(Datum));
    for (uint16 i = 0; i < cap->ancestor_n; ++i)
      ancestor_elems[i] = ObjectIdGetDatum(cap->ancestors[i]);
    ancestors_arr = construct_array(ancestor_elems, cap->ancestor_n,
                                    OIDOID, sizeof(Oid), true, 'i');
    pfree(ancestor_elems);
  }
  ancestors_datum = PointerGetDatum(ancestors_arr);
  DirectFunctionCall2(set_ancestors,
                      ObjectIdGetDatum(new_relid),
                      ancestors_datum);

  /* Install the provenance_guard trigger via SPI.  Users who later
   * INSERT / UPDATE OF provsql with a non-NULL value will then
   * trigger the standard kind flip to OPAQUE; users who omit the
   * column on INSERT get a fresh @c uuid_generate_v4 leaf (which
   * already disconnects the row from the inherited lineage, but the
   * guard prevents the more dangerous shared-UUID aliasing path).
   *
   * Materialized views are exempt: PG forbids triggers on them, and
   * they cannot be modified through DML anyway (only @c REFRESH @c
   * MATERIALIZED @c VIEW changes the contents -- which re-runs the
   * inner SELECT and the freshly-projected rows continue to carry
   * lineage from the same sources). */
  /* PG 14 renamed CreateTableAsStmt.relkind -> objtype (same ObjectType,
   * same OBJECT_* values; pure field rename). */
#if PG_VERSION_NUM >= 140000
  if (stmt->objtype == OBJECT_MATVIEW)
#else
  if (stmt->relkind == OBJECT_MATVIEW)
#endif
    return;

  nspname = get_namespace_name(get_rel_namespace(new_relid));
  relname = get_rel_name(new_relid);
  if (nspname == NULL || relname == NULL)
    return;
  initStringInfo(&trigger_sql);
  appendStringInfo(&trigger_sql,
      "CREATE TRIGGER provenance_guard "
      "BEFORE INSERT OR UPDATE OF provsql ON %s.%s "
      /* "EXECUTE PROCEDURE" is the legacy form, kept as a valid synonym
       * of "EXECUTE FUNCTION" through PG 18 -- matches the rest of the
       * codebase and stays PG 10-compatible.  Promote when PG 10 drops
       * out of the support floor. */
      "FOR EACH ROW EXECUTE PROCEDURE provsql.provenance_guard()",
      quote_identifier(nspname), quote_identifier(relname));
  if (SPI_connect() != SPI_OK_CONNECT)
    provsql_error("CTAS lineage hook: SPI_connect failed");
  if (SPI_exec(trigger_sql.data, 0) != SPI_OK_UTILITY)
    provsql_error("CTAS lineage hook: failed to install provenance_guard "
                  "on %s.%s", nspname, relname);
  SPI_finish();
  pfree(trigger_sql.data);
}

static void provsql_ProcessUtility(
    PlannedStmt *pstmt,
    const char *queryString,
#if PG_VERSION_NUM >= 140000
    bool readOnlyTree,
#endif
    ProcessUtilityContext context,
    ParamListInfo params,
    QueryEnvironment *queryEnv,
    DestReceiver *dest,
#if PG_VERSION_NUM >= 130000
    QueryCompletion *qc
#else
    char *completionTag
#endif
    ) {
  Node              *parsetree = pstmt ? pstmt->utilityStmt : NULL;
  ProvSQLCtasCapture cap = {0};

  provsql_ProcessUtility_capture(parsetree, &cap);

  if (prev_ProcessUtility)
    prev_ProcessUtility(pstmt, queryString,
#if PG_VERSION_NUM >= 140000
                        readOnlyTree,
#endif
                        context, params, queryEnv, dest,
#if PG_VERSION_NUM >= 130000
                        qc
#else
                        completionTag
#endif
                        );
  else
    standard_ProcessUtility(pstmt, queryString,
#if PG_VERSION_NUM >= 140000
                            readOnlyTree,
#endif
                            context, params, queryEnv, dest,
#if PG_VERSION_NUM >= 130000
                            qc
#else
                            completionTag
#endif
                            );

  provsql_ProcessUtility_apply(parsetree, &cap);
}

/**
 * @brief Extension initialization – called once when the shared library is loaded.
 *
 * Registers the GUC variables (@c provsql.active, @c where_provenance,
 * @c update_provenance, @c verbose_level, @c aggtoken_text_as_uuid,
 * @c tool_search_path), installs the planner hook and shared-memory hooks,
 * and launches the background MMap worker.
 *
 * Must be loaded via @c shared_preload_libraries; raises an error otherwise.
 */
void _PG_init(void) {
#ifndef PROVSQL_INPROCESS_STORE
  /* The multi-process build registers background workers and a shared
     memory segment, which only works when loaded at postmaster start via
     shared_preload_libraries.  The single-process build has neither: the
     planner hook is installed here at CREATE EXTENSION / dlopen time and
     covers every subsequent query in the one backend, so no preload (and
     no shared_preload_libraries, which the WASM host does not support) is
     required. */
  if (!process_shared_preload_libraries_in_progress)
    provsql_error("provsql needs to be added to the shared_preload_libraries "
                  "configuration variable");
#endif

  DefineCustomBoolVariable("provsql.active",
                           "Should ProvSQL track provenance?",
                           "1 is standard ProvSQL behavior, 0 means provsql attributes will be dropped.",
                           &provsql_active,
                           true,
                           PGC_USERSET,
                           0,
                           NULL,
                           NULL,
                           NULL);
  DefineCustomBoolVariable("provsql.where_provenance",
                           "Should ProvSQL track where-provenance?",
                           "1 turns where-provenance on, 0 off.",
                           &provsql_where_provenance,
                           false,
                           PGC_USERSET,
                           0,
                           NULL,
                           NULL,
                           NULL);
  DefineCustomBoolVariable("provsql.update_provenance",
                           "Should ProvSQL track update provenance?",
                           "1 turns update provenance on, 0 off.",
                           &provsql_update_provenance,
                           false,
                           PGC_USERSET,
                           0,
                           NULL,
                           NULL,
                           NULL);
  DefineCustomBoolVariable("provsql.aggtoken_text_as_uuid",
                           "Output agg_token cells as the underlying UUID "
                           "instead of \"value (*)\".",
                           "Off by default for psql-friendly output. UI "
                           "layers (notably ProvSQL Studio) flip this on "
                           "per session so aggregate cells expose the "
                           "circuit root UUID for click-through; the "
                           "display value is recovered via "
                           "provsql.agg_token_value_text(uuid).",
                           &provsql_aggtoken_text_as_uuid,
                           false,
                           PGC_USERSET,
                           0,
                           NULL,
                           NULL,
                           NULL);
  DefineCustomIntVariable("provsql.verbose_level",
                          "Level of verbosity for ProvSQL informational and debug messages",
                          "0 for quiet (default), 1-9 for informational messages, 10-100 for debug information.",
                          &provsql_verbose,
                          0,
                          0,
                          100,
                          PGC_USERSET,
                          1,
                          NULL,
                          NULL,
                          NULL);
  DefineCustomStringVariable("provsql.tool_search_path",
                             "Directories prepended to PATH when ProvSQL spawns external tools (superuser-only).",
                             "Colon-separated list of directories searched before the server's PATH "
                             "when locating d4, c2d, minic2d, dsharp, weightmc, or graph-easy. "
                             "Empty (default) means rely on the server's PATH alone. "
                             "Restricted to superusers (PGC_SUSET): it controls which directories the "
                             "postgres OS user searches for executables, so a non-privileged role must "
                             "not be able to redirect it to an attacker-controlled binary.",
                             &provsql_tool_search_path,
                             "",
                             PGC_SUSET,
                             0,
                             NULL,
                             NULL,
                             NULL);
  DefineCustomStringVariable("provsql.fallback_compiler",
                             "Compiler used by makeDD's final fallback when both "
                             "interpretAsDD and tree-decomposition fail.",
                             "Name of the external compiler invoked by "
                             "BooleanCircuit::makeDD after interpretAsDD raises "
                             "(non-independent or non-NNF circuit) and the "
                             "tree-decomposition builder raises (treewidth above "
                             "the supported bound). Accepts any value supported "
                             "by BooleanCircuit::compilation: d4, d4v2, c2d, "
                             "minic2d, dsharp, panini-obdd, panini-obdd-and, "
                             "panini-decdnnf. Default: d4.",
                             &provsql_fallback_compiler,
                             "d4",
                             PGC_USERSET,
                             0,
                             NULL,
                             NULL,
                             NULL);
  DefineCustomStringVariable("provsql.kcmcp_server",
                             "Launch command for the managed KCMCP knowledge-compiler server.",
                             "Shell command the supervisor background worker runs to start a "
                             "warm KCMCP server (see the KC server protocol). The literal "
                             "{endpoint} is replaced by a Unix-socket path the worker picks "
                             "and publishes for the in-extension client to reach (a registry "
                             "record of kind 'kcmcp' with endpoint 'managed' uses it). {endpoint} "
                             "already carries the scheme (e.g. unix:/path). Empty (default) "
                             "launches no server. Example: 'tdkc --kcmcp {endpoint}'. "
                             "PGC_SIGHUP (config file / ALTER SYSTEM + reload): it runs an "
                             "arbitrary command as the postgres OS user, so like "
                             "provsql.tool_search_path it is not settable per session.",
                             &provsql_kcmcp_server,
                             "",
                             PGC_SIGHUP,
                             0,
                             NULL,
                             NULL,
                             NULL);
  DefineCustomStringVariable("provsql.last_eval_method",
                             "Probability evaluation method(s) used by the most "
                             "recent probability_evaluate call.",
                             "Set automatically after each probability_evaluate "
                             "call to the method that produced the result "
                             "(comma-separated and deduplicated across calls in "
                             "the session).  Useful to see which strategy the "
                             "default auto-selection settled on.",
                             &provsql_last_eval_method,
                             "",
                             PGC_USERSET,
                             0,
                             NULL,
                             NULL,
                             NULL);
  DefineCustomBoolVariable("provsql.simplify_on_load",
                           "Apply universal cmp-resolution passes when "
                           "loading a provenance circuit.",
                           "When on (default), every GenericCircuit returned "
                           "by getGenericCircuit goes through RangeCheck "
                           "(and any future universal pass): comparators "
                           "decidable to certain Boolean values become "
                           "Bernoulli gate_input gates with probability 0 "
                           "or 1, transparent to every downstream consumer "
                           "(semiring evaluators, MC, view_circuit, PROV "
                           "export). Set off to inspect raw circuit "
                           "structure (e.g. when debugging gate-creation "
                           "paths).",
                           &provsql_simplify_on_load,
                           true,
                           PGC_USERSET,
                           0,
                           NULL,
                           NULL,
                           NULL);
  /* Debug-only: hidden from SHOW ALL and postgresql.conf.sample.
   * On is strictly better for end users (analytic answers where
   * possible, lower MC variance, more methods usable on continuous
   * circuits); off only serves developer A/B against pure MC and as
   * a bisection escape valve if a closure rule misbehaves. */
  DefineCustomBoolVariable("provsql.hybrid_evaluation",
                           "Run the hybrid-evaluator simplifier and "
                           "island decomposer inside probability_evaluate. "
                           "Debug only.",
                           "When on (default), probability_evaluate runs "
                           "the HybridEvaluator peephole simplifier "
                           "between RangeCheck and AnalyticEvaluator and "
                           "the per-cmp MC island decomposer after "
                           "AnalyticEvaluator. Off bypasses both and lets "
                           "unresolved comparators fall through to "
                           "whole-circuit MC. End users have no reason "
                           "to flip this; it exists for developer A/B "
                           "testing against the unfolded path and as a "
                           "bisection knob if a closure rule turns out "
                           "to be unsound on some workload.",
                           &provsql_hybrid_evaluation,
                           true,
                           PGC_USERSET,
                           GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
                           NULL,
                           NULL,
                           NULL);
  /* Debug-only: hidden from SHOW ALL and postgresql.conf.sample.
   * Umbrella for closed-form / analytic resolution of gate_cmp
   * probabilities in probability_evaluate (currently the
   * Poisson-binomial HAVING-COUNT pre-pass; future MIN / MAX / SUM
   * pre-passes gate on the same flag).  On is strictly better for
   * end users (each resolver replaces an exponential DNF
   * construction with O(N) or O(N x C) arithmetic); off only serves
   * developer A/B against the unoptimised enumerate_valid_worlds
   * path and as a bisection escape valve. */
  DefineCustomBoolVariable("provsql.cmp_probability_evaluation",
                           "Run closed-form / analytic probability "
                           "evaluators for gate_cmps inside "
                           "probability_evaluate. Debug only.",
                           "When on (default), probability_evaluate "
                           "runs pre-passes that recognise specific "
                           "gate_cmp shapes (currently HAVING COUNT(*) "
                           "op C over distinct gate_input leaves) and "
                           "replace each cmp with a Bernoulli "
                           "gate_input carrying the closed-form "
                           "probability, bypassing the DNF that "
                           "provsql_having's enumerate_valid_worlds "
                           "would otherwise emit. Off forces the cmp "
                           "to fall through to that enumeration path. "
                           "Future MIN / MAX / SUM probability "
                           "evaluators will gate on the same flag. "
                           "End users have no reason to flip this; it "
                           "exists for developer A/B testing and as a "
                           "bisection escape valve.",
                           &provsql_cmp_probability_evaluation,
                           true,
                           PGC_USERSET,
                           GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
                           NULL,
                           NULL,
                           NULL);
  /* Kill-switch for the automatic inversion-free path in the default
   * probability chain.  The path only fires when the query carries an
   * inversion-free certificate (attached by the planner only to certified
   * queries), so it is self-gating and safe on by default; off is for A/B
   * testing against the tree-decomposition / d4 fallback.  The explicit
   * 'inversion-free' method bypasses this flag. */
  DefineCustomBoolVariable("provsql.inversion_free",
                           "Use the inversion-free structured-d-DNNF "
                           "probability path when available.",
                           "When on (default), probability_evaluate, on a "
                           "query whose provenance root carries an "
                           "inversion-free tractability certificate, tries the "
                           "structured-d-DNNF builder after the read-once "
                           "independent evaluator and before the "
                           "tree-decomposition / external-compiler fallback. "
                           "Off disables only this automatic insertion; the "
                           "explicit probability_evaluate(token, "
                           "'inversion-free') method always runs and errors "
                           "without a certificate. The path is gated on the "
                           "certificate, attached only to certified queries, "
                           "so on is safe; off serves developer A/B testing.",
                           &provsql_inversion_free,
                           true,
                           PGC_USERSET,
                           GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
                           NULL,
                           NULL,
                           NULL);
  DefineCustomBoolVariable("provsql.boolean_provenance",
                           "Opt-in safe-query optimisation for hierarchical conjunctive queries.",
                           "When on, the planner rewrites self-join-free "
                           "hierarchical conjunctive queries (and independent "
                           "UCQs) over TID/BID tables to a read-once form "
                           "whose probability is computable in linear time "
                           "via the existing BooleanCircuit independent "
                           "evaluator. The rewrite preserves the Boolean "
                           "polynomial of the existential lineage, so any "
                           "evaluation that factors through a homomorphism "
                           "from Boolean functions remains sound (notably "
                           "probability and Shapley / Banzhaf). The "
                           "resulting root gate is tagged; semiring "
                           "evaluators are individually marked at compile "
                           "time as compatible or not with this rewrite, "
                           "and incompatible evaluators refuse to run on a "
                           "tagged circuit. Off by default because the "
                           "rewrite changes the multiset of result rows and "
                           "is therefore unsound for per-row provenance "
                           "interrogations and aggregation queries.",
                           &provsql_boolean_provenance,
                           false,
                           PGC_USERSET,
                           0,
                           NULL,
                           NULL,
                           NULL);
  DefineCustomBoolVariable("provsql.classify_top_level",
                           "Emit a NOTICE classifying each top-level SELECT.",
                           "When on, every top-level SELECT that "
                           "touches a relation triggers a NOTICE of "
                           "the form `ProvSQL: query result is "
                           "<KIND> (sources: ...)` where <KIND> is "
                           "TID, BID, or OPAQUE under the existing "
                           "provsql_table_kind taxonomy and the "
                           "sources list names the provenance-"
                           "tracked base relations the query touches. "
                           "Read-only : the classifier does not "
                           "rewrite the query.  Studio reads the "
                           "NOTICE to label query results with their "
                           "certified kind.",
                           &provsql_classify_top_level,
                           false,
                           PGC_USERSET,
                           0,
                           NULL,
                           NULL,
                           NULL);
  DefineCustomBoolVariable("provsql.classify_having",
                           "Emit a NOTICE classifying each HAVING aggregate "
                           "comparison under the Ré-Suciu trichotomy.",
                           "When on, every top-level SELECT with a HAVING "
                           "aggregate comparison `alpha(y) theta k` triggers "
                           "a NOTICE labelling the (alpha, theta) pair as "
                           "safe (exact PTIME), apx-safe (exact #P-hard but an "
                           "FPRAS exists), hazardous (no FPRAS), or open, "
                           "combining the static (alpha, theta) overlay with "
                           "the skeleton-safety axis.  Read-only : the "
                           "classifier does not rewrite the query.",
                           &provsql_classify_having,
                           false,
                           PGC_USERSET,
                           0,
                           NULL,
                           NULL,
                           NULL);
  DefineCustomIntVariable("provsql.monte_carlo_seed",
                          "Seed for the Monte Carlo sampler.",
                          "-1 (default) seeds from std::random_device for "
                          "non-deterministic sampling. Any other value "
                          "(including 0) is used as a literal seed for "
                          "std::mt19937_64, making "
                          "probability_evaluate(..., 'monte-carlo', n) "
                          "reproducible across runs and across the Bernoulli "
                          "and continuous (gate_rv) sampling paths.",
                          &provsql_monte_carlo_seed,
                          -1,
                          -1,
                          INT_MAX,
                          PGC_USERSET,
                          0,
                          NULL,
                          NULL,
                          NULL);
  DefineCustomIntVariable("provsql.rv_mc_samples",
                          "Default sample count for analytical-evaluator MC fallbacks.",
                          "Used when an analytical evaluator (Expectation, "
                          "future hybrid evaluator, etc.) cannot decompose a "
                          "sub-circuit and needs to fall back to Monte Carlo. "
                          "Default 10000. Set to 0 to disable the fallback "
                          "entirely: callers raise an exception rather than "
                          "sampling, which is useful when only analytical "
                          "answers are acceptable. Unrelated to "
                          "probability_evaluate(..., 'monte-carlo', n) where "
                          "the sample count is an explicit argument.",
                          &provsql_rv_mc_samples,
                          10000,
                          0,
                          INT_MAX,
                          PGC_USERSET,
                          0,
                          NULL,
                          NULL,
                          NULL);

  // Emit warnings for undeclared provsql.* configuration parameters
  EmitWarningsOnPlaceholders("provsql");

  prev_planner = planner_hook;
  prev_shmem_startup = shmem_startup_hook;
  prev_ExecutorStart = ExecutorStart_hook;
  prev_ExecutorEnd   = ExecutorEnd_hook;
  prev_ProcessUtility = ProcessUtility_hook;
#ifdef PROVSQL_INPROCESS_STORE
  /* Single-process store: no shared memory to request and no background
     worker; the circuit lives in this process behind an in-memory
     dispatch. */
  provsql_inproc_init();
#elif (PG_VERSION_NUM >= 150000)
  prev_shmem_request = shmem_request_hook;
  shmem_request_hook = provsql_shmem_request;
#else
  provsql_shmem_request();
#endif

  planner_hook        = provsql_planner;
#ifndef PROVSQL_INPROCESS_STORE
  shmem_startup_hook  = provsql_shmem_startup;
#endif
  ExecutorStart_hook  = provsql_executor_start;
  ExecutorEnd_hook    = provsql_executor_end;
  ProcessUtility_hook = provsql_ProcessUtility;

#ifndef PROVSQL_INPROCESS_STORE
  RegisterProvSQLMMapWorker();
  RegisterProvSQLKCMCPWorker();
#endif
}

/**
 * @brief Extension teardown — restores the planner and shmem hooks.
 */
void _PG_fini(void) {
  planner_hook        = prev_planner;
  shmem_startup_hook  = prev_shmem_startup;
  ExecutorStart_hook  = prev_ExecutorStart;
  ExecutorEnd_hook    = prev_ExecutorEnd;
  ProcessUtility_hook = prev_ProcessUtility;
}
