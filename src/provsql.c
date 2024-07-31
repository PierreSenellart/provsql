#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pg_config.h"
#include <time.h>
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/print.h"
#include "optimizer/planner.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "utils/guc.h"

#include "provsql_utils.h"
#include "provsql_shmem.h"
#include "provsql_mmap.h"

#if PG_VERSION_NUM < 90600
#error "ProvSQL requires PostgreSQL version 9.6 or later"
#endif

#include "compatibility.h"

PG_MODULE_MAGIC;

bool provsql_interrupted = false;
bool provsql_where_provenance = false;
int provsql_verbose = 100;

static const char *PROVSQL_COLUMN_NAME = "provsql";

extern void _PG_init(void);
extern void _PG_fini(void);

static planner_hook_type prev_planner = NULL;

static Query *process_query(
  const constants_t *constants,
  Query *q,
  bool **removed);

static Var *make_provenance_attribute(const constants_t *constants, Query *q, RangeTblEntry *r, Index relid, AttrNumber attid) {
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
  if(r->perminfoindex != 0) {
    RTEPermissionInfo *rpi = list_nth_node(RTEPermissionInfo, q->rteperminfos, r->perminfoindex - 1);
    rpi->selectedCols = bms_add_member(rpi->selectedCols,
                                       attid - FirstLowInvalidHeapAttributeNumber);
  }
#else
  r->selectedCols = bms_add_member(r->selectedCols, attid - FirstLowInvalidHeapAttributeNumber);
#endif

  return v;
}

typedef struct reduce_varattno_mutator_context
{
  Index varno;
  int *offset;
} reduce_varattno_mutator_context;

static Node *reduce_varattno_mutator(Node *node, reduce_varattno_mutator_context *context)
{
  if(node==NULL)
    return NULL;

  if(IsA(node, Var)) {
    Var *v= (Var *)node;

    if(v->varno == context->varno) {
      v->varattno += context->offset[v->varattno-1];
    }
  }

  return expression_tree_mutator(node, reduce_varattno_mutator, (void*) context);
}

static void reduce_varattno_by_offset(List *targetList, Index varno, int *offset)
{
  ListCell *lc;
  reduce_varattno_mutator_context context = {varno, offset};

  foreach (lc, targetList)
  {
    Node *te = lfirst(lc);
    expression_tree_mutator(te, reduce_varattno_mutator, (void*) &context);
  }
}

typedef struct aggregation_type_mutator_context
{
  Index varno;
  Index varattno;
  const constants_t *constants;
} aggregation_type_mutator_context;

static Node *aggregation_type_mutator(Node *node, aggregation_type_mutator_context *context)
{
  if (node == NULL)
    return NULL;

  if (IsA(node, Var))
  {
    Var *v = (Var *)node;

    if (v->varno == context->varno && v->varattno == context->varattno)
    {
      v->vartype = context->constants->OID_TYPE_AGG_TOKEN;
    }
  }

  return expression_tree_mutator(node, aggregation_type_mutator, (void *)context);
}

static void fix_type_of_aggregation_result(const constants_t *constants, Query *q, Index rteid, List *targetList)
{
  ListCell *lc;
  aggregation_type_mutator_context context = {0, 0, constants};
  Index attno = 1;

  foreach (lc, targetList)
  {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    if(IsA(te->expr, FuncExpr)) {
      FuncExpr *f = (FuncExpr *) te->expr;

      if(f->funcid == constants->OID_FUNCTION_PROVENANCE_AGGREGATE) {
        context.varno = rteid;
        context.varattno = attno;
        query_tree_mutator(q, aggregation_type_mutator, &context, QTW_DONT_COPY_QUERY | QTW_IGNORE_RC_SUBQUERIES);
      }
    }
    ++attno;
  }
}

