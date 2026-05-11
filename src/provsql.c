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
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/print.h"
#include "optimizer/planner.h"
#include "parser/parse_coerce.h"
#include "parser/parse_oper.h"
#include "utils/builtins.h"
#include "parser/parsetree.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "catalog/pg_cast.h"
#include <time.h>

#include "provsql_mmap.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

#if PG_VERSION_NUM < 100000
#error "ProvSQL requires PostgreSQL version 10 or later"
#endif

#include "compatibility.h"

PG_MODULE_MAGIC; ///< Required PostgreSQL extension magic block

/* -------------------------------------------------------------------------
 * Global state & forward declarations
 * ------------------------------------------------------------------------- */

bool provsql_interrupted = false;
bool provsql_active = true; ///< @c true while ProvSQL query rewriting is enabled
bool provsql_where_provenance = false;
bool provsql_update_provenance = false; ///< @c true when provenance tracking for DML is enabled
int provsql_verbose = 100; ///< Verbosity level; controlled by the @c provsql.verbose_level GUC
bool provsql_aggtoken_text_as_uuid = false; ///< When @c true, @c agg_token::text emits the underlying provenance UUID instead of @c "value (*)"
char *provsql_tool_search_path = NULL; ///< Colon-separated directory list prepended to @c PATH when invoking external tools (d4, c2d, minic2d, dsharp, weightmc, graph-easy); controlled by the @c provsql.tool_search_path GUC
int provsql_monte_carlo_seed = -1; ///< Seed for the Monte Carlo sampler; -1 means non-deterministic (std::random_device); controlled by the @c provsql.monte_carlo_seed GUC
int provsql_rv_mc_samples = 10000; ///< Default sample count for analytical-evaluator MC fallbacks; 0 disables fallback (callers raise instead); controlled by the @c provsql.rv_mc_samples GUC
bool provsql_simplify_on_load = true; ///< Run universal cmp-resolution passes when @c getGenericCircuit returns; controlled by the @c provsql.simplify_on_load GUC
bool provsql_hybrid_evaluation = true; ///< Run the hybrid-evaluator simplifier inside @c probability_evaluate; controlled by the @c provsql.hybrid_evaluation GUC

static const char *PROVSQL_COLUMN_NAME = "provsql"; ///< Name of the provenance column added to tracked tables

extern void _PG_init(void);
extern void _PG_fini(void);

static planner_hook_type prev_planner = NULL; ///< Previous planner hook (chained)

static Query *process_query(const constants_t *constants, Query *q,
                            bool **removed);

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
 * @param node     Current expression tree node.
 * @param context  Mutation context carrying varno and offset.
 * @return         Possibly modified node.
 */
