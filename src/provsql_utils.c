#include "postgres.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "nodes/value.h"
#include "parser/parse_func.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"

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

static Oid GetProvSQLFuncOid(char *s)
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
  
  constants->OID_TYPE_INT = TypenameGetTypid("int4");
  CheckOid(OID_TYPE_INT);

  constants->OID_TYPE_INT_ARRAY = TypenameGetTypid("_int4");
  CheckOid(OID_TYPE_INT_ARRAY);
  
  constants->OID_FUNCTION_ARRAY_AGG = GetFuncOid("array_agg");
  CheckOid(OID_FUNCTION_ARRAY_AGG);

  constants->OID_FUNCTION_PROVENANCE_PLUS = GetProvSQLFuncOid("provenance_plus");
  CheckOid(OID_FUNCTION_PROVENANCE_PLUS);

  constants->OID_FUNCTION_PROVENANCE_TIMES = GetProvSQLFuncOid("provenance_times");
  CheckOid(OID_FUNCTION_PROVENANCE_TIMES);

  constants->OID_FUNCTION_PROVENANCE_MONUS = GetProvSQLFuncOid("provenance_monus");
  CheckOid(OID_FUNCTION_PROVENANCE_MONUS);
  
  constants->OID_FUNCTION_PROVENANCE_PROJECT = GetProvSQLFuncOid("provenance_project");
  CheckOid(OID_FUNCTION_PROVENANCE_PROJECT);

  constants->OID_FUNCTION_PROVENANCE_EQ = GetProvSQLFuncOid("provenance_eq");
  CheckOid(OID_FUNCTION_PROVENANCE_EQ);

  constants->OID_FUNCTION_PROVENANCE = GetProvSQLFuncOid("provenance");
  CheckOid(OID_FUNCTION_PROVENANCE);

  constants->OID_FUNCTION_PROVENANCE_DELTA = GetFuncOid("provenance_delta");
  CheckOid(OID_FUNCTION_PROVENANCE_DELTA);

  constants->OID_FUNCTION_PROVENANCE_AGGREGATE = GetFuncOid("provenance_aggregate");
  CheckOid(OID_FUNCTION_PROVENANCE_AGGREGATE);

  constants->OID_FUNCTION_PROVENANCE_SEMIMOD = GetFuncOid("provenance_semimod");
  CheckOid(OID_FUNCTION_PROVENANCE_SEMIMOD);

  return true;
}

/* Copied over from parse_oper.c as defined static there */
static Oid
binary_oper_exact(List *opname, Oid arg1, Oid arg2)
{
	Oid			result;
	bool		was_unknown = false;

	/* Unspecified type for one of the arguments? then use the other */
	if ((arg1 == UNKNOWNOID) && (arg2 != InvalidOid))
	{
		arg1 = arg2;
		was_unknown = true;
	}
	else if ((arg2 == UNKNOWNOID) && (arg1 != InvalidOid))
	{
		arg2 = arg1;
		was_unknown = true;
	}

	result = OpernameGetOprid(opname, arg1, arg2);
	if (OidIsValid(result))
		return result;

	if (was_unknown)
	{
		/* arg1 and arg2 are the same here, need only look at arg1 */
		Oid			basetype = getBaseType(arg1);

		if (basetype != arg1)
		{
			result = OpernameGetOprid(opname, basetype, basetype);
			if (OidIsValid(result))
				return result;
		}
	}

	return InvalidOid;
}

/* Similar mechanism as in parse_oper.ci, in particular
 * in the static function oper_select_candidate */
Oid find_equality_operator(Oid ltypeId, Oid rtypeId)
{
  List * const equals=list_make1(makeString("="));

  FuncCandidateList clist;
  Oid inputOids[2] = {ltypeId,rtypeId};
  int ncandidates;
  
  Oid result = binary_oper_exact(equals, ltypeId, rtypeId);

  if(result!=InvalidOid)
    return result;

  clist = OpernameGetCandidates(equals, 'b', false);

  ncandidates = func_match_argtypes(2, inputOids,
      clist, &clist);

  if (ncandidates == 0)
    return InvalidOid;
  else if (ncandidates == 1)
    return clist->oid;
  
  clist = func_select_candidate(2, inputOids, clist);

  if(clist)
    return clist->oid;
  else
    return InvalidOid;
}
