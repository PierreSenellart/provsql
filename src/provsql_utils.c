#include "postgres.h"
#include "catalog/namespace.h"
#include "nodes/value.h"

#include "provsql_utils.h"

static Oid GetFuncOid(char *s)
{
  FuncCandidateList fcl=FuncnameGetCandidates(
      list_make1(makeString(s)),-1,NIL,false,false,false);
  if(fcl)
    return fcl->oid;    
  else
    return 0;
}

void initialize_constants(constants_t *constants)
{
  constants->PROVENANCE_TOKEN_OID = TypenameGetTypid("provenance_token");
  constants->UUID_OID = TypenameGetTypid("uuid");
  constants->UUID_ARRAY_OID = TypenameGetTypid("_uuid");
  constants->PROVENANCE_AND_OID = GetFuncOid("provenance_and");
  constants->PROVENANCE_AGG_OID = GetFuncOid("provenance_agg");
  constants->PROVENANCE_OID = GetFuncOid("provenance");
}
