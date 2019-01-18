#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pg_config.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/planner.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "utils/guc.h"

#include "provsql_utils.h"

#if PG_VERSION_NUM < 90400
#error "ProvSQL requires PostgreSQL version 9.4 or later"
#endif

PG_MODULE_MAGIC;

bool provsql_shared_library_loaded = false;
bool provsql_interrupted = false;
bool provsql_where_provenance = false;

static const char *PROVSQL_COLUMN_NAME="provsql";

extern void _PG_init(void);
extern void _PG_fini(void);

static planner_hook_type prev_planner = NULL;

static Query *process_query(
    Query *q,
    const constants_t *constants,
    bool subquery);

static RelabelType *make_provenance_attribute(RangeTblEntry *r, Index relid, AttrNumber attid, const constants_t *constants) {
  RelabelType *re=makeNode(RelabelType);
  Var *v=makeNode(Var);
  v->varno=v->varnoold=relid;
  v->varattno=v->varoattno=attid;
  v->vartype=constants->OID_TYPE_PROVENANCE_TOKEN;
  v->varcollid=InvalidOid;
  v->vartypmod=-1;
  v->location=-1;

  re->arg=(Expr*)v;
  re->resulttype=constants->OID_TYPE_UUID;
  re->resulttypmod=-1;
  re->resultcollid=InvalidOid;
  re->relabelformat=COERCE_IMPLICIT_CAST;
  re->location=-1;

  r->selectedCols=bms_add_member(r->selectedCols,attid-FirstLowInvalidHeapAttributeNumber);

  return re;
}

static List *get_provenance_attributes(Query *q, const constants_t *constants) {
  List *prov_atts=NIL;
  ListCell *l;
  Index rteid=1;

  foreach(l, q->rtable) {
    RangeTblEntry *r = (RangeTblEntry *) lfirst(l);

    if(r->rtekind == RTE_RELATION) {
      ListCell *lc;
      AttrNumber attid=1;

      foreach(lc,r->eref->colnames) {
        Value *v = (Value *) lfirst(lc);

        if(!strcmp(strVal(v),PROVSQL_COLUMN_NAME) &&
            get_atttype(r->relid,attid)==constants->OID_TYPE_PROVENANCE_TOKEN) {
          prov_atts=lappend(prov_atts,make_provenance_attribute(r,rteid,attid,constants));
        }

        ++attid;
      }
    } else if(r->rtekind == RTE_SUBQUERY) {
      Query *new_subquery = process_query(r->subquery, constants, true);
      if(new_subquery != NULL) {
        r->subquery = new_subquery;
        r->eref->colnames = lappend(r->eref->colnames, makeString(pstrdup(PROVSQL_COLUMN_NAME)));
        prov_atts=lappend(prov_atts,make_provenance_attribute(r,rteid,new_subquery->targetList->length,constants));
      }
    } else if(r->rtekind == RTE_JOIN) {
      if(r->jointype == JOIN_INNER ||
         r->jointype == JOIN_LEFT ||
         r->jointype == JOIN_FULL ||
         r->jointype == JOIN_RIGHT) {
        // Nothing to do, there will also be RTE entries for the tables
        // that are part of the join, from which we will extract the
        // provenance information
      } else { // Semijoin (should be feasible, but check whether the second provenance information is available)
               // Antijoin (feasible with negation)
       ereport(ERROR, (errmsg("JOIN type not supported by provsql")));
      }
    } else if(r->rtekind == RTE_FUNCTION) {
      ListCell *lc;
      AttrNumber attid=1;

      foreach(lc,r->functions) {
        RangeTblFunction *func = (RangeTblFunction *) lfirst(lc);

        if(func->funccolcount==1) {
          FuncExpr *expr = (FuncExpr *) func->funcexpr;
          if(expr->funcresulttype == constants->OID_TYPE_PROVENANCE_TOKEN
              && !strcmp(get_rte_attribute_name(r,attid),PROVSQL_COLUMN_NAME)) {
            prov_atts=lappend(prov_atts,make_provenance_attribute(r,rteid,attid,constants));
          }
        } else {
          ereport(ERROR, (errmsg("FROM function with multiple output attributes not supported by provsql")));
        }

        attid+=func->funccolcount;
      }
    } else {
      ereport(ERROR, (errmsg("FROM clause unsupported by provsql")));
    }

    ++rteid;
  }

  return prov_atts;
}

