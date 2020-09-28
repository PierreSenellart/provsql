#include "postgres.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "nodes/value.h"
#include "parser/parse_func.h"
#include "utils/lsyscache.h"

#include "provsql_utils.h"

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
