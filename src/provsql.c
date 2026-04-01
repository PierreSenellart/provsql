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
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/print.h"
#include "optimizer/planner.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include <time.h>

#include "provsql_mmap.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

#if PG_VERSION_NUM < 100000
#error "ProvSQL requires PostgreSQL version 10 or later"
#endif

#include "compatibility.h"

PG_MODULE_MAGIC;

/* -------------------------------------------------------------------------
 * Global state & forward declarations
 * ------------------------------------------------------------------------- */

bool provsql_interrupted = false;
bool provsql_active = true;
bool provsql_where_provenance = false;
bool provsql_update_provenance = false;
int provsql_verbose = 100;

static const char *PROVSQL_COLUMN_NAME = "provsql";

extern void _PG_init(void);
extern void _PG_fini(void);

static planner_hook_type prev_planner = NULL;

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

typedef struct reduce_varattno_mutator_context {
  Index varno;
  int *offset;
} reduce_varattno_mutator_context;

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

typedef struct aggregation_type_mutator_context {
  Index varno;
  Index varattno;
  const constants_t *constants;
} aggregation_type_mutator_context;

static Node *
aggregation_type_mutator(Node *node,
                         aggregation_type_mutator_context *context) {
  if (node == NULL)
    return NULL;

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
      }
    }
    ++attno;
  }
}

/**
 * @brief Collect all provenance Var nodes reachable from @p q's range table.
 *
 * Walks every RTE in @p q->rtable:
 * - @c RTE_RELATION: looks for a column named @c provsql of type UUID.
 * - @c RTE_SUBQUERY: recursively calls @c process_query and splices the
 *   resulting provenance column back into the parent's column list, also
 *   patching outer Var attribute numbers if inner columns were removed.
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
  ListCell *l;
  Index rteid = 1;

  foreach (l, q->rtable) {
    RangeTblEntry *r = (RangeTblEntry *)lfirst(l);

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
          if (!strcmp(te->resname, PROVSQL_COLUMN_NAME))
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
        ereport(ERROR, (errmsg("JOIN type not supported by provsql")));
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
          ereport(ERROR, (errmsg("FROM function with multiple output "
                                 "attributes not supported by provsql")));
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
      ereport(ERROR, (errmsg("FROM clause unsupported by provsql")));
    }

    ++rteid;
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
 */
typedef enum { SR_PLUS, SR_MONUS, SR_TIMES } semiring_operation;

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
 * @param columns      Per-RTE column-numbering array.
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
      ereport(ERROR, (errmsg("Boolean operators OR and NOT in a join...on "
                             "clause are not supported by provsql")));
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
        elog(ERROR, "ProvSQL cannot handle complex HAVING expressions");
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
        elog(ERROR, "ProvSQL cannot handle complex HAVING expressions");
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
      elog(ERROR, "ProvSQL cannot handle complex HAVING expressions");
    }
  }

  if (negated) {
    opno = get_negator(opno);
    if (!opno)
      elog(ERROR, "ProvSQL: Missing negator");
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
      elog(ERROR, "ProvSQL: Unknown Boolean operator");

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
    elog(ERROR, "ProvSQL: Unknown structure within Boolean expression");
}

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

    fe->funcid = constants->OID_FUNCTION_PROVENANCE_PROJECT;
    fe->funcvariadic = true;
    fe->funcresulttype = constants->OID_TYPE_UUID;
    fe->location = -1;

    array->array_typeid = constants->OID_TYPE_INT_ARRAY;
    array->element_typeid = constants->OID_TYPE_INT;
    array->elements = NIL;
    array->location = -1;

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
            value_v = columns[v->varno - 1][v->varattno - 1];
          } else {
            Const *ce = makeConst(constants->OID_TYPE_INT, -1, InvalidOid,
                                  sizeof(int32), Int32GetDatum(0), false, true);

            array->elements = lappend(array->elements, ce);
            value_v = 0;
          }
        } else
