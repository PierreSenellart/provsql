#include "postgres.h"
#include "catalog/namespace.h"
#include "nodes/value.h"
#include "utils/syscache.h"

#include "provsql_utils.h"

static Oid GetFuncOid(char *s)
{
  FuncCandidateList fcl=FuncnameGetCandidates(
      list_make2(makeString("provsql"),makeString(s)),-1,NIL,false,false,false);
  if(fcl)
    return fcl->oid;    
  else
    return 0;
}

void initialize_constants(constants_t *constants)
{
  constants->OID_SCHEMA_PROVSQL = get_namespace_oid("provsql", true);
  if(constants->OID_SCHEMA_PROVSQL==InvalidOid)
    return false;

  constants->OID_TYPE_PROVENANCE_TOKEN = GetSysCacheOid2(TYPENAMENSP,CStringGetDatum("provenance_token"),ObjectIdGetDatum(constants->OID_SCHEMA_PROVSQL));
  constants->OID_TYPE_UUID = TypenameGetTypid("uuid");
  constants->OID_TYPE_UUID_ARRAY = TypenameGetTypid("_uuid");
  constants->OID_FUNCTION_PROVENANCE_AND = GetFuncOid("provenance_and");
  constants->OID_FUNCTION_PROVENANCE_AGG = GetFuncOid("provenance_agg");
  constants->OID_FUNCTION_PROVENANCE = GetFuncOid("provenance");
}
