#include "postgres.h"
#include "access/sysattr.h"
#include "catalog/pg_aggregate.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/planner.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

#include "provsql_utils.h"

PG_MODULE_MAGIC;

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
  re->relabelformat=COERCION_EXPLICIT;
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
        r->eref->colnames = lappend(r->eref->colnames, makeString("provsql"));
        prov_atts=lappend(prov_atts,make_provenance_attribute(r,rteid,r->eref->colnames->length,constants));
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
       ereport(WARNING, (errmsg("JOIN type not supported by provsql")));
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
          ereport(WARNING, (errmsg("FROM function with multiple output attributes are not supported by provsql")));
        }

        attid+=func->funccolcount;
      }
    } else {
      ereport(WARNING, (errmsg("FROM clause unsupported by provsql")));
    }

    ++rteid;
  }

  return prov_atts;
}

static Bitmapset *remove_provenance_attributes_select(
    Query *q,
    const constants_t *constants)
{
  int nbRemoved=0;
  int i=0;
  Bitmapset *ressortgrouprefs = NULL;

  for(ListCell *cell=list_head(q->targetList), *prev=NULL;
      cell!=NULL
      ;) {
    TargetEntry *rt = (TargetEntry *) lfirst(cell);
    bool removed=false;

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

          removed=true;
          ++nbRemoved;

          if(rt->ressortgroupref > 0)
            ressortgrouprefs = bms_add_member(ressortgrouprefs, rt->ressortgroupref);
        }
      }

      ++i;
    }

    if(removed) {
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
  }

  return ressortgrouprefs;
}

static Expr *add_provenance_to_select(
    Query *q, 
    List *prov_atts, 
    const constants_t *constants, 
    bool aggregation_needed,
    bool union_required)
{
  ArrayExpr *array;
  FuncExpr *expr;
  TargetEntry *te=makeNode(TargetEntry);

  te->resno=list_length(q->targetList)+1;
  te->resname=(char *)PROVSQL_COLUMN_NAME;

  if(union_required) {
    RelabelType *re=(RelabelType *) linitial(prov_atts);
    te->expr=re->arg;
  } else {
    array=makeNode(ArrayExpr);
    array->array_typeid=constants->OID_TYPE_UUID_ARRAY;
    array->element_typeid=constants->OID_TYPE_UUID;
    array->elements=prov_atts;
    array->location=-1;

    expr=makeNode(FuncExpr);
    expr->funcid=constants->OID_FUNCTION_PROVENANCE_TIMES;
    expr->funcresulttype=constants->OID_TYPE_PROVENANCE_TOKEN;
    expr->funcvariadic=true;
    expr->args=list_make1(array);
    expr->location=-1;

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

      te->expr=(Expr*)agg;
    } else {
      te->expr=(Expr*)expr;
    }
  }
    
  q->targetList=lappend(q->targetList,te);

  return te->expr;
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
      return copyObject(context->provsql);
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
      
static void remove_provenance_attribute_groupref(Query *q, const constants_t *constants, const Bitmapset *removed_sortgrouprefs)
{
  List **lists[3]={&q->groupClause,&q->distinctClause,&q->sortClause};

  for(int i=0;i<3;++i) {
    for(ListCell *cell=list_head(*lists[i]), *prev=NULL;
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
      
static Query *rewrite_all_into_external_group_by(Query *q)
{
  Query *new_query = makeNode(Query);
  RangeTblEntry *rte = makeNode(RangeTblEntry);
  FromExpr *jointree = makeNode(FromExpr);
  RangeTblRef *rtr = makeNode(RangeTblRef);
  
  SetOperationStmt *stmt = (SetOperationStmt *) q->setOperations;

  ListCell *lc;
  int sortgroupref = 0;

  stmt->all=true;

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

static Query *process_query(
    Query *q,
    const constants_t *constants,
    bool subquery)
{
  List *prov_atts;
  Expr *provsql;
  bool has_union = false;
  bool supported=true;

//  ereport(NOTICE, (errmsg("Before: %s",nodeToString(q))));

  if(q->setOperations) {
    SetOperationStmt *stmt = (SetOperationStmt *) q->setOperations;
    if(!stmt->all) {
      q = rewrite_all_into_external_group_by(q);
      return process_query(q, constants, subquery);
    }
  }
    
  prov_atts=get_provenance_attributes(q, constants);
  
  if(prov_atts==NIL)
    return q;
  
  if(q->hasAggs) {
    ereport(ERROR, (errmsg("Aggregation not supported on tables with provenance")));
    supported=false;
  }

  if(!subquery) {
    Bitmapset *removed_sortgrouprefs = NULL;
    removed_sortgrouprefs=remove_provenance_attributes_select(q, constants);
    if(removed_sortgrouprefs != NULL)
      remove_provenance_attribute_groupref(q, constants, removed_sortgrouprefs);
  }

  if(supported && q->distinctClause) {
    if(list_length(q->distinctClause) < list_length(q->targetList)) {
      ereport(WARNING, (errmsg("DISTINCT ON not supported by provsql")));
      supported=false;
    } else 
      transform_distinct_into_group_by(q, constants);
  }

  if(supported &&
     q->groupClause &&
     !provenance_function_in_group_by(q, constants)) {
    q->hasAggs=true;
  }

  if(supported && q->setOperations) {
    SetOperationStmt *stmt = (SetOperationStmt *) q->setOperations;

    if(stmt->op == SETOP_UNION) {
      stmt->colTypes=lappend_oid(stmt->colTypes,
          constants->OID_TYPE_PROVENANCE_TOKEN);
      stmt->colTypmods=lappend_int(stmt->colTypmods, -1);
      stmt->colCollations=lappend_int(stmt->colCollations, 0);

      has_union = true;
    } else {
      ereport(WARNING, (errmsg("Set operations other than UNION not supported by provsql")));
      supported=false;
    }
  }
  
  if(supported) {
    provsql = add_provenance_to_select(
        q,
        prov_atts,
        constants,
        q->hasAggs,
        has_union);
  
    replace_provenance_function_by_expression(q, provsql, constants);
  }

//  ereport(NOTICE, (errmsg("After: %s",nodeToString(q))));

  return q;
}

static PlannedStmt *provsql_planner(
    Query *q,
    int cursorOptions,
    ParamListInfo boundParams)
{
  if(q->commandType==CMD_SELECT) {
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
  prev_planner = planner_hook;
  planner_hook = provsql_planner;
}

void _PG_fini(void)
{
  planner_hook = prev_planner;
}