static List *get_provenance_attributes(const constants_t *constants, Query *q)
{
  List *prov_atts=NIL;
  ListCell *l;
  Index rteid = 1;

  foreach (l, q->rtable)
  {
    RangeTblEntry *r = (RangeTblEntry *)lfirst(l);

    if (r->rtekind == RTE_RELATION)
    {
      ListCell *lc;
      AttrNumber attid = 1;

      foreach (lc, r->eref->colnames)
      {
        const char *v = strVal(lfirst(lc));

        if(!strcmp(v,PROVSQL_COLUMN_NAME) &&
           get_atttype(r->relid,attid)==constants->OID_TYPE_UUID) {
          prov_atts = lappend(prov_atts, make_provenance_attribute(constants, q, r, rteid, attid));
        }

        ++attid;
      }
    } else if(r->rtekind == RTE_SUBQUERY) {
      bool *inner_removed = NULL;
      int old_targetlist_length=r->subquery->targetList->length;
      Query *new_subquery = process_query(constants, r->subquery, &inner_removed);
      if(new_subquery != NULL) {
        int i=0;
        int *offset = (int *)palloc(old_targetlist_length * sizeof(int));
        unsigned varattnoprovsql;
        ListCell *cell, *prev;

        r->subquery = new_subquery;

        if(inner_removed != NULL) {
          for (cell = list_head(r->eref->colnames), prev = NULL;
               cell != NULL;)
          {
            if(inner_removed[i]) {
              r->eref->colnames = my_list_delete_cell(r->eref->colnames, cell, prev);
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
          for(i=0; i<old_targetlist_length; ++i) {
            offset[i] = (i==0?0:offset[i-1]) - (inner_removed[i]?1:0);
          }

          reduce_varattno_by_offset(q->targetList, rteid, offset);
        }

        varattnoprovsql=0;
        for(cell = list_head(new_subquery->targetList); cell!=NULL; cell=my_lnext(new_subquery->targetList, cell)) {
          TargetEntry *te = (TargetEntry*) lfirst(cell);
          ++varattnoprovsql;
          if(!strcmp(te->resname,PROVSQL_COLUMN_NAME))
            break;
        }

        if(cell!=NULL) {
          r->eref->colnames = lappend(r->eref->colnames, makeString(pstrdup(PROVSQL_COLUMN_NAME)));
          prov_atts=lappend(prov_atts,make_provenance_attribute(constants, q, r, rteid, varattnoprovsql));
        }
        fix_type_of_aggregation_result(constants, q, rteid, r->subquery->targetList);
      }
    }
    else if (r->rtekind == RTE_JOIN)
    {
      if (r->jointype == JOIN_INNER ||
          r->jointype == JOIN_LEFT ||
          r->jointype == JOIN_FULL ||
          r->jointype == JOIN_RIGHT)
      {
        // Nothing to do, there will also be RTE entries for the tables
        // that are part of the join, from which we will extract the
        // provenance information
      }
      else
      {   // Semijoin (should be feasible, but check whether the second provenance information is available)
          // Antijoin (feasible with negation)
        ereport(ERROR, (errmsg("JOIN type not supported by provsql")));
      }
    } else if (r->rtekind == RTE_FUNCTION)
    {
      ListCell *lc;
      AttrNumber attid=1;

      foreach(lc,r->functions) {
        RangeTblFunction *func = (RangeTblFunction *) lfirst(lc);

        if(func->funccolcount==1) {
          FuncExpr *expr = (FuncExpr *) func->funcexpr;
          if(expr->funcresulttype == constants->OID_TYPE_UUID
             && !strcmp(get_rte_attribute_name(r,attid),PROVSQL_COLUMN_NAME)) {
            prov_atts=lappend(prov_atts,make_provenance_attribute(constants, q, r, rteid, attid));
          }
        }
        else
        {
          ereport(ERROR, (errmsg("FROM function with multiple output attributes not supported by provsql")));
        }

        attid += func->funccolcount;
      }
    } else if (r->rtekind == RTE_VALUES) {
      // Nothing to do, no provenance attribute in literal values
    } else {
      ereport(ERROR, (errmsg("FROM clause unsupported by provsql")));
    }

    ++rteid;
  }

  return prov_atts;
}

static Bitmapset *remove_provenance_attributes_select(
  const constants_t *constants,
  Query *q,
  bool **removed)
{
  int nbRemoved = 0;
  int i = 0;
  Bitmapset *ressortgrouprefs = NULL;
  ListCell *cell, *prev;
  *removed = (bool *)palloc(q->targetList->length * sizeof(bool));

  for (cell = list_head(q->targetList), prev = NULL;
       cell != NULL;)
  {
    TargetEntry *rt = (TargetEntry *)lfirst(cell);
    (*removed)[i] = false;

    if (rt->expr->type == T_Var)
    {
      Var *v = (Var *)rt->expr;

      if(v->vartype==constants->OID_TYPE_UUID) {
        const char *colname;

        if (rt->resname)
          colname = rt->resname;
        else
        {
          /* This case occurs, for example, when grouping by a column
           * that is projected out */
          RangeTblEntry *r = (RangeTblEntry *)list_nth(q->rtable, v->varno - 1);
          colname = strVal(list_nth(r->eref->colnames, v->varattno - 1));
        }

        if (!strcmp(colname, PROVSQL_COLUMN_NAME))
        {
          q->targetList = my_list_delete_cell(q->targetList, cell, prev);

          (*removed)[i] = true;
          ++nbRemoved;

          if (rt->ressortgroupref > 0)
            ressortgrouprefs = bms_add_member(ressortgrouprefs, rt->ressortgroupref);
        }
      }
    }

    if ((*removed)[i])
    {
      if (prev)
      {
        cell = my_lnext(q->targetList, prev);
      }
      else
      {
        cell = list_head(q->targetList);
      }
    }
    else
    {
      rt->resno -= nbRemoved;
      prev = cell;
      cell = my_lnext(q->targetList, cell);
    }

    ++i;
  }

  return ressortgrouprefs;
}

typedef enum
{
  SR_PLUS,
  SR_MONUS,
  SR_TIMES
} semiring_operation;

/* An OpExpr leads directly to an eq gate.
 * toExpr is the former expression for the provenance.
 * The function returns the new expression with toExpr
 * nested inside the call of the eq function.
 *
 * Note: this function can also be used to handle an OpExpr
 * coming from a WHERE expression. So we need to perform
 * more tests because not all OpExpr are used to express
 * a join in this case */
static Expr *add_eq_from_OpExpr_to_Expr(
  const constants_t *constants,
  OpExpr *fromOpExpr,
  Expr *toExpr,
  int **columns)
{
  Datum first_arg;
  Datum second_arg;
  FuncExpr *fc;
  Const *c1;
  Const *c2;
  Var *v1;
  Var *v2;

  if (my_lnext(fromOpExpr->args, list_head(fromOpExpr->args)))
  {
    /* Sometimes Var is nested within a RelabelType */
    if (IsA(linitial(fromOpExpr->args), Var))
    {
      v1 = linitial(fromOpExpr->args);
    }
    else if (IsA(linitial(fromOpExpr->args), RelabelType))
    {
      /* In the WHERE case it can be a Const */
      RelabelType *rt1 = linitial(fromOpExpr->args);
      if (IsA(rt1->arg, Var))
      {   /* Can be Param in the WHERE case */
        v1 = (Var *)rt1->arg;
      }
      else
        return toExpr;
    }
    else
      return toExpr;
    first_arg = Int16GetDatum(columns[v1->varno - 1][v1->varattno - 1]);

    if (IsA(lsecond(fromOpExpr->args), Var))
    {
      v2 = lsecond(fromOpExpr->args);
    }
    else if (IsA(lsecond(fromOpExpr->args), RelabelType))
    {
      /* In the WHERE case it can be a Const */
      RelabelType *rt2 = lsecond(fromOpExpr->args);
      if (IsA(rt2->arg, Var))
      {   /* Can be Param in the WHERE case */
        v2 = (Var *)rt2->arg;
      }
      else
        return toExpr;
    }
    else
      return toExpr;
    second_arg = Int16GetDatum(columns[v2->varno - 1][v2->varattno - 1]);

    fc = makeNode(FuncExpr);
    fc->funcid=constants->OID_FUNCTION_PROVENANCE_EQ;
    fc->funcvariadic=false;
    fc->funcresulttype=constants->OID_TYPE_UUID;
    fc->location=-1;

    c1=makeConst(
      constants->OID_TYPE_INT,
      -1,
      InvalidOid,
      sizeof(int16),
      first_arg,
      false,
      true);

    c2=makeConst(
      constants->OID_TYPE_INT,
      -1,
      InvalidOid,
      sizeof(int16),
      second_arg,
      false,
      true);

    fc->args=list_make3(toExpr, c1, c2);
    return (Expr *)fc;
  }
  return toExpr;
}

/* This function handles a Quals node.
 *
 * Two cases are possible, one coming from JoinExpr and the other
 * directly from FromExpr.
 * */
static Expr *add_eq_from_Quals_to_Expr(
  const constants_t *constants,
  Node *quals,
  Expr *result,
  int **columns)
{
  OpExpr *oe;

  if (!quals)
    return result;

  if(IsA(quals, OpExpr)) {
    oe = (OpExpr *) quals;
    result = add_eq_from_OpExpr_to_Expr(constants,oe,result,columns);
  }   /* Sometimes OpExpr is nested within a BoolExpr */
  else if (IsA(quals, BoolExpr))
  {
    BoolExpr *be = (BoolExpr *)quals;
    /* In some cases, there can be an OR or a NOT specified with ON clause */
    if (be->boolop == OR_EXPR || be->boolop == NOT_EXPR)
    {
      ereport(ERROR, (errmsg("Boolean operators OR and NOT in a join...on clause are not supported by provsql")));
    } else {
      ListCell *lc2;
      foreach(lc2,be->args) {
        if(IsA(lfirst(lc2),OpExpr)) {
          oe = (OpExpr *) lfirst(lc2);
          result = add_eq_from_OpExpr_to_Expr(constants,oe,result,columns);
        }
      }
    }
  }
  else
  {   /* Handle other cases */
  }
  return result;
}

static Expr *make_aggregation_expression(
  const constants_t *constants,
  Aggref *agg_ref,
  List *prov_atts,
  semiring_operation op)
{
  Expr *result;
  FuncExpr *expr, *expr_s;
  Aggref *agg = makeNode(Aggref);
  FuncExpr *plus = makeNode(FuncExpr);
  TargetEntry *te_inner = makeNode(TargetEntry);
  Const *fn = makeNode(Const);
  Const *typ = makeNode(Const);

  if (op == SR_PLUS)
  {
    result = linitial(prov_atts);
  }
  else
  {
    if (my_lnext(prov_atts, list_head(prov_atts)) == NULL)
    {
      expr = linitial(prov_atts);
    }
    else
    {
      expr = makeNode(FuncExpr);
      if (op == SR_TIMES)
      {
        ArrayExpr *array = makeNode(ArrayExpr);

        expr->funcid = constants->OID_FUNCTION_PROVENANCE_TIMES;
        expr->funcvariadic = true;

        array->array_typeid = constants->OID_TYPE_UUID_ARRAY;
        array->element_typeid = constants->OID_TYPE_UUID;
        array->elements = prov_atts;
        array->location = -1;

        expr->args = list_make1(array);
      }
      else
      {   // SR_MONUS
        expr->funcid = constants->OID_FUNCTION_PROVENANCE_MONUS;
        expr->args = prov_atts;
      }
      expr->funcresulttype = constants->OID_TYPE_UUID;
      expr->location = -1;
    }

    //semimodule function
    expr_s = makeNode(FuncExpr);
    expr_s->funcid = constants->OID_FUNCTION_PROVENANCE_SEMIMOD;
    expr_s->funcresulttype = constants->OID_TYPE_UUID;

    //check the particular case of count
    if(agg_ref->aggfnoid==2803||agg_ref->aggfnoid==2147)   //count(*) or count(arg)
    {
      Const *one = makeConst(constants->OID_TYPE_INT,
                             -1,
                             InvalidOid,
                             sizeof(int32),
                             Int32GetDatum(1),
                             false,
                             true);
      expr_s->args = list_make2(one, expr);
    }
    else
    {
      expr_s->args = list_make2(((TargetEntry *)linitial(agg_ref->args))->expr, expr);
    }

    expr_s->location=-1;

    //aggregating all semirings in an array
    te_inner->resno = 1;
    te_inner->expr = (Expr *)expr_s;
    agg->aggfnoid=constants->OID_FUNCTION_ARRAY_AGG;
    agg->aggtype=constants->OID_TYPE_UUID_ARRAY;
    agg->args=list_make1(te_inner);
    agg->aggkind=AGGKIND_NORMAL;
    agg->location=-1;
#if PG_VERSION_NUM >= 140000
    agg->aggno=agg->aggtransno=-1;
#endif

    agg->aggargtypes = list_make1_oid(constants->OID_TYPE_UUID);

    //final aggregation function
    plus->funcid=constants->OID_FUNCTION_PROVENANCE_AGGREGATE;

    fn = makeConst(constants->OID_TYPE_INT,
                   -1,
                   InvalidOid,
                   sizeof(int32),
                   Int32GetDatum(agg_ref->aggfnoid),
                   false,
                   true);

    typ = makeConst(constants->OID_TYPE_INT,
                    -1,
                    InvalidOid,
                    sizeof(int32),
                    Int32GetDatum(agg_ref->aggtype),
                    false,
                    true);

    plus->funcresulttype=constants->OID_TYPE_AGG_TOKEN;
    plus->args = list_make4(fn, typ, agg_ref, agg);
    plus->location=-1;

    result=(Expr*)plus;
  }

  return result;
}

static Expr *make_provenance_expression(
  const constants_t *constants,
  Query *q,
  List *prov_atts,
  bool aggregation,
  bool group_by_rewrite,
  semiring_operation op,
  int **columns,
  int nbcols)
{
  Expr *result;
  ListCell *lc_v;

  if (op == SR_PLUS) {
    result = linitial(prov_atts);
  } else {
    if (my_lnext(prov_atts, list_head(prov_atts)) == NULL)
    {
      result = linitial(prov_atts);
    } else {
      FuncExpr *expr = makeNode(FuncExpr);
      if (op == SR_TIMES)
      {
        ArrayExpr *array = makeNode(ArrayExpr);

        expr->funcid = constants->OID_FUNCTION_PROVENANCE_TIMES;
        expr->funcvariadic = true;

        array->array_typeid = constants->OID_TYPE_UUID_ARRAY;
        array->element_typeid = constants->OID_TYPE_UUID;
        array->elements = prov_atts;
        array->location = -1;

        expr->args = list_make1(array);
      }
      else
      {   // SR_MONUS
        expr->funcid = constants->OID_FUNCTION_PROVENANCE_MONUS;
        expr->args = prov_atts;
      }
      expr->funcresulttype = constants->OID_TYPE_UUID;
      expr->location = -1;

      result = (Expr*) expr;
    }

    if (group_by_rewrite || aggregation)
    {
      Aggref *agg = makeNode(Aggref);
      FuncExpr *plus = makeNode(FuncExpr);
      TargetEntry *te_inner = makeNode(TargetEntry);

      q->hasAggs = true;

      te_inner->resno = 1;
      te_inner->expr = (Expr *)result;

      agg->aggfnoid=constants->OID_FUNCTION_ARRAY_AGG;
      agg->aggtype=constants->OID_TYPE_UUID_ARRAY;
      agg->args=list_make1(te_inner);
      agg->aggkind=AGGKIND_NORMAL;
      agg->location=-1;
#if PG_VERSION_NUM >= 140000
      agg->aggno=agg->aggtransno=-1;
#endif

      agg->aggargtypes=list_make1_oid(constants->OID_TYPE_UUID);

      plus->funcid=constants->OID_FUNCTION_PROVENANCE_PLUS;
      plus->args=list_make1(agg);
      plus->funcresulttype=constants->OID_TYPE_UUID;
      plus->location=-1;

      result=(Expr*)plus;
    }

    if (aggregation) {
      FuncExpr *deltaExpr = makeNode(FuncExpr);

      //adding the delta gate to the provenance circuit
      deltaExpr->funcid = constants->OID_FUNCTION_PROVENANCE_DELTA;
      deltaExpr->args = list_make1(result);
      deltaExpr->funcresulttype = constants->OID_TYPE_UUID;
      deltaExpr->location = -1;

      result = (Expr *)deltaExpr;
    }
  }

  /* Part to handle eq gates used for where-provenance.
   * Placed before projection gates because they need
   * to be deeper in the provenance tree. */
  if (provsql_where_provenance && q->jointree)
  {
    ListCell *lc;
    foreach(lc, q->jointree->fromlist) {
      if(IsA(lfirst(lc), JoinExpr)) {
        JoinExpr *je = (JoinExpr *) lfirst(lc);
        /* Study equalities coming from From clause */
        result = add_eq_from_Quals_to_Expr(constants, je->quals, result, columns);
      }
    }
    /* Study equalities coming from WHERE clause */
    result = add_eq_from_Quals_to_Expr(constants, q->jointree->quals, result, columns);
  }

  if(provsql_where_provenance) {
    ArrayExpr *array=makeNode(ArrayExpr);
    FuncExpr *fe=makeNode(FuncExpr);
    bool projection=false;
    int nb_column=0;

    fe->funcid=constants->OID_FUNCTION_PROVENANCE_PROJECT;
    fe->funcvariadic=true;
    fe->funcresulttype=constants->OID_TYPE_UUID;
    fe->location=-1;

    array->array_typeid=constants->OID_TYPE_INT_ARRAY;
    array->element_typeid=constants->OID_TYPE_INT;
    array->elements=NIL;
    array->location=-1;

    foreach(lc_v, q->targetList) {
      TargetEntry *te_v = (TargetEntry *) lfirst(lc_v);
      if(IsA(te_v->expr, Var)) {
        Var *vte_v = (Var *) te_v->expr;
        RangeTblEntry *rte_v = (RangeTblEntry *) lfirst(list_nth_cell(q->rtable, vte_v->varno-1));
        int value_v;
        /* Check if this targetEntry references a column in a RTE of type RTE_JOIN */
        if (rte_v->rtekind != RTE_JOIN)
        {
          value_v = columns[vte_v->varno - 1][vte_v->varattno - 1];
        }
        else
        {   // is a join
          Var *jav_v = (Var *)lfirst(list_nth_cell(rte_v->joinaliasvars, vte_v->varattno - 1));
          value_v = columns[jav_v->varno - 1][jav_v->varattno - 1];
        }
        /* If this is a valid column */
        if(value_v > 0) {
          Const *ce=makeConst(constants->OID_TYPE_INT,
                              -1,
                              InvalidOid,
                              sizeof(int32),
                              Int32GetDatum(value_v),
                              false,
                              true);

          array->elements=lappend(array->elements, ce);

          if(value_v!=++nb_column)
            projection=true;
        } else {
          if(value_v!=-1)
            projection=true;
        }
      } else {   // we have a function in target
        Const *ce=makeConst(constants->OID_TYPE_INT,
                            -1,
                            InvalidOid,
                            sizeof(int32),
                            Int32GetDatum(0),
                            false,
                            true);

        array->elements=lappend(array->elements, ce);
        projection=true;
      }
    }

    if (nb_column != nbcols)
      projection = true;

    if (projection)
    {
      fe->args = list_make2(result, array);
      result = (Expr *)fe;
    }
    else
    {
      pfree(array);
      pfree(fe);
    }
  }

  return result;
}

static Query* rewrite_for_agg_distinct(Query *q, Query *subq){
  //variables
  Alias *alias = makeNode(Alias);
  Alias *eref = makeNode(Alias);
  FromExpr *jointree = makeNode(FromExpr);
  RangeTblEntry *rte = makeNode(RangeTblEntry);
  RangeTblRef *rtr = makeNode(RangeTblRef);
  ListCell *lc_v;
  int groupRef = 1;
  //rewrite the rtable to contain only one relation, the alias
  alias->aliasname = "a";
  eref->aliasname = "a";
  eref->colnames = NIL;
  foreach(lc_v, q->targetList)
  {
    TargetEntry *te_v = (TargetEntry *)lfirst(lc_v);
    eref->colnames = lappend(eref->colnames,makeString(pstrdup(te_v->resname)));
#if PG_VERSION_NUM < 160000
    // For PG_VERSION_NUM >= 160000, rte->perminfoindex==0 so no need to
    // care about permissions
    rte->selectedCols = bms_add_member(rte->selectedCols, te_v->resno - FirstLowInvalidHeapAttributeNumber);
#endif
  }
  rte->alias = alias;
  rte->eref = eref;
  rte->rtekind = RTE_SUBQUERY;
  rte->subquery = subq;

  q->rtable = list_make1(rte);
  q->groupClause = NIL;
  //correct var indexes and group by references
  foreach(lc_v, q->targetList)
  {
    TargetEntry *te_v = (TargetEntry *)lfirst(lc_v);
    Var *var = makeNode(Var);
    var->varno = 1;
    var->varattno = te_v->resno;
    if (IsA(te_v->expr, Aggref))
    {
      Aggref *ar_v = (Aggref *)te_v->expr;
      TargetEntry *te_new = makeNode(TargetEntry);
      var->vartype = linitial_oid(ar_v->aggargtypes);
      te_new->resno = 1;
      te_new->expr = (Expr*) var;
      ar_v->args = list_make1(te_new);
      ar_v->aggdistinct = NIL;
    }
    else if (IsA(te_v->expr, Var))
    {
      Var *var_v = (Var *)te_v->expr;
      var_v->varno = 1;
      var_v->varattno = te_v->resno;
    }
    else {
      var->vartype = exprType((Node *)te_v->expr);
      te_v->expr = (Expr*) var;
    }
    //add to GROUP BY list
    if (!IsA(te_v->expr, Aggref))
    {
      SortGroupClause *sgc = makeNode(SortGroupClause);
      sgc->tleSortGroupRef = groupRef;
      te_v->ressortgroupref = groupRef;
      sgc->nulls_first = false;
      get_sort_group_operators(exprType((Node *)te_v->expr), true, true, false, &sgc->sortop, &sgc->eqop, NULL, &sgc->hashable);
      q->groupClause = lappend(q->groupClause,sgc);
      groupRef++;
    }
  }
  //rewrite the jointree to contain only one relation
  rtr->rtindex = 1;
  jointree->fromlist = list_make1(rtr);
  q->jointree = jointree;
  return q;
}

static Query *check_for_agg_distinct(Query *q){
  ListCell *lc_v;
  List* lst_v = NIL;
  Query* new_q = copyObject(q);
  unsigned char found = 0;

  //replace each Aggref with a TargetEntry calling the agg function
  //-- only in the top-level of the query
  foreach (lc_v, new_q->targetList)
  {
    TargetEntry *te_v = (TargetEntry *)lfirst(lc_v);
    if (IsA(te_v->expr, Aggref))
    {
      Aggref *ar_v = (Aggref *)te_v->expr;
      if (list_length(ar_v->aggdistinct)>0) {
        TargetEntry *te_new = NULL;
        SortGroupClause *sgc = (SortGroupClause*) linitial(ar_v->aggdistinct);

        found = 1;
        //the agg distinct clause is added to the GROUP BY clause
        //remove aggref and replace by its arguments
        te_new = (TargetEntry *)linitial(ar_v->args);
        sgc->tleSortGroupRef = te_v->resno;
        new_q->groupClause = lappend(new_q->groupClause, sgc);
        te_new->resno = te_v->resno;
        te_new->resname = te_v->resname;
        te_new->ressortgroupref = te_v->resno;
        lst_v = lappend(lst_v, te_new);
      }
      else {
        lst_v = lappend(lst_v, ar_v);
      }
    } else {   //keep the current TE
      lst_v = lappend(lst_v, te_v);
    }
  }
  if(lst_v!=NIL) new_q->targetList = lst_v;
  if(!found) return NULL;
  else return new_q;
}

typedef struct aggregation_mutator_context
{
  List *prov_atts;
  semiring_operation op;
  const constants_t* constants;
} aggregation_mutator_context;

static Node *aggregation_mutator(Node *node, aggregation_mutator_context *context)
{
  if (node == NULL)
    return NULL;

  if (IsA(node, Aggref))
  {
    Aggref *ar_v = (Aggref *) node;
    return (Node *) make_aggregation_expression(
      context->constants,
      ar_v,
      context->prov_atts,
      context->op);
  }

  return expression_tree_mutator(node, aggregation_mutator, (void *)context);
}

static void replace_aggregations_in_select(
  const constants_t *constants,
  Query *q,
  List *prov_atts,
  semiring_operation op)
{

  aggregation_mutator_context context = {prov_atts, op, constants};

  query_tree_mutator(q, aggregation_mutator, &context, QTW_DONT_COPY_QUERY | QTW_IGNORE_RT_SUBQUERIES);
}

static void add_to_select(
  Query *q,
  Expr *provenance)
{
  TargetEntry *newte = makeNode(TargetEntry);
  bool inserted=false;
  unsigned resno=0;

  newte->expr = provenance;
  newte->resname = (char *)PROVSQL_COLUMN_NAME;

  if (IsA(provenance, Var)) {
    RangeTblEntry *rte = list_nth(q->rtable, ((Var*) provenance)->varno - 1);
    newte->resorigtbl = rte->relid;
    newte->resorigcol = ((Var*) provenance)->varattno;;
  }

  /* Make sure to insert before all resjunk Target Entry */
  for (ListCell *cell = list_head(q->targetList); cell != NULL;)
  {
    TargetEntry *te = (TargetEntry*) lfirst(cell);

    if(!inserted)
      ++resno;

    if(te->resjunk) {
      if(!inserted) {
        newte->resno=resno;
        q->targetList = list_insert_nth(q->targetList, resno-1, newte);
        cell=list_nth_cell(q->targetList, resno);
        te = (TargetEntry*) lfirst(cell);
        inserted=true;
      }

      ++te->resno;
    }

    cell = my_lnext(q->targetList, cell);
  }

  if(!inserted) {
    newte->resno = resno+1;
    q->targetList = lappend(q->targetList, newte);
  }
}

typedef struct provenance_mutator_context
{
  Expr *provsql;
  const constants_t *constants;
} provenance_mutator_context;

static Node *provenance_mutator(Node *node, provenance_mutator_context *context)
{
  if (node == NULL)
    return NULL;

  if (IsA(node, FuncExpr)) {
    FuncExpr *f = (FuncExpr *)node;

    if(f->funcid == context->constants->OID_FUNCTION_PROVENANCE) {
      return (Node*) copyObject(context->provsql);
    }
  } else if(IsA(node, RangeTblEntry) || IsA(node, RangeTblFunction)) {
    // A provenance() expression in a From (not within a subquery) is
    // non-sensical
    return node;
  }

  return expression_tree_mutator(node, provenance_mutator, (void *)context);
}

static void replace_provenance_function_by_expression(const constants_t *constants, Query *q, Expr *provsql)
{
  provenance_mutator_context context = {provsql, constants};

  query_tree_mutator(q, provenance_mutator, &context, QTW_DONT_COPY_QUERY | QTW_IGNORE_RT_SUBQUERIES);
}

static void transform_distinct_into_group_by(Query *q)
{
  // First check which are already in the group by clause
  // Should be either none or all as "SELECT DISTINCT a, b ... GROUP BY a"
  // is invalid
  Bitmapset *already_in_group_by = NULL;
  ListCell *lc;
  foreach (lc, q->groupClause)
  {
    SortGroupClause *sgc = (SortGroupClause *)lfirst(lc);
    already_in_group_by = bms_add_member(already_in_group_by, sgc->tleSortGroupRef);
  }

  foreach (lc, q->distinctClause)
  {
    SortGroupClause *sgc = (SortGroupClause *)lfirst(lc);
    if (!bms_is_member(sgc->tleSortGroupRef, already_in_group_by))
    {
      q->groupClause = lappend(q->groupClause, sgc);
    }
  }

  q->distinctClause = NULL;
}

static void remove_provenance_attribute_groupref(Query *q, const Bitmapset *removed_sortgrouprefs)
{
  List **lists[3] = {&q->groupClause, &q->distinctClause, &q->sortClause};
  int i = 0;

  for (i = 0; i < 3; ++i)
  {
    ListCell *cell, *prev;

    for (cell = list_head(*lists[i]), prev = NULL;
         cell != NULL;)
    {
      SortGroupClause *sgc = (SortGroupClause *)lfirst(cell);
      if (bms_is_member(sgc->tleSortGroupRef, removed_sortgrouprefs))
      {
        *lists[i] = my_list_delete_cell(*lists[i], cell, prev);

        if (prev)
        {
          cell = my_lnext(*lists[i], prev);
        }
        else
        {
          cell = list_head(*lists[i]);
        }
      }
      else
      {
        prev = cell;
        cell = my_lnext(*lists[i], cell);
      }
    }
  }
}

static void remove_provenance_attribute_setoperations(Query *q, bool *removed)
{
  SetOperationStmt *so = (SetOperationStmt *)q->setOperations;
  List **lists[3] = {&so->colTypes, &so->colTypmods, &so->colCollations};
  int i = 0;

  for (i = 0; i < 3; ++i)
  {
    ListCell *cell, *prev;
    int j;

    for (cell = list_head(*lists[i]), prev = NULL, j = 0;
         cell != NULL;
         ++j)
    {
      if (removed[j])
      {
        *lists[i] = my_list_delete_cell(*lists[i], cell, prev);

        if (prev)
        {
          cell = my_lnext(*lists[i], prev);
        }
        else
        {
          cell = list_head(*lists[i]);
        }
      }
      else
      {
        prev = cell;
        cell = my_lnext(*lists[i], cell);
      }
    }
  }
}

static Query *rewrite_non_all_into_external_group_by(Query *q)
{
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

  foreach (lc, new_query->targetList)
  {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    SortGroupClause *sgc = makeNode(SortGroupClause);

    sgc->tleSortGroupRef = te->ressortgroupref = ++sortgroupref;

    get_sort_group_operators(exprType((Node *)te->expr), false, true, false, &sgc->sortop, &sgc->eqop, NULL, &sgc->hashable);

    new_query->groupClause = lappend(new_query->groupClause, sgc);
  }

  return new_query;
}

static bool provenance_function_walker(
  Node *node, void *data)
{
  const constants_t *constants = (const constants_t*) data;
  if (node == NULL)
    return false;

  if (IsA(node, FuncExpr))
  {
    FuncExpr *f = (FuncExpr *)node;

    if(f->funcid == constants->OID_FUNCTION_PROVENANCE)
      return true;
  }

  return expression_tree_walker(node, provenance_function_walker, data);
}

static bool provenance_function_in_group_by(
  const constants_t *constants,
  Query *q)
{
  ListCell *lc;
  foreach (lc, q->targetList)
  {
    TargetEntry *te = (TargetEntry *)lfirst(lc);
    if (te->ressortgroupref > 0 &&
        expression_tree_walker((Node*)te, provenance_function_walker, (void*) constants)) {
      return true;
    }
  }

  return false;
}

static bool has_provenance_walker(
  Node *node,
  void *data) {
  const constants_t *constants=(const constants_t *) data;
  if(node==NULL)
    return false;

  if (IsA(node, Query))
  {
    Query *q = (Query *)node;
    ListCell *rc;

    if(query_tree_walker(q, has_provenance_walker, data, 0))
      return true;

    foreach (rc, q->rtable)
    {
      RangeTblEntry *r = (RangeTblEntry *)lfirst(rc);
      if (r->rtekind == RTE_RELATION)
      {
        ListCell *lc;
        AttrNumber attid = 1;

        foreach (lc, r->eref->colnames)
        {
          const char *v = strVal(lfirst(lc));

          if(!strcmp(v,PROVSQL_COLUMN_NAME) &&
             get_atttype(r->relid,attid)==constants->OID_TYPE_UUID) {
            return true;
          }

          ++attid;
        }
      }
      else if (r->rtekind == RTE_FUNCTION)
      {
        ListCell *lc;
        AttrNumber attid = 1;

        foreach (lc, r->functions)
        {
          RangeTblFunction *func = (RangeTblFunction *)lfirst(lc);

          if(func->funccolcount==1) {
            FuncExpr *expr = (FuncExpr *) func->funcexpr;
            if(expr->funcresulttype == constants->OID_TYPE_UUID
               && !strcmp(get_rte_attribute_name(r,attid),PROVSQL_COLUMN_NAME)) {
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

static bool has_provenance(
  const constants_t *constants,
  Query *q) {
  return has_provenance_walker((Node *) q, (void*) constants);
}

static bool transform_except_into_join(
  const constants_t *constants,
  Query *q) {
  SetOperationStmt *setOps = (SetOperationStmt *) q->setOperations;
  RangeTblEntry *rte = makeNode(RangeTblEntry);
  FromExpr *fe = makeNode(FromExpr);
  JoinExpr *je = makeNode(JoinExpr);
  BoolExpr *expr = makeNode(BoolExpr);
  ListCell *lc;
  int attno = 1;

  if (!IsA(setOps->larg, RangeTblRef) || !IsA(setOps->rarg, RangeTblRef))
  {
    ereport(ERROR, (errmsg("Unsupported chain of EXCEPT operations")));
  }

  expr->boolop = AND_EXPR;
  expr->location = -1;
  expr->args = NIL;

  foreach (lc, q->targetList)
  {
    TargetEntry *te = (TargetEntry *)lfirst(lc);

    Var *v = (Var *)te->expr;

    if(v->vartype != constants->OID_TYPE_UUID) {
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

  je->jointype = JOIN_FULL;

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

// This function explores the tree of SetOperationStmt of an union to
// add the provenance information and to set the union mode to "all"
// on all nodes (terms have been previously treated by
// rewrite_non_all_into_external_group_by)
static void process_set_operation_union(
  const constants_t *constants,
  SetOperationStmt * stmt,
  bool * supported)
{
  if (stmt->op != SETOP_UNION)
  {
    supported = false;
    ereport(ERROR, (errmsg("Unsupported mixed set operations")));
  }
  if(IsA(stmt->larg,SetOperationStmt)) {
    process_set_operation_union(constants, (SetOperationStmt*)(stmt->larg), supported);
  }
  if(IsA(stmt->rarg,SetOperationStmt)) {
    process_set_operation_union(constants, (SetOperationStmt*)(stmt->rarg), supported);
  }
  stmt->colTypes=lappend_oid(stmt->colTypes,
                             constants->OID_TYPE_UUID);
  stmt->colTypmods=lappend_int(stmt->colTypmods, -1);
  stmt->colCollations=lappend_int(stmt->colCollations, 0);
  stmt->all = true;
}

static void add_select_non_zero(
  const constants_t *constants,
  Query *q,
  Expr *provsql)
{
  FuncExpr *gate_zero = makeNode(FuncExpr);
  OpExpr *oe = makeNode(OpExpr);

  gate_zero->funcid = constants->OID_FUNCTION_GATE_ZERO;
  gate_zero->funcresulttype = constants->OID_TYPE_UUID;

  oe->opno = constants->OID_OPERATOR_NOT_EQUAL_UUID;
  oe->opfuncid = constants->OID_FUNCTION_NOT_EQUAL_UUID;
  oe->opresulttype = BOOLOID;
  oe->args = list_make2(provsql, gate_zero);
  oe->location = -1;

  if(q->jointree->quals != NULL)
  {
    BoolExpr *be = makeNode(BoolExpr);

    be->boolop = AND_EXPR;
    be->args = list_make2(oe, q->jointree->quals);
    be->location = -1;

    q->jointree->quals = (Node*) be;
  } else
    q->jointree->quals = (Node*) oe;
}

static Query *process_query(
  const constants_t *constants,
  Query *q,
  bool **removed)
{
  List *prov_atts;
  bool has_union = false;
  bool has_difference = false;
  bool supported=true;
  bool group_by_rewrite = false;
  int nbcols=0;
  int **columns;
  unsigned i=0;

  if(provsql_verbose>=50)
    elog_node_display(NOTICE, "Before ProvSQL query rewriting", q, true);

  if(q->rtable == NULL) {
    // No FROM clause, we can skip this query
    return NULL;
  }

  columns=(int **) palloc(q->rtable->length*sizeof(int*));

//ereport(NOTICE, (errmsg("Before: %s",nodeToString(q))));

  if (q->setOperations)
  {
    // TODO: Nest set operations as subqueries in FROM,
    // so that we only do set operations on base tables

    SetOperationStmt *stmt = (SetOperationStmt *)q->setOperations;
    if (!stmt->all)
    {
      q = rewrite_non_all_into_external_group_by(q);
      return process_query(constants, q, removed);
    }
  }

  if (q->hasAggs) {
    Query *subq;

    if (q->havingQual) {
      elog(ERROR, "Non-terminal aggregates not supported by ProvSQL");
    }

    subq = check_for_agg_distinct(q);
    if(subq)   // agg distinct detected, create a subquery
    {
      q = rewrite_for_agg_distinct(q,subq);
      return process_query(constants, q, removed);
    }
  }

  // get_provenance_attributes will also recursively process subqueries
  // by calling process_query
  prov_atts=get_provenance_attributes(constants, q);

  if (prov_atts == NIL)
    return q;

  {
    Bitmapset *removed_sortgrouprefs = NULL;

    if(q->targetList) {
      removed_sortgrouprefs=remove_provenance_attributes_select(constants, q, removed);
      if(removed_sortgrouprefs != NULL)
        remove_provenance_attribute_groupref(q, removed_sortgrouprefs);
      if (q->setOperations)
        remove_provenance_attribute_setoperations(q, *removed);
    }
  }

  if (q->hasSubLinks)
  {
    ereport(ERROR, (errmsg("Subqueries in WHERE clause not supported by provsql")));
    supported = false;
  }

  if (supported && q->distinctClause)
  {
    if (q->hasDistinctOn)
    {
      ereport(ERROR, (errmsg("DISTINCT ON not supported by provsql")));
      supported = false;
    }
    else if (list_length(q->distinctClause) < list_length(q->targetList))
    {
      ereport(ERROR, (errmsg("Inconsistent DISTINCT and GROUP BY clauses not supported by provsql")));
      supported=false;
    } else {
      transform_distinct_into_group_by(q);
    }
  }

  if (supported && q->setOperations)
  {
    SetOperationStmt *stmt = (SetOperationStmt *)q->setOperations;

    if(stmt->op == SETOP_UNION) {
      process_set_operation_union(constants, stmt, &supported);
      has_union = true;
    } else if(stmt->op == SETOP_EXCEPT) {
      if(!transform_except_into_join(constants, q))
        supported=false;
      has_difference=true;
    } else {
      ereport(ERROR, (errmsg("Set operations other than UNION and EXCEPT not supported by provsql")));
      supported = false;
    }
  }

  if(supported &&
     q->groupClause &&
     !provenance_function_in_group_by(constants, q)) {
    group_by_rewrite = true;
  }

  if (supported && q->groupingSets)
  {
    if (q->groupClause ||
        list_length(q->groupingSets) > 1 ||
        ((GroupingSet *)linitial(q->groupingSets))->kind != GROUPING_SET_EMPTY)
    {
      ereport(ERROR, (errmsg("GROUPING SETS, CUBE, and ROLLUP not supported by provsql")));
      supported = false;
    }
    else
    {
      // Simple GROUP BY ()
      group_by_rewrite = true;
    }
  }

  if (supported)
  {
    ListCell *l;

    foreach (l, q->rtable)
    {
      RangeTblEntry *r = (RangeTblEntry *)lfirst(l);
      ListCell *lc;

      columns[i] = 0;
      if (r->eref)
      {
        unsigned j = 0;

        columns[i] = (int *)palloc(r->eref->colnames->length * sizeof(int));

        foreach (lc, r->eref->colnames)
        {
          const char *v=strVal(lfirst(lc));

          if (strcmp(v, "") && r->rtekind != RTE_JOIN)
          {   // TODO: More robust test
              // join RTE columns ignored
            if (!strcmp(v, PROVSQL_COLUMN_NAME))
              columns[i][j] = -1;
            else
              columns[i][j] = ++nbcols;
          }
          else
          {
            columns[i][j] = 0;
          }
          ++j;
        }
      }

      ++i;
    }
  }

  if (supported)
  {
    Expr *provenance;

    if (q->hasAggs)
    {
      ListCell *lc_sort;

      // Compute aggregation expressions
      replace_aggregations_in_select(constants, q, prov_atts,
                                     has_union ? SR_PLUS : (has_difference ? SR_MONUS : SR_TIMES));

      // If there are any sort clauses on something whose type is now
      // aggregate token, we throw an error: sorting aggregation values
      // when provenance is captured is ill-defined
      foreach (lc_sort, q->sortClause) {
        SortGroupClause *sort = (SortGroupClause*) lfirst(lc_sort);
        ListCell *lc_te;
        foreach(lc_te, q->targetList) {
          TargetEntry *te = (TargetEntry*) lfirst(lc_te);
          if(sort->tleSortGroupRef==te->ressortgroupref) {
            if(exprType((Node*) te->expr) == constants->OID_TYPE_AGG_TOKEN)
              elog(ERROR, "ORDER BY on the result of an aggregate function is not supported by ProvSQL");
            break;
          }
        }
      }
    }


    provenance = make_provenance_expression(
      constants,
      q,
      prov_atts,
      q->hasAggs,
      group_by_rewrite,
      has_union ? SR_PLUS : (has_difference ? SR_MONUS : SR_TIMES),
      columns,
      nbcols);

    add_to_select(q,provenance);
    replace_provenance_function_by_expression(constants, q, provenance);

    if(has_difference)
      add_select_non_zero(constants, q, provenance);
  }

  for (i = 0; i < q->rtable->length; ++i)
  {
    if (columns[i])
      pfree(columns[i]);
  }

  if(provsql_verbose>=50)
    elog_node_display(NOTICE, "After ProvSQL query rewriting", q, true);

  return q;
}

static PlannedStmt *provsql_planner(
  Query *q,
#if PG_VERSION_NUM >= 130000
  const char *query_string,
#endif
  int cursorOptions,
  ParamListInfo boundParams)
{
  if(q->commandType==CMD_SELECT && q->rtable) {
    const constants_t constants = get_constants(false);

    if(constants.ok && has_provenance(&constants, q)) {
//        clock_t begin = clock(), end;
//        double time_spent;

      bool *removed = NULL;
      Query *new_query = process_query(&constants, q, &removed);
      if(new_query != NULL)
        q = new_query;

//        end = clock();
//        time_spent = (double)(end - begin) / CLOCKS_PER_SEC;
//        ereport(NOTICE, (errmsg("planner time spent=%f",time_spent)));
    }
  }

  if (prev_planner)
    return prev_planner(
      q,
#if PG_VERSION_NUM >= 130000
      query_string,
#endif
      cursorOptions,
      boundParams);
  else
    return standard_planner(
      q,
#if PG_VERSION_NUM >= 130000
      query_string,
#endif
      cursorOptions,
      boundParams);
}

void _PG_init(void)
{
  if(!process_shared_preload_libraries_in_progress)
    elog(ERROR, "provsql needs to be added to the shared_preload_libraries configuration variable");

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

void _PG_fini(void)
{
  planner_hook = prev_planner;
  shmem_startup_hook = prev_shmem_startup;
}