static Node *reduce_varattno_mutator(Node *node,
                                     reduce_varattno_mutator_context *context) {
  if (node == NULL)
    return NULL;

  if (IsA(node, Var)) {
    Var *v = (Var *)node;

    if (v->varno == context->varno) {
      v->varattno += context->offset[v->varattno - 1];
    }
  }

  return expression_tree_mutator(node, reduce_varattno_mutator,
                                 (void *)context);
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
    expression_tree_mutator(te, reduce_varattno_mutator, (void *)&context);
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
 * @param node     Current expression tree node.
 * @param context  Mutation context with varno, varattno, and constants.
 * @return         Possibly modified node.
 */
static Node *
aggregation_type_mutator(Node *node,
                         aggregation_type_mutator_context *context) {
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
  return expression_tree_mutator(node, aggregation_type_mutator,
                                 (void *)context);
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
 * @brief Inline CTE references as subqueries within a query.
 *
 * Replaces each non-recursive RTE_CTE entry in @p rtable with an
 * RTE_SUBQUERY containing a copy of the CTE's query, looking up
 * definitions in @p cteList.  Recurses into newly inlined subqueries
 * to handle nested CTE references (ctelevelsup > 0).
 *
 * @param rtable   Range table to scan for RTE_CTE entries.
 * @param cteList  CTE definitions to look up names in.
 */
static void inline_ctes_in_rtable(List *rtable, List *cteList) {
  ListCell *lc;
  foreach (lc, rtable) {
    RangeTblEntry *r = (RangeTblEntry *)lfirst(lc);
    if (r->rtekind == RTE_CTE) {
      ListCell *lc2;
      foreach (lc2, cteList) {
        CommonTableExpr *cte = (CommonTableExpr *)lfirst(lc2);
        if (strcmp(cte->ctename, r->ctename) == 0) {
          if (cte->cterecursive) {
            provsql_error("Recursive CTEs not supported");
          } else {
            r->rtekind = RTE_SUBQUERY;
            r->subquery = copyObject((Query *)cte->ctequery);
            r->ctename = NULL;
            r->ctelevelsup = 0;
            /* Recurse: the inlined subquery may reference other CTEs
             * from the same cteList */
            inline_ctes_in_rtable(r->subquery->rtable, cteList);
          }
          break;
        }
      }
    } else if (r->rtekind == RTE_SUBQUERY && r->subquery != NULL) {
      /* Recurse into existing subqueries (e.g., UNION branches) to
       * inline CTE references they may contain */
      inline_ctes_in_rtable(r->subquery->rtable, cteList);
    }
  }
}

/**
 * @brief Inline all CTE references in @p q as subqueries.
 */
static void inline_ctes(Query *q) {
  if (q->cteList == NIL)
    return;
  inline_ctes_in_rtable(q->rtable, q->cteList);
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
 * @return  List of @c Var nodes, one per provenance source; @c NIL if the
 *          query has no provenance-bearing relation.
 */
static List *get_provenance_attributes(const constants_t *constants, Query *q) {
  List *prov_atts = NIL;

  for(Index rteid = 1; rteid <= q->rtable->length; ++rteid) {
    RangeTblEntry *r = list_nth_node(RangeTblEntry, q->rtable, rteid-1);

    if (r->rtekind == RTE_RELATION) {
      ListCell *lc;
      AttrNumber attid = 1;

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
        process_query(constants, r->subquery, &inner_removed);
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
    if (aggregation_function == F_COUNT_ ||
        aggregation_function == F_COUNT_ANY) // count(*) or count(arg)
    {
      Const *one = makeConst(constants->OID_TYPE_INT, -1, InvalidOid,
                             sizeof(int32), Int32GetDatum(1), false, true);
      expr_s->args = list_make2(one, expr);
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

/**
 * @brief Convert a comparison @c OpExpr on aggregate results into a
 *        @c provenance_cmp gate expression.
 *
 * Each argument of @p opExpr must be one of:
 * - A @c Var of type @c agg_token (or a @c FuncExpr implicit-cast wrapper
 *   around one) → cast to UUID via @c agg_token_to_uuid.
 * - A scalar @c Const → wrapped in @c provenance_semimod(const, gate_one()).
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

    if (IsA(node, FuncExpr)) {
      FuncExpr *fe = (FuncExpr *)node;
      if (fe->funcid == constants->OID_FUNCTION_PROVENANCE_AGGREGATE) {
        // We need to add an explicit cast to UUID
        FuncExpr *castToUUID = makeNode(FuncExpr);

        castToUUID->funcid = constants->OID_FUNCTION_AGG_TOKEN_UUID;
        castToUUID->funcresulttype = constants->OID_TYPE_UUID;
        castToUUID->args = list_make1(fe);
        castToUUID->location = -1;

        arguments[i] = (Node *)castToUUID;
      } else {
        provsql_error("cannot handle complex HAVING expressions");
      }
    } else if (IsA(node, Var)) {
      Var *v = (Var *)node;

      if (v->vartype == constants->OID_TYPE_AGG_TOKEN) {
        // We need to add an explicit cast to UUID
        FuncExpr *castToUUID = makeNode(FuncExpr);

        castToUUID->funcid = constants->OID_FUNCTION_AGG_TOKEN_UUID;
        castToUUID->funcresulttype = constants->OID_TYPE_UUID;
        castToUUID->args = list_make1(v);
        castToUUID->location = -1;

        arguments[i] = (Node *)castToUUID;
      } else {
        provsql_error("cannot handle complex HAVING expressions");
      }
    } else if (IsA(node, Const)) {
      Const *literal = (Const *)node;
      FuncExpr *oneExpr, *semimodExpr;

      // gate_one() expression
      oneExpr = makeNode(FuncExpr);
      oneExpr->funcid = constants->OID_FUNCTION_GATE_ONE;
      oneExpr->funcresulttype = constants->OID_TYPE_UUID;
      oneExpr->args = NIL;
      oneExpr->location = -1;

      // provenance_semimod(literal, gate_one())
      semimodExpr = makeNode(FuncExpr);
      semimodExpr->funcid = constants->OID_FUNCTION_PROVENANCE_SEMIMOD;
      semimodExpr->funcresulttype = constants->OID_TYPE_UUID;
      semimodExpr->args = list_make2((Expr *)literal, (Expr *)oneExpr);
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
 *        @c random_variable_uuid() FuncExpr to extract its UUID.
 *
 * Operand of the comparison may be a Var, a constant lifted by an
 * implicit cast, or another OpExpr (e.g. <tt>a + b</tt>).  In every
 * case the wrapping is the same — a single-arg FuncExpr around it.
 */
static FuncExpr *
wrap_random_variable_uuid(Node *operand, const constants_t *constants)
{
  FuncExpr *cast = makeNode(FuncExpr);
  cast->funcid = constants->OID_FUNCTION_RANDOM_VARIABLE_UUID;
  cast->funcresulttype = constants->OID_TYPE_UUID;
  cast->args = list_make1(operand);
  cast->location = -1;
  return cast;
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
 * @return  The provenance @c Expr to be appended to the target list.
 */
static Expr *make_provenance_expression(const constants_t *constants, Query *q,
                                        List *prov_atts, bool aggregation,
                                        bool group_by_rewrite,
                                        semiring_operation op, int **columns,
                                        int nbcols) {
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

    if (aggregation && !q->havingQual) {
      FuncExpr *deltaExpr = makeNode(FuncExpr);

      // adding the delta gate to the provenance circuit
      deltaExpr->funcid = constants->OID_FUNCTION_PROVENANCE_DELTA;
      deltaExpr->args = list_make1(result);
      deltaExpr->funcresulttype = constants->OID_TYPE_UUID;
      deltaExpr->location = -1;

      result = (Expr *)deltaExpr;
    }

    if (q->havingQual) {
      result = (Expr*) having_Expr_to_provenance_cmp((Expr*)q->havingQual, constants, false);
      q->havingQual = NULL;
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
resolve_group_rte_vars_mutator(Node *node, resolve_group_rte_ctx *ctx) {
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
  return expression_tree_mutator(node, resolve_group_rte_vars_mutator,
                                 (void *)ctx);
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
 * @param q            Query to inspect and possibly rewrite.
 * @param constants    Extension OID cache.
 * @return   Rewritten query, or @c NULL if no @c AGG(DISTINCT) was found.
 */
static Query *rewrite_agg_distinct(Query *q, const constants_t *constants) {
  List *distinct_agg_tes = NIL;
  List *groupby_tes = NIL;
  ListCell *lc;

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
  if (q->hasGroupRTE) {
    resolve_group_rte_ctx grp_ctx;
    bool found = false;
    ListCell *lc2;
    Index idx = 1;
    int rte_len = 0;

    foreach (lc2, q->rtable) {
      RangeTblEntry *r = (RangeTblEntry *)lfirst(lc2);
      if (r->rtekind == RTE_GROUP) {
        grp_ctx.group_rtindex = idx;
        grp_ctx.groupexprs    = r->groupexprs;
        found    = true;
        rte_len  = idx - 1;
        break;
      }
      idx++;
    }

    if (found) {
      /* Remove the RTE_GROUP (always last, so truncate is safe) */
      q->rtable      = list_truncate(q->rtable, rte_len);
      q->hasGroupRTE = false;

      /* Resolve Var(group_rtindex, i) → underlying base-table expression
       * throughout the parts of q we will touch below */
      foreach (lc2, q->targetList) {
        TargetEntry *te = (TargetEntry *)lfirst(lc2);
        te->expr = (Expr *)resolve_group_rte_vars_mutator(
          (Node *)te->expr, &grp_ctx);
      }
      if (q->jointree && q->jointree->quals)
        q->jointree->quals = resolve_group_rte_vars_mutator(
          q->jointree->quals, &grp_ctx);
    }
  }
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

  if (distinct_agg_tes == NIL)
    return NULL;

  {
    int n_aggs = list_length(distinct_agg_tes);
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
 * @param node     Current expression tree node.
 * @param context  Mutation context with prov_atts, op, and constants.
 * @return         Possibly modified node.
 */
static Node *aggregation_mutator(Node *node,
                                 aggregation_mutator_context *context) {
  if (node == NULL)
    return NULL;

  if (IsA(node, Aggref)) {
    Aggref *ar_v = (Aggref *)node;
    return (Node *)make_aggregation_expression(context->constants, ar_v,
                                               context->prov_atts, context->op);
  }

  return expression_tree_mutator(node, aggregation_mutator, (void *)context);
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
} provenance_mutator_context;

/**
 * @brief Tree-mutator that replaces provenance() calls with the actual provenance expression.
 * @param node     Current expression tree node.
 * @param context  Mutation context with the provenance expression and constants.
 * @return         Possibly modified node.
 */
static Node *provenance_mutator(Node *node,
                                provenance_mutator_context *context) {
  if (node == NULL)
    return NULL;

  if (IsA(node, FuncExpr)) {
    FuncExpr *f = (FuncExpr *)node;

    if (f->funcid == context->constants->OID_FUNCTION_PROVENANCE) {
      return (Node *)copyObject(context->provsql);
    }
  } else if (IsA(node, RangeTblEntry) || IsA(node, RangeTblFunction)) {
    // A provenance() expression in a From (not within a subquery) is
    // non-sensical
    return node;
  }

  return expression_tree_mutator(node, provenance_mutator, (void *)context);
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
  provenance_mutator_context context = {provsql, constants};

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
static bool has_provenance_walker(Node *node, void *data) {
  const constants_t *constants = (const constants_t *)data;
  if (node == NULL)
    return false;

  if (IsA(node, Query)) {
    Query *q = (Query *)node;
    ListCell *rc;

    /* Walk into CTE subqueries explicitly, because expression_tree_walker
     * ignores Query nodes so query_tree_walker's walk of cteList does not
     * recurse into ctequery */
    foreach (rc, q->cteList) {
      CommonTableExpr *cte = (CommonTableExpr *)lfirst(rc);
      if (has_provenance_walker((Node *)cte->ctequery, data))
        return true;
    }

    if (query_tree_walker(q, has_provenance_walker, data, 0))
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
      }
    }
  }

  return expression_tree_walker(node, provenance_function_walker, data);
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
 * @brief Tree walker that detects any Var of type agg_token.
 * @param node      Current expression tree node.
 * @param constants Extension OID cache.
 * @return          @c true if an agg_token Var is found in @p node.
 */
static bool aggtoken_walker(Node *node, const constants_t *constants) {
  if (node == NULL)
    return false;

  if (IsA(node, Var)) {
    Var *v = (Var *) node;
    if(v->vartype == constants->OID_TYPE_AGG_TOKEN)
      return true;
  }

  return expression_tree_walker(node, aggtoken_walker, (void*) constants);
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

  rte->alias = NULL;
  rte->eref = NULL;
  rte->joinaliasvars = NULL;

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
    if (r->eref) {
      unsigned j = 0;

      columns[i] = (int *)palloc(r->eref->colnames->length * sizeof(int));

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
 * @return  The (possibly restructured) rewritten query, or @c NULL if the
 *          query has no FROM clause and can be skipped.
 */
static Query *process_query(const constants_t *constants, Query *q,
                            bool **removed) {
  List *prov_atts;
  bool has_union = false;
  bool has_difference = false;
  bool supported = true;
  bool group_by_rewrite = false;
  int nbcols = 0;
  int **columns;
  unsigned i = 0;
  if (provsql_verbose >= 50)
    elog_node_display(NOTICE, "ProvSQL: Before query rewriting", q, true);

  if (q->rtable == NULL) {
    // No FROM clause, we can skip this query
    return NULL;
  }

  /* Inline non-recursive CTE references as subqueries so we can track
   * provenance through them. Must happen before set operation handling
   * since UNION/EXCEPT branches may reference CTEs. */
  inline_ctes(q);

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
        return process_query(constants, q, removed);
      }
    }

    if (q->hasAggs) {
      Query *rewritten = rewrite_agg_distinct(q, constants);
      if (rewritten)
        return process_query(constants, rewritten, removed);
    }

    // get_provenance_attributes will also recursively process subqueries
    // by calling process_query
    prov_atts = get_provenance_attributes(constants, q);

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
        nbcols);

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

  if (provsql_te == NULL)
    return;

  /* Rewrite the source SELECT to carry provenance */
  {
    bool *removed = NULL;
    Query *new_subquery = process_query(constants, src_rte->subquery, &removed);
    AttrNumber src_provsql_attno = 0;

    if (new_subquery == NULL)
      return;

    src_rte->subquery = new_subquery;

    /* Find the provsql column in the rewritten subquery, verify its type */
    foreach (lc, new_subquery->targetList) {
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
static PlannedStmt *provsql_planner(Query *q,
#if PG_VERSION_NUM >= 130000
                                    const char *query_string,
#endif
                                    int cursorOptions,
                                    ParamListInfo boundParams) {
  if (q->commandType == CMD_INSERT && q->rtable) {
    const constants_t constants = get_constants(false);
    if (constants.ok)
      process_insert_select(&constants, q);
  } else if (q->commandType == CMD_SELECT && q->rtable) {
    const constants_t constants = get_constants(false);

    if (constants.ok && has_provenance(&constants, q)) {
      bool *removed = NULL;
      Query *new_query;
      clock_t begin = 0;

#if PG_VERSION_NUM >= 150000
      if (provsql_verbose >= 20)
        provsql_notice("Main query before query rewriting:\n%s\n",
                       pg_get_querydef(q, true));
#endif

      if (provsql_verbose >= 40)
        begin = clock();

      new_query = process_query(&constants, q, &removed);

      if (provsql_verbose >= 40)
        provsql_notice("planner time spent=%f",
                       (double)(clock() - begin) / CLOCKS_PER_SEC);

#if PG_VERSION_NUM >= 150000
      if (provsql_verbose >= 20)
        provsql_notice("Main query after query rewriting:\n%s\n",
                       pg_get_querydef(q, true));
#endif

      if (new_query != NULL)
        q = new_query;
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
  if (!process_shared_preload_libraries_in_progress)
    provsql_error("provsql needs to be added to the shared_preload_libraries "
                  "configuration variable");

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
                             "Directories prepended to PATH when ProvSQL spawns external tools.",
                             "Colon-separated list of directories searched before the server's PATH "
                             "when locating d4, c2d, minic2d, dsharp, weightmc, or graph-easy. "
                             "Empty (default) means rely on the server's PATH alone.",
                             &provsql_tool_search_path,
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
  DefineCustomBoolVariable("provsql.hybrid_evaluation",
                           "Run the hybrid-evaluator simplifier inside "
                           "probability_evaluate.",
                           "When on (default), probability_evaluate runs "
                           "the HybridEvaluator peephole simplifier "
                           "between RangeCheck and AnalyticEvaluator: "
                           "gate_arith subtrees are constant-folded, "
                           "identity-element wires (0 in PLUS, 1 in "
                           "TIMES) are dropped, and PLUS over independent "
                           "normals or i.i.d. exponentials with the same "
                           "rate is collapsed to a single normal / "
                           "Erlang gate_rv. The folded forms unlock "
                           "AnalyticEvaluator's closed-form CDF on "
                           "naturally-written predicates. Set off to "
                           "bypass the simplifier and let undecidable "
                           "comparators fall through to whole-circuit MC; "
                           "useful when A/B-testing the analytic path "
                           "against the MC fallback. No effect on "
                           "non-probability queries.",
                           &provsql_hybrid_evaluation,
                           true,
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
#if (PG_VERSION_NUM >= 150000)
  prev_shmem_request = shmem_request_hook;
  shmem_request_hook = provsql_shmem_request;
#else
  provsql_shmem_request();
#endif

  planner_hook = provsql_planner;
  shmem_startup_hook = provsql_shmem_startup;

  RegisterProvSQLMMapWorker();
}

/**
 * @brief Extension teardown — restores the planner and shmem hooks.
 */
void _PG_fini(void) {
  planner_hook = prev_planner;
  shmem_startup_hook = prev_shmem_startup;
}
