#include "postgres.h"
#include "fmgr.h"
#include "access/sysattr.h"
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "optimizer/planner.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC;

static const char *PROVSQL_COLUMN_NAME="provsql";

extern void _PG_init(void);
extern void _PG_fini(void);

static planner_hook_type prev_planner = NULL;

typedef struct {
  Oid PROVENANCE_TOKEN_OID;
  Oid UUID_OID;
  Oid UUID_ARRAY_OID;
  Oid PROVENANCE_AND_OID;
  Oid PROVENANCE_AGG_OID;
} constants_t;

static List *getProvenanceAttributes(Query *q, const constants_t *constants) {
  List *prov_atts=NIL;
  ListCell *l;
  Index relid=1;

  foreach(l, q->rtable) {
    RangeTblEntry *r = (RangeTblEntry *) lfirst(l);
    if(r->rtekind == RTE_RELATION) {
      ListCell *lc;
      AttrNumber attid=1;
      foreach(lc,r->eref->colnames) {
        Value *v = (Value *) lfirst(lc);
        
        if(!strcmp(strVal(v),PROVSQL_COLUMN_NAME) &&
            get_atttype(r->relid,attid)==constants->PROVENANCE_TOKEN_OID) {
          RelabelType *re=makeNode(RelabelType);
          Var *v=makeNode(Var);
          v->varno=v->varnoold=relid;
          v->varattno=v->varoattno=attid;
          v->vartype=constants->PROVENANCE_TOKEN_OID;
          v->varcollid=InvalidOid;
          v->vartypmod=-1;
          v->location=-1;

          re->arg=(Expr*)v;
          re->resulttype=constants->UUID_OID;
          re->resulttypmod=-1;
          re->resultcollid=InvalidOid;
          re->relabelformat=COERCION_EXPLICIT;
          re->location=-1;

          prov_atts=lappend(prov_atts,re);
          r->selectedCols=bms_add_member(r->selectedCols,attid-FirstLowInvalidHeapAttributeNumber);
        }

        ++attid;
      }
    } else {
      ereport(WARNING, (errmsg("FROM clause unsupported by provsql")));
    }

    ++relid;
  }
  
  return prov_atts;
}

static void removeProvenanceAttributesSelect(Query *q, const constants_t *constants)
{
  int nbRemoved=0;

  for(ListCell *cell=list_head(q->targetList), *prev=NULL;cell!=NULL;) {
    TargetEntry *rt = (TargetEntry *) lfirst(cell);
    bool removed=false;

    if(rt->expr->type==T_Var) {
      Var *v =(Var *) rt->expr;

      if(!strcmp(rt->resname,PROVSQL_COLUMN_NAME) &&
          v->vartype==constants->PROVENANCE_TOKEN_OID) {
        q->targetList=list_delete_cell(q->targetList, cell, prev);

        removed=true;
        ++nbRemoved;
      }
    }

    if(removed) {
      if(prev)
        cell=lnext(prev);
      else
        cell=list_head(q->targetList);
    } else {
      rt->resno-=nbRemoved;
      prev=cell;
      cell=lnext(cell);
    }
  }
}

static void addProvenanceAndSelect(Query *q, List *prov_atts, const constants_t *constants, bool aggregation_needed)
{
  ArrayExpr *array=makeNode(ArrayExpr);
  FuncExpr *expr=makeNode(FuncExpr);
  TargetEntry *te=makeNode(TargetEntry);

  array->array_typeid=constants->UUID_ARRAY_OID;
  array->element_typeid=constants->UUID_OID;
  array->elements=prov_atts;
  array->location=-1;

  expr->funcid=constants->PROVENANCE_AND_OID;
  expr->funcresulttype=constants->PROVENANCE_TOKEN_OID;
  expr->funcvariadic=true;
  expr->args=list_make1(array);
  expr->location=-1;

  te->resno=list_length(q->targetList)+1;
  te->resname=(char *)PROVSQL_COLUMN_NAME;

  if(aggregation_needed) {
    Aggref *agg = makeNode(Aggref);
    TargetEntry *te_inner = makeNode(TargetEntry);

    te_inner->resno=1;
    te_inner->expr=(Expr*)expr;

    agg->aggfnoid=constants->PROVENANCE_AGG_OID;
    agg->aggtype=constants->PROVENANCE_TOKEN_OID;
    agg->args=list_make1(te_inner);
    agg->aggkind=AGGKIND_NORMAL;
    agg->location=-1;

    te->expr=(Expr*)agg;
  } else {
    te->expr=(Expr*)expr;
  }
    
  q->targetList=lappend(q->targetList,te);
}

static void initialize_constants(constants_t *constants)
{
  FuncCandidateList fcl;

  constants->PROVENANCE_TOKEN_OID = TypenameGetTypid("provenance_token");
  constants->UUID_OID = TypenameGetTypid("uuid");
  constants->UUID_ARRAY_OID = TypenameGetTypid("_uuid");

  fcl=FuncnameGetCandidates(list_make1(makeString("provenance_and")),-1,NIL,false,false,false);
  if(fcl)
    constants->PROVENANCE_AND_OID=fcl->oid;    
  else
    constants->PROVENANCE_AND_OID=0;
  
  fcl=FuncnameGetCandidates(list_make1(makeString("provenance_agg")),-1,NIL,false,false,false);
  if(fcl)
    constants->PROVENANCE_AGG_OID=fcl->oid;    
  else
    constants->PROVENANCE_AGG_OID=0;
}

static PlannedStmt *provsql_planner(
    Query *q,
    int cursorOptions,
    ParamListInfo boundParams)
{
  if(q->commandType==CMD_SELECT) {
    List *prov_atts;
    constants_t constants;

    initialize_constants(&constants);

    prov_atts=getProvenanceAttributes(q, &constants);

    if(prov_atts!=NIL) {
      if(q->hasAggs) {
        ereport(ERROR, (errmsg("Aggregation not supported on tables with provenance")));
      }

      removeProvenanceAttributesSelect(q, &constants);

      if(q->groupClause) {
        q->hasAggs=true;
      }

      addProvenanceAndSelect(q, prov_atts, &constants, q->groupClause != NIL);
      ereport(WARNING, (errmsg("%s",nodeToString(q))));
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