static Bitmapset *remove_provenance_attributes_select(
    Query *q,
    const constants_t *constants,
    bool **removed)
{
  int nbRemoved=0;
  int i=0;
  Bitmapset *ressortgrouprefs = NULL;
  ListCell *cell, *prev;
  *removed=(bool*) palloc(q->targetList->length*sizeof(bool));

  for(cell=list_head(q->targetList), prev=NULL;
      cell!=NULL
      ;) {
    TargetEntry *rt = (TargetEntry *) lfirst(cell);
    (*removed)[i]=false;

    if(rt->expr->type==T_Var) {
      Var *v =(Var *) rt->expr;

      if(v->vartype==constants->OID_TYPE_PROVENANCE_TOKEN) {
        const char *colname;

        if(rt->resname)
          colname=rt->resname;
        else {
          /* This case occurs, for example, when grouping by a column
           * that is projected out */
          RangeTblEntry *r = (RangeTblEntry *) list_nth(q->rtable, v->varno-1);
          Value *val = (Value *) list_nth(r->eref->colnames, v->varattno-1);
          colname = strVal(val);
        }

        if(!strcmp(colname,PROVSQL_COLUMN_NAME)) {
          q->targetList=list_delete_cell(q->targetList, cell, prev);

          (*removed)[i]=true;
          ++nbRemoved;

          if(rt->ressortgroupref > 0)
            ressortgrouprefs = bms_add_member(ressortgrouprefs, rt->ressortgroupref);
        }
      }
    }

    if((*removed)[i]) {
      if(prev) {
        cell=lnext(prev);
      }
      else {
        cell=list_head(q->targetList);
      }
    } else {
      rt->resno-=nbRemoved;
      prev=cell;
      cell=lnext(cell);
    }
      
    ++i;
  }

  return ressortgrouprefs;
}

