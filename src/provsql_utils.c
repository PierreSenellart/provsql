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

bool initialize_constants(constants_t *constants)
{
#define CheckOid(o) if(constants->o==InvalidOid) return false

  constants->OID_SCHEMA_PROVSQL = get_namespace_oid("provsql", true);
  CheckOid(OID_SCHEMA_PROVSQL);

  constants->OID_TYPE_PROVENANCE_TOKEN = GetSysCacheOid2(TYPENAMENSP,CStringGetDatum("provenance_token"),ObjectIdGetDatum(constants->OID_SCHEMA_PROVSQL));
  CheckOid(OID_TYPE_PROVENANCE_TOKEN);

  constants->OID_TYPE_UUID = TypenameGetTypid("uuid");
  CheckOid(OID_TYPE_UUID);

  constants->OID_TYPE_UUID_ARRAY = TypenameGetTypid("_uuid");
  CheckOid(OID_TYPE_UUID_ARRAY);

  constants->OID_FUNCTION_PROVENANCE_AGG_PLUS = GetFuncOid("provenance_agg");
  CheckOid(OID_FUNCTION_PROVENANCE_AGG_PLUS);

  constants->OID_FUNCTION_PROVENANCE_TIMES = GetFuncOid("provenance_times");
  CheckOid(OID_FUNCTION_PROVENANCE_TIMES);

  constants->OID_FUNCTION_PROVENANCE_MONUS = GetFuncOid("provenance_monus");
  CheckOid(OID_FUNCTION_PROVENANCE_MONUS);

  constants->OID_FUNCTION_PROVENANCE = GetFuncOid("provenance");
  CheckOid(OID_FUNCTION_PROVENANCE);

  return true;
}