#endif
        if (rte_v->rtekind != RTE_JOIN) { // Normal RTE
          value_v = columns[vte_v->varno - 1][vte_v->varattno - 1];
        } else { // Join RTE
          Var *jav_v = (Var *)lfirst(
            list_nth_cell(rte_v->joinaliasvars, vte_v->varattno - 1));
          value_v = columns[jav_v->varno - 1][jav_v->varattno - 1];
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

/**
 * @brief Wrap @p subq in a new outer query that groups away the DISTINCT.
 *
 * Called when a query contains AGG DISTINCT (e.g., @c COUNT(DISTINCT x)).
 * @p subq is the version of the query where the DISTINCT has been converted
 * to a GROUP BY (produced by @c check_for_agg_distinct).  A new outer query
 * is built that treats @p subq as a subquery RTE and re-applies the original
 * aggregate function on top.
 *
 * @param q    Original query (its structure is reused for the outer query).
 * @param subq Inner query with DISTINCT converted to GROUP BY.
 * @return  The rewritten outer query.
 */
static Query *rewrite_for_agg_distinct(Query *q, Query *subq) {
  // variables
  Alias *alias = makeNode(Alias);
  Alias *eref = makeNode(Alias);
  FromExpr *jointree = makeNode(FromExpr);
  RangeTblEntry *rte = makeNode(RangeTblEntry);
  RangeTblRef *rtr = makeNode(RangeTblRef);
  ListCell *lc_v;
  int groupRef = 1;
  // rewrite the rtable to contain only one relation, the alias
  alias->aliasname = "a";
  eref->aliasname = "a";
  eref->colnames = NIL;

  foreach (lc_v, q->targetList) {
    TargetEntry *te_v = (TargetEntry *)lfirst(lc_v);
    eref->colnames =
      lappend(eref->colnames, te_v->resname?makeString(pstrdup(te_v->resname)):NULL);
#if PG_VERSION_NUM < 160000
    // For PG_VERSION_NUM >= 160000, rte->perminfoindex==0 so no need to
    // care about permissions
    rte->selectedCols = bms_add_member(
      rte->selectedCols, te_v->resno - FirstLowInvalidHeapAttributeNumber);
#endif
  }
  rte->alias = alias;
  rte->eref = eref;
  rte->rtekind = RTE_SUBQUERY;
  rte->subquery = subq;

  q->rtable = list_make1(rte);

  // correct var indexes and group by references
  foreach (lc_v, q->targetList) {
    TargetEntry *te_v = (TargetEntry *)lfirst(lc_v);
    Var *var = makeNode(Var);
    var->varno = 1;
    var->varattno = te_v->resno;
    if (IsA(te_v->expr, Aggref)) {
      Aggref *ar_v = (Aggref *)te_v->expr;
      TargetEntry *te_new = makeNode(TargetEntry);
      var->vartype = linitial_oid(ar_v->aggargtypes);
      te_new->resno = 1;
      te_new->expr = (Expr *)var;
      ar_v->args = list_make1(te_new);
      ar_v->aggdistinct = NIL;
    } else if (IsA(te_v->expr, Var)) {
      Var *var_v = (Var *)te_v->expr;
      var_v->varno = 1;
      var_v->varattno = te_v->resno;
    } else {
      var->vartype = exprType((Node *)te_v->expr);
      te_v->expr = (Expr *)var;
    }
    // add to GROUP BY list
    if (!IsA(te_v->expr, Aggref)) {
      SortGroupClause *sgc = makeNode(SortGroupClause);
      sgc->tleSortGroupRef = groupRef;
      te_v->ressortgroupref = groupRef;
      sgc->nulls_first = false;
      get_sort_group_operators(exprType((Node *)te_v->expr), true, true, false,
                               &sgc->sortop, &sgc->eqop, NULL, &sgc->hashable);
      q->groupClause = lappend(q->groupClause, sgc);
      groupRef++;
    }
  }
  // rewrite the jointree to contain only one relation
  rtr->rtindex = 1;
  jointree->fromlist = list_make1(rtr);
  q->jointree = jointree;
  return q;
}

/**
 * @brief Detect AGG DISTINCT in @p q and rewrite it as a GROUP BY subquery.
 *
 * Scans the top-level target list for @c Aggref nodes with a non-empty
 * @c aggdistinct list.  If found, returns a copy of @p q where each such
 * aggregate is replaced by its argument (which becomes a GROUP BY key),
 * enabling @c rewrite_for_agg_distinct to build the outer aggregation on top.
 *
 * @param q  Query to inspect.
 * @return   Modified copy of @p q if AGG DISTINCT was found, or @c NULL if
 *           no rewriting is needed.
 */
static Query *check_for_agg_distinct(Query *q) {
  ListCell *lc_v;
  List *lst_v = NIL;
  Query *new_q = copyObject(q);
  unsigned char found = 0;

  // replace each Aggref with a TargetEntry calling the agg function
  //-- only in the top-level of the query
  foreach (lc_v, new_q->targetList) {
    TargetEntry *te_v = (TargetEntry *)lfirst(lc_v);
    if (IsA(te_v->expr, Aggref)) {
      Aggref *ar_v = (Aggref *)te_v->expr;
      if (list_length(ar_v->aggdistinct) > 0) {
        TargetEntry *te_new = NULL;
        SortGroupClause *sgc = (SortGroupClause *)linitial(ar_v->aggdistinct);

        found = 1;
        // the agg distinct clause is added to the GROUP BY clause
        // remove aggref and replace by its arguments
        te_new = (TargetEntry *)linitial(ar_v->args);
        sgc->tleSortGroupRef = te_v->resno;
        new_q->groupClause = lappend(new_q->groupClause, sgc);
        te_new->resno = te_v->resno;
        te_new->resname = te_v->resname;
        te_new->ressortgroupref = te_v->resno;
        lst_v = lappend(lst_v, te_new);
      } else {
        lst_v = lappend(lst_v, ar_v);
      }
    } else { // keep the current TE
      // If the TE is a discarded out column used for groupping,
      // we want to keep it to preserve the groupping
      if(te_v->resjunk && !te_v->resname) {
        te_v->resjunk=false;
        te_v->resname="temp";
      }
      lst_v = lappend(lst_v, te_v);
    }
  }
  if (lst_v != NIL)
    new_q->targetList = lst_v;
  if (!found)
    return NULL;
  else
    return new_q;
}

/* -------------------------------------------------------------------------
 * Aggregation replacement mutator
 * ------------------------------------------------------------------------- */

typedef struct aggregation_mutator_context {
  List *prov_atts;
  semiring_operation op;
  const constants_t *constants;
} aggregation_mutator_context;

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

  query_tree_mutator(q, aggregation_mutator, &context,
                     QTW_DONT_COPY_QUERY | QTW_IGNORE_RT_SUBQUERIES);
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

typedef struct provenance_mutator_context {
  Expr *provsql;
  const constants_t *constants;
} provenance_mutator_context;

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
  foreach (lc, q->targetList) {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    if (te->ressortgroupref > 0) {
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

static bool has_provenance_walker(Node *node, void *data) {
  const constants_t *constants = (const constants_t *)data;
  if (node == NULL)
    return false;

  if (IsA(node, Query)) {
    Query *q = (Query *)node;
    ListCell *rc;

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
    ereport(ERROR, (errmsg("Unsupported chain of EXCEPT operations")));
  }

  expr->boolop = AND_EXPR;
  expr->location = -1;
  expr->args = NIL;

  foreach (lc, q->targetList) {
    TargetEntry *te = (TargetEntry *)lfirst(lc);

    Var *v = (Var *)te->expr;

    if (v->vartype != constants->OID_TYPE_UUID) {
      OpExpr *oe = makeNode(OpExpr);
      Oid opno = find_equality_operator(v->vartype, v->vartype);
      Operator opInfo = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
      Form_pg_operator opform = (Form_pg_operator)GETSTRUCT(opInfo);
      Var *leftArg = makeNode(Var), *rightArg = makeNode(Var);

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

  rte->rtekind = RTE_JOIN;
  rte->jointype = JOIN_LEFT;
  // TODO : rte->eref =
  // TODO : rte->joinaliasvars

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
 */
static void process_set_operation_union(const constants_t *constants,
                                        SetOperationStmt *stmt) {
  if (stmt->op != SETOP_UNION) {
    ereport(ERROR, (errmsg("Unsupported mixed set operations")));
  }
  if (IsA(stmt->larg, SetOperationStmt)) {
    process_set_operation_union(constants, (SetOperationStmt *)(stmt->larg));
  }
  if (IsA(stmt->rarg, SetOperationStmt)) {
    process_set_operation_union(constants, (SetOperationStmt *)(stmt->rarg));
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
    elog(ERROR, "ProvSQL: Unknown structure within Boolean expression");

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
    elog(ERROR, "ProvSQL: Unknown structure within Boolean expression");
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
 * @c provsql column is assigned -1 so callers can detect it.  Join-RTE
 * columns and empty-named columns (used for anonymous GROUP BY keys) are
 * assigned 0.
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
 * @brief Move WHERE conditions on aggregate results (@c agg_token) to HAVING.
 *
 * Supported patterns (moved to HAVING):
 * - The entire WHERE is a supported agg comparison.
 * - The WHERE is a top-level AND where some conjuncts reference aggregates
 *   (those are extracted individually) and the rest remain in WHERE.
 *
 * Unsupported patterns (e.g., "WHERE x=1 OR c>3") raise an error.
 *
 * @param constants  Extension OID cache.
 * @param q          Query to modify in place (@c jointree->quals and
 *                   @c havingQual may both be updated).
 */
static void migrate_aggtoken_quals_to_having(const constants_t *constants,
                                             Query *q) {
  if (!q->jointree || !q->jointree->quals)
    return;

  if (!has_aggtoken(q->jointree->quals, constants))
    return;

  /*
   * We support WHERE clauses that are (possibly trivial) AND conjunctions of:
   * - Conditions that do not mention aggregates (kept in WHERE).
   * - Arbitrary Boolean combinations that all refer to aggregates and that
   *   check_expr_on_aggregate accepts (moved to HAVING).
   * Other forms (e.g., "WHERE x=1 OR c>3") are not supported.
   */
  if (check_expr_on_aggregate((Expr *)q->jointree->quals, constants)) {
    /* Entire WHERE is an agg comparison — move it wholesale to HAVING */
    q->havingQual =
      add_to_havingQual(q->havingQual, (Expr *)q->jointree->quals);
    q->jointree->quals = NULL;
  } else if (IsA(q->jointree->quals, BoolExpr)) {
    BoolExpr *be = (BoolExpr *)q->jointree->quals;
    if (be->boolop == AND_EXPR) {
      /* Split the AND: move agg conjuncts to HAVING, leave the rest */
      ListCell *cell, *prev;
      for (cell = list_head(be->args), prev = NULL; cell != NULL;) {
        if (has_aggtoken(lfirst(cell), constants)) {
          Expr *expr = (Expr *)lfirst(cell);

          if (check_expr_on_aggregate(expr, constants)) {
            be->args = my_list_delete_cell(be->args, cell, prev);
            if (prev)
              cell = my_lnext(be->args, prev);
            else
              cell = list_head(be->args);

            q->havingQual = add_to_havingQual(q->havingQual, expr);
          } else {
            elog(ERROR, "Complex selection on aggregation results not supported by ProvSQL");
          }
        } else {
          prev = cell;
          cell = my_lnext(be->args, cell);
        }
      }
    } else {
      elog(ERROR, "Complex selection on aggregation results not supported by ProvSQL");
    }
  } else {
    elog(ERROR, "ProvSQL: Unknown structure within Boolean expression");
  }
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
    elog_node_display(NOTICE, "Before ProvSQL query rewriting", q, true);

  if (q->rtable == NULL) {
    // No FROM clause, we can skip this query
    return NULL;
  }

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

    // ereport(NOTICE, (errmsg("Before: %s",nodeToString(q))));

    if (q->setOperations) {
      // TODO: Nest set operations as subqueries in FROM,
      // so that we only do set operations on base tables

      SetOperationStmt *stmt = (SetOperationStmt *)q->setOperations;
      if (!stmt->all) {
        q = rewrite_non_all_into_external_group_by(q);
        return process_query(constants, q, removed);
      }
    }

    if (q->hasAggs) {
      Query *subq;

      subq = check_for_agg_distinct(q);
      if (subq) // agg distinct detected, create a subquery
      {
        q = rewrite_for_agg_distinct(q, subq);
        return process_query(constants, q, removed);
      }
    }

    // get_provenance_attributes will also recursively process subqueries
    // by calling process_query
    prov_atts = get_provenance_attributes(constants, q);

    if (prov_atts == NIL)
      return q;

    if (q->hasSubLinks) {
      ereport(ERROR,
              (errmsg("Subqueries in WHERE clause not supported by provsql")));
      supported = false;
    }

    if (supported && q->distinctClause) {
      if (q->hasDistinctOn) {
        ereport(ERROR, (errmsg("DISTINCT ON not supported by provsql")));
        supported = false;
      } else if (list_length(q->distinctClause) < list_length(q->targetList)) {
        ereport(ERROR, (errmsg("Inconsistent DISTINCT and GROUP BY clauses not "
                               "supported by provsql")));
        supported = false;
      } else {
        transform_distinct_into_group_by(q);
      }
    }

    if (supported && q->setOperations) {
      SetOperationStmt *stmt = (SetOperationStmt *)q->setOperations;

      if (stmt->op == SETOP_UNION) {
        process_set_operation_union(constants, stmt);
        has_union = true;
      } else if (stmt->op == SETOP_EXCEPT) {
        if (!transform_except_into_join(constants, q))
          supported = false;
        has_difference = true;
      } else {
        ereport(ERROR, (errmsg("Set operations other than UNION and EXCEPT not "
                               "supported by provsql")));
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
        ereport(
          ERROR,
          (errmsg("GROUPING SETS, CUBE, and ROLLUP not supported by provsql")));
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
                elog(ERROR, "ORDER BY on the result of an aggregate function is "
                     "not supported by ProvSQL");
              break;
            }
          }
        }
      }

      /* Move any WHERE comparisons on aggregate results to HAVING */
      migrate_aggtoken_quals_to_having(constants, q);

      provenance = make_provenance_expression(
        constants, q, prov_atts, q->hasAggs, group_by_rewrite,
        has_union ? SR_PLUS : (has_difference ? SR_MONUS : SR_TIMES), columns,
        nbcols);

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
    elog_node_display(NOTICE, "After ProvSQL query rewriting", q, true);

  return q;
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
 */
static PlannedStmt *provsql_planner(Query *q,
#if PG_VERSION_NUM >= 130000
                                    const char *query_string,
#endif
                                    int cursorOptions,
                                    ParamListInfo boundParams) {
  if (q->commandType == CMD_SELECT && q->rtable) {
    const constants_t constants = get_constants(false);

    if (constants.ok && has_provenance(&constants, q)) {
      bool *removed = NULL;
      Query *new_query;
      clock_t begin = 0;

      if (provsql_verbose >= 40)
        begin = clock();

      new_query = process_query(&constants, q, &removed);

      if (provsql_verbose >= 40)
        ereport(NOTICE, (errmsg("planner time spent=%f",
                                (double)(clock() - begin) / CLOCKS_PER_SEC)));

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
 * @brief Extension initialization — called once when the shared library is loaded.
 *
 * Registers the four GUC variables (@c provsql.active, @c where_provenance,
 * @c update_provenance, @c verbose_level), installs the planner hook and
 * shared-memory hooks, and launches the background MMap worker.
 *
 * Must be loaded via @c shared_preload_libraries; raises an error otherwise.
 */
void _PG_init(void) {
  if (!process_shared_preload_libraries_in_progress)
    elog(ERROR, "provsql needs to be added to the shared_preload_libraries "
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