typedef enum { SR_PLUS, SR_MONUS, SR_TIMES } semiring_operation;

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
    OpExpr *fromOpExpr,
    Expr *toExpr,
    int **columns,
    const constants_t *constants)
{
  Datum first_arg;
  Datum second_arg;
  FuncExpr *fc;
  Const *c1;
  Const *c2;
  Var *v1;
  Var *v2;
  
  if(lnext(list_head(fromOpExpr->args))) {
    /* Sometimes Var is nested within a RelabelType */
    if(IsA(linitial(fromOpExpr->args), Var)) {
      v1 = linitial(fromOpExpr->args);  
    } else if (IsA(linitial(fromOpExpr->args), RelabelType)) {
      /* In the WHERE case it can be a Const */
      RelabelType *rt1 = linitial(fromOpExpr->args); 
      if(IsA(rt1->arg, Var)) { /* Can be Param in the WHERE case */
        v1 = (Var *) rt1->arg;
      } else return toExpr;	
    } else return toExpr;
    first_arg = Int16GetDatum(columns[v1->varno-1][v1->varattno-1]);

    if(IsA(lsecond(fromOpExpr->args), Var)) {  
      v2 = lsecond(fromOpExpr->args);  
    } else if (IsA(lsecond(fromOpExpr->args), RelabelType)) {
      /* In the WHERE case it can be a Const */ 
      RelabelType *rt2 = lsecond(fromOpExpr->args); 
      if(IsA(rt2->arg, Var)) { /* Can be Param in the WHERE case */
        v2 = (Var *) rt2->arg;
      }	else return toExpr;
    } else return toExpr;
    second_arg = Int16GetDatum(columns[v2->varno-1][v2->varattno-1]);

    fc = makeNode(FuncExpr);
          fc->funcid=constants->OID_FUNCTION_PROVENANCE_EQ;
          fc->funcvariadic=false;
          fc->funcresulttype=constants->OID_TYPE_PROVENANCE_TOKEN;
          fc->location=-1;

    c1=makeConst(constants->OID_TYPE_INT,
        -1,
        InvalidOid,
        sizeof(int16),
        first_arg,
        false,
        true);

    c2=makeConst(constants->OID_TYPE_INT,
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
    Node *quals,
    Expr *result,
    int **columns,
    const constants_t *constants)
{
  OpExpr *oe;
  if(quals && IsA(quals, OpExpr)) {
    oe = (OpExpr *) quals;
    result = add_eq_from_OpExpr_to_Expr(oe,result,columns,constants);
  } /* Sometimes OpExpr is nested within a BoolExpr */
  else if (quals) {
    BoolExpr *be = (BoolExpr *) quals;
    /* In some cases, there can be an OR or a NOT specified with ON clause */
    if(be->boolop == OR_EXPR || be->boolop == NOT_EXPR) {
      ereport(ERROR, (errmsg("Boolean operators OR and NOT in a join...on clause are not supported by provsql")));
    } else {
      ListCell *lc2; 
      foreach(lc2,be->args) {
        if(IsA(lfirst(lc2),OpExpr)) {
          oe = (OpExpr *) lfirst(lc2);
          result = add_eq_from_OpExpr_to_Expr(oe,result,columns,constants);
        }
      }
    }
  } /* Handle case of CROSS JOIN with no eqop */
  else { }
  return result;
}

static Expr *make_provenance_expression(
    Query *q,
    List *prov_atts, 
    const constants_t *constants, 
    bool aggregation_needed,
    semiring_operation op,
    int **columns,
    int nbcols)
{
  Expr *result;
  FuncExpr *expr;
  ListCell *lc_v;

  if(op==SR_PLUS) {
    RelabelType *re=(RelabelType *) linitial(prov_atts);
    result=re->arg;
  } else {
    if(lnext(list_head(prov_atts))==NULL) {
      expr=linitial(prov_atts);
    } else {
      expr=makeNode(FuncExpr);
      if(op==SR_TIMES) {
        ArrayExpr *array=makeNode(ArrayExpr);
  
        expr->funcid=constants->OID_FUNCTION_PROVENANCE_TIMES;
        expr->funcvariadic=true;
        
        array->array_typeid=constants->OID_TYPE_UUID_ARRAY;
        array->element_typeid=constants->OID_TYPE_UUID;
        array->elements=prov_atts;
        array->location=-1;
        
        expr->args=list_make1(array);
      } else { // SR_MONUS
        expr->funcid=constants->OID_FUNCTION_PROVENANCE_MONUS;
        expr->args=prov_atts;
      }
      expr->funcresulttype=constants->OID_TYPE_PROVENANCE_TOKEN;
      expr->location=-1;
    }

    if(aggregation_needed) {
      Aggref *agg = makeNode(Aggref);
      TargetEntry *te_inner = makeNode(TargetEntry);

      te_inner->resno=1;
      te_inner->expr=(Expr*)expr;

      agg->aggfnoid=constants->OID_FUNCTION_PROVENANCE_AGG_PLUS;
      agg->aggtype=constants->OID_TYPE_PROVENANCE_TOKEN;
      agg->args=list_make1(te_inner);
      agg->aggkind=AGGKIND_NORMAL;
      agg->location=-1;

#if PG_VERSION_NUM >= 90600
      /* aggargtypes was added in version 9.6 of PostgreSQL */
      agg->aggargtypes=list_make1_oid(constants->OID_TYPE_PROVENANCE_TOKEN);
#endif /* PG_VERSION_NUM >= 90600 */

      result=(Expr*)agg;
    } else {
      result=(Expr*)expr;
    }
  }

//ereport(WARNING,(errmsg("Before: %s",nodeToString(q))));

  /* Part to handle eq gates used for where-provenance. 
   * Placed before projection gates because they need
   * to be deeper in the provenance tree. */
  if(provsql_where_provenance && q->jointree) {
    ListCell *lc;
    foreach(lc, q->jointree->fromlist) {
      if(IsA(lfirst(lc), JoinExpr)) {
        JoinExpr *je = (JoinExpr *) lfirst(lc);
	/* Study equalities coming from From clause */
	result = add_eq_from_Quals_to_Expr(je->quals, result, columns, constants);
      }
    }
    /* Study equalities coming from WHERE clause */
    result = add_eq_from_Quals_to_Expr(q->jointree->quals, result, columns, constants);
  }

  if(provsql_where_provenance) {
    ArrayExpr *array=makeNode(ArrayExpr);
    FuncExpr *fe=makeNode(FuncExpr);
    bool projection=false;
    int nb_column=0;

    fe->funcid=constants->OID_FUNCTION_PROVENANCE_PROJECT;
    fe->funcvariadic=true;
    fe->funcresulttype=constants->OID_TYPE_PROVENANCE_TOKEN;
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
        if(rte_v->rtekind != RTE_JOIN) {
          value_v = columns[vte_v->varno-1][vte_v->varattno-1];
        } else { // is a join
          Var *jav_v = (Var *) lfirst(list_nth_cell(rte_v->joinaliasvars, vte_v->varattno-1));
          value_v = columns[jav_v->varno-1][jav_v->varattno-1];
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
      } else { // we have a function in target
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

    if(nb_column!=nbcols)
      projection=true;

    if(projection) {
      fe->args=list_make2(result, array);
      result=(Expr *)fe;
    } else {
      pfree(array);
      pfree(fe);
    }
  }

  return result;
}

static void add_to_select(
    Query *q, 
    Expr *provenance)
{
  TargetEntry *te=makeNode(TargetEntry);
  te->expr=provenance;
  te->resno=list_length(q->targetList)+1;
  te->resname=(char *)PROVSQL_COLUMN_NAME;
  q->targetList=lappend(q->targetList,te);
}

typedef struct provenance_mutator_context {
  constants_t *constants;
  Expr *provsql;
} provenance_mutator_context;

static Node *provenance_mutator(Node *node, provenance_mutator_context *context)
{
  if(node == NULL)
    return NULL;

  if(IsA(node, FuncExpr)) {
    FuncExpr *f = (FuncExpr *) node;

    if(f->funcid == context->constants->OID_FUNCTION_PROVENANCE) {
      return (Node*) copyObject(context->provsql);
    }
  }

  return expression_tree_mutator(node, provenance_mutator, (void *) context);
}

static void replace_provenance_function_by_expression(Query *q, Expr *provsql, const constants_t *constants)
{
  provenance_mutator_context context = {(constants_t *) constants, provsql};

  query_tree_mutator(q, provenance_mutator, &context, QTW_DONT_COPY_QUERY | QTW_IGNORE_RT_SUBQUERIES);
}

static void transform_distinct_into_group_by(Query *q, const constants_t *constants)
{
  // First check which are already in the group by clause
  // Should be either none or all as "SELECT DISTINCT a, b ... GROUP BY a" 
  // is invalid
  Bitmapset *already_in_group_by=NULL;
  ListCell *lc;
  foreach(lc, q->groupClause) {
    SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
    already_in_group_by = bms_add_member(already_in_group_by, sgc->tleSortGroupRef);
  }

  foreach(lc, q->distinctClause) {
    SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
    if(!bms_is_member(sgc->tleSortGroupRef, already_in_group_by)) {
      q->groupClause = lappend(q->groupClause, sgc);
    }
  }

  q->distinctClause = NULL;
}

static void remove_provenance_attribute_groupref(Query *q, const Bitmapset *removed_sortgrouprefs)
{
  List **lists[3]={&q->groupClause,&q->distinctClause,&q->sortClause};
  int i=0;

  for(i=0;i<3;++i) {
    ListCell *cell, *prev;

    for(cell=list_head(*lists[i]), prev=NULL;
        cell!=NULL
        ;) {
      SortGroupClause *sgc = (SortGroupClause *) lfirst(cell);
      if(bms_is_member(sgc->tleSortGroupRef,removed_sortgrouprefs)) {
        *lists[i] = list_delete_cell(*lists[i], cell, prev);

        if(prev) {
          cell=lnext(prev);
        }
        else {
          cell=list_head(*lists[i]);
        }
      } else {
        prev=cell;
        cell=lnext(cell);
      }
    }
  }
}

static void remove_provenance_attribute_setoperations(Query *q, bool *removed)
{
  SetOperationStmt *so=(SetOperationStmt*) q->setOperations;
  List **lists[3]={&so->colTypes,&so->colTypmods,&so->colCollations};
  int i=0;

  for(i=0;i<3;++i) {
    ListCell *cell, *prev;
    int j;

    for(cell=list_head(*lists[i]), prev=NULL, j=0;
        cell!=NULL;
        ++j) {
      if(removed[j]) {
        *lists[i]=list_delete_cell(*lists[i], cell, prev);

        if(prev) {
          cell=lnext(prev);
        }
        else {
          cell=list_head(*lists[i]);
        }
      } else {
        prev=cell;
        cell=lnext(cell);
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

  SetOperationStmt *stmt = (SetOperationStmt *) q->setOperations;

  ListCell *lc;
  int sortgroupref = 0;

  stmt->all = true ;
  // we might leave sub nodes of the SetOperationsStmt tree with all = false
  // but only for recursive trees of operators and only union can be recursive
  // https://doxygen.postgresql.org/prepunion_8c_source.html#l00479
  // we will set therefore set them later in process_set_operation_union

  rte->rtekind = RTE_SUBQUERY;
  rte->subquery=q;
  rte->eref=copyObject(((RangeTblEntry*)linitial(q->rtable))->eref);
  rte->requiredPerms=ACL_SELECT;
  rte->inFromCl=true;

  rtr->rtindex=1;
  jointree->fromlist=list_make1(rtr);

  new_query->commandType=CMD_SELECT;
  new_query->canSetTag=true;
  new_query->rtable=list_make1(rte);
  new_query->jointree=jointree;
  new_query->targetList=copyObject(q->targetList);

  foreach(lc,new_query->targetList) {
    TargetEntry *te = (TargetEntry *) lfirst(lc);
    SortGroupClause *sgc = makeNode(SortGroupClause);

    sgc->tleSortGroupRef=te->ressortgroupref=++sortgroupref;

    get_sort_group_operators(exprType((Node*) te->expr),false,true,false,&sgc->sortop,&sgc->eqop,NULL,&sgc->hashable);

    new_query->groupClause = lappend(new_query->groupClause, sgc);
  }

  return new_query;
}

static bool provenance_function_walker(
    Node *node, 
    const constants_t *constants) {
  if(node==NULL)
    return false;

  if(IsA(node, FuncExpr)) {
    FuncExpr *f = (FuncExpr *) node;

    if(f->funcid == constants->OID_FUNCTION_PROVENANCE)
      return true;
  }

  return expression_tree_walker(node, provenance_function_walker, (void*) constants);
}

static bool provenance_function_in_group_by(
    Query *q,
    const constants_t *constants) {
  ListCell *lc;
  foreach(lc,q->targetList) {
    TargetEntry *te = (TargetEntry *) lfirst(lc);
    if(te->ressortgroupref > 0 &&
       expression_tree_walker((Node*)te, provenance_function_walker, (void*) constants)) {
      return true;
    }
  }

  return false;
}

static bool has_provenance_walker(
    Node *node,
    const constants_t *constants) {
  if(node==NULL)
    return false;

  if (IsA(node, Query)) {
    Query *q = (Query *) node;
    ListCell *rc;

    if(query_tree_walker(q, has_provenance_walker, (void*) constants, 0))
      return true;

    foreach(rc, q->rtable) {
      RangeTblEntry *r = (RangeTblEntry*) lfirst(rc);
      if(r->rtekind == RTE_RELATION) {
        ListCell *lc;
        AttrNumber attid=1;

        foreach(lc,r->eref->colnames) {
          Value *v = (Value *) lfirst(lc);

          if(!strcmp(strVal(v),PROVSQL_COLUMN_NAME) &&
              get_atttype(r->relid,attid)==constants->OID_TYPE_PROVENANCE_TOKEN) {
            return true;
          }

          ++attid;
        }
      } else if(r->rtekind == RTE_FUNCTION) {
        ListCell *lc;
        AttrNumber attid=1;

        foreach(lc,r->functions) {
          RangeTblFunction *func = (RangeTblFunction *) lfirst(lc);

          if(func->funccolcount==1) {
            FuncExpr *expr = (FuncExpr *) func->funcexpr;
            if(expr->funcresulttype == constants->OID_TYPE_PROVENANCE_TOKEN
                && !strcmp(get_rte_attribute_name(r,attid),PROVSQL_COLUMN_NAME)) {
              return true;
            }
          }

          attid+=func->funccolcount;
        }
      }
    }
  }

  return expression_tree_walker(node, provenance_function_walker, (void*) constants);
}

static bool has_provenance(
    Query *q,
    const constants_t *constants) {
  return has_provenance_walker((Node *) q, constants);
}

static bool transform_except_into_join(
    Query *q,
    const constants_t *constants) {
  SetOperationStmt *setOps = (SetOperationStmt *) q->setOperations;
  RangeTblEntry *rte = makeNode(RangeTblEntry);
  FromExpr *fe = makeNode(FromExpr);
  JoinExpr *je = makeNode(JoinExpr);
  BoolExpr *expr = makeNode(BoolExpr);
  
  // Rewriting of complex set operations has already been done at
  // this point, q->setOps has simple RangeTblRef as children
//  RangeTblEntry *rteLeft = (RangeTblEntry *) list_nth(q->rtable, ((RangeTblRef *) setOps->larg)->rtindex);
//  RangeTblEntry *rteRight = (RangeTblEntry *) list_nth(q->rtable, ((RangeTblRef *) setOps->rarg)->rtindex);

  ListCell *lc;
  int attno=1;

  expr->boolop = AND_EXPR;
  expr->location = -1;
  expr->args=NIL;

  foreach(lc, q->targetList) {
    TargetEntry *te = (TargetEntry *) lfirst(lc);

    Var *v =(Var *) te->expr;

    if(v->vartype != constants->OID_TYPE_PROVENANCE_TOKEN) {
      OpExpr *oe = makeNode(OpExpr);
      Oid opno = find_equality_operator(v->vartype,v->vartype);
      Operator opInfo = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
      Form_pg_operator opform = (Form_pg_operator) GETSTRUCT(opInfo);
      Var *leftArg = makeNode(Var), *rightArg = makeNode(Var);

      oe->opno = opno;
      oe->opfuncid = opform->oprcode;
      oe->opresulttype = opform->oprresult;   
      oe->opcollid = InvalidOid;
      oe->inputcollid = InvalidOid;

      leftArg->varno=leftArg->varnoold=((RangeTblRef *) setOps->larg)->rtindex;
      rightArg->varno=rightArg->varnoold=((RangeTblRef *) setOps->rarg)->rtindex;
      leftArg->varattno=rightArg->varattno=attno;
      leftArg->varoattno=rightArg->varoattno=attno;
      leftArg->vartype=rightArg->vartype=v->vartype;
      leftArg->varcollid=rightArg->varcollid=InvalidOid;
      leftArg->vartypmod=rightArg->vartypmod=-1;
      leftArg->location=rightArg->location=-1;

      oe->args = list_make2(leftArg,rightArg);
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
  je->quals = (Node *) expr;
  je->rtindex=list_length(q->rtable);

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
    SetOperationStmt * stmt,
    const constants_t *constants,
    bool * supported)
{
  if(stmt->op != SETOP_UNION) {
    supported = false ;
    ereport(ERROR, (errmsg("Unsupported mixed set operations")));
  }
  if(IsA(stmt->larg,SetOperationStmt)) {
    process_set_operation_union((SetOperationStmt*)(stmt->larg), constants, supported);
  }
  if(IsA(stmt->rarg,SetOperationStmt)) {
    process_set_operation_union((SetOperationStmt*)(stmt->rarg), constants, supported);
  }
  stmt->colTypes=lappend_oid(stmt->colTypes,
                             constants->OID_TYPE_PROVENANCE_TOKEN);
  stmt->colTypmods=lappend_int(stmt->colTypmods, -1);
  stmt->colCollations=lappend_int(stmt->colCollations, 0);
  stmt->all = true ;
}

static Query *process_query(
    Query *q,
    const constants_t *constants,
    bool subquery)
{
  List *prov_atts;
  bool has_union = false;
  bool has_difference = false;
  bool supported=true;
  int nbcols=0;
  int *columns[q->rtable->length];
  unsigned i=0;

//ereport(NOTICE, (errmsg("Before: %s",nodeToString(q))));

  if(q->setOperations) {
    // TODO: Nest set operations as subqueries in FROM,
    // so that we only do set operations on base tables 

    SetOperationStmt *stmt = (SetOperationStmt *) q->setOperations;
    if(!stmt->all) {
      q = rewrite_non_all_into_external_group_by(q);
      return process_query(q, constants, subquery);
    }
  }

  // get_provenance_attributes will also recursively process subqueries
  // by calling process_query
  prov_atts=get_provenance_attributes(q, constants);

  if(prov_atts==NIL)
    return q;

  if(!subquery) {
    Bitmapset *removed_sortgrouprefs = NULL;
    bool *removed;

    removed_sortgrouprefs=remove_provenance_attributes_select(q, constants, &removed);
    if(removed_sortgrouprefs != NULL)
      remove_provenance_attribute_groupref(q, removed_sortgrouprefs);
    if(q->setOperations)
      remove_provenance_attribute_setoperations(q, removed);
  }

  if(q->hasAggs) {
    ereport(ERROR, (errmsg("Aggregation not supported by provsql")));
    supported=false;
  }

  if(q->hasSubLinks) {
    ereport(ERROR, (errmsg("Subqueries in WHERE clause not supported by provsql")));
    supported=false;
  }

  if(supported && q->distinctClause) {
    if(q->hasDistinctOn) {
      ereport(ERROR, (errmsg("DISTINCT ON not supported by provsql")));
      supported=false;
    } else if(list_length(q->distinctClause) < list_length(q->targetList)) {
      ereport(ERROR, (errmsg("Inconsistent DISTINCT and GROUP BY clauses not supported by provsql")));
      supported=false;
    } else {
      transform_distinct_into_group_by(q, constants);
    }
  }

  if(supported && q->setOperations) {
    SetOperationStmt *stmt = (SetOperationStmt *) q->setOperations;

    if(stmt->op == SETOP_UNION) {
      process_set_operation_union(stmt, constants,&supported);
      has_union = true;
    } else if(stmt->op == SETOP_EXCEPT) {
      if(!transform_except_into_join(q, constants))
        supported=false;
      has_difference=true;
    } else {
      ereport(ERROR, (errmsg("Set operations other than UNION and EXCEPT not supported by provsql")));
      supported=false;
    }
  }

  if(supported &&
     q->groupClause &&
     !provenance_function_in_group_by(q, constants)) {
    q->hasAggs=true;
  }

#if PG_VERSION_NUM >= 90500
  /* GROUPING SETS were introduced in version 9.5 of PostgreSQL */
  if(supported && q->groupingSets) {
    if(q->groupClause || 
       list_length(q->groupingSets)>1 ||
       ((GroupingSet *)linitial(q->groupingSets))->kind != GROUPING_SET_EMPTY) {
      ereport(ERROR, (errmsg("GROUPING SETS, CUBE, and ROLLUP not supported by provsql")));
      supported=false;
    } else {
      // Simple GROUP BY ()
      q->hasAggs=true;
    }
  }
#endif /* PG_VERSION_NUM >= 90500 */
  
  if(supported) {
    ListCell *l;
    
    foreach(l, q->rtable) {
      RangeTblEntry *r = (RangeTblEntry *) lfirst(l);
      ListCell *lc;

      columns[i]=0;
      if(r->eref) {
        unsigned j=0;

        columns[i]=(int *) palloc(r->eref->colnames->length*sizeof(int));

        foreach(lc, r->eref->colnames) {
          Value *v = (Value *) lfirst(lc);

          if(strcmp(strVal(v),"") && r->rtekind != RTE_JOIN) { // TODO: More robust test
                                                                              // join RTE columns ignored
            if(!strcmp(strVal(v),PROVSQL_COLUMN_NAME))
              columns[i][j]=-1;              
            else
              columns[i][j]=++nbcols;
          } else {
            columns[i][j]=0;
          }
          ++j;
        }
      }
         
      ++i;
    }
  }

  if(supported) {
    Expr *provenance = make_provenance_expression(
        q,
        prov_atts,
        constants,
        q->hasAggs,
        has_union ? SR_PLUS : (has_difference ? SR_MONUS : SR_TIMES),
        columns,
        nbcols);

    add_to_select(q,provenance);
    replace_provenance_function_by_expression(q, provenance, constants);
  }

  for(i=0;i<q->rtable->length;++i) {
    if(columns[i])
      pfree(columns[i]);
  }

//ereport(NOTICE, (errmsg("After: %s",nodeToString(q))));

  return q;
}

static PlannedStmt *provsql_planner(
    Query *q,
    int cursorOptions,
    ParamListInfo boundParams)
{
  if(q->commandType==CMD_SELECT && q->rtable) {
    constants_t constants;
    if(initialize_constants(&constants)) {
      if(has_provenance(q,&constants)) {
        Query *new_query = process_query(q, &constants, false);
        if(new_query != NULL)
          q = new_query;
      }
    }
  }

  if(prev_planner)
    return prev_planner(q, cursorOptions, boundParams);
  else
    return standard_planner(q, cursorOptions, boundParams);
}

void _PG_init(void)
{
  DefineCustomBoolVariable("provsql.where_provenance",
                          "Should ProvSQL track where-provenance?",
                          "1 turns where-provenance on, 0 off.",
                          &provsql_where_provenance,
                          true,
                          PGC_USERSET,
                          0,
                          NULL,
                          NULL,
                          NULL); 

  prev_planner = planner_hook;

  if(process_shared_preload_libraries_in_progress) {
    planner_hook = provsql_planner;

    provsql_shared_library_loaded=true;
  }
}

void _PG_fini(void)
{
  planner_hook = prev_planner;
}
