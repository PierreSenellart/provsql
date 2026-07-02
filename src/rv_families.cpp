/**
 * @file rv_families.cpp
 * @brief SQL surface for the continuous-distribution family registry
 *        (@ref src/distributions/Distribution.h).
 *
 * Exposes @c rv_families(), the set-returning catalog of every registered
 * @c gate_rv family: the on-disk name token, parameter count and
 * conventional parameter symbols, and a short display label.  UI clients
 * (ProvSQL Studio's circuit inspector) read it to render families they
 * were not hard-coded for, so a family added under
 * @c src/distributions/ shows up without a client release.
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "compatibility.h"   /* TYPALIGN_INT fallback for PG < 11 */

PG_FUNCTION_INFO_V1(rv_families);
}

#include "distributions/Distribution.h"
#include "provsql_error.h"

#include <string>
#include <vector>

namespace {

/// Build a text[] Datum from the family's parameter-name literals.
Datum param_names_to_text_array(const provsql::DistributionFamily &fam)
{
  std::vector<Datum> elems;
  for (unsigned i = 0; i < fam.nparams && i < 2; ++i) {
    const char *name = fam.param_names[i];
    if (!name)
      break;
    elems.push_back(PointerGetDatum(cstring_to_text(name)));
  }
  if (elems.empty())
    return PointerGetDatum(construct_empty_array(TEXTOID));
  ArrayType *arr = construct_array(elems.data(),
                                   static_cast<int>(elems.size()),
                                   TEXTOID, -1, false, TYPALIGN_INT);
  return PointerGetDatum(arr);
}

}  // namespace

extern "C" Datum
rv_families(PG_FUNCTION_ARGS)
{
  ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

  MemoryContext per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
  MemoryContext oldcontext = MemoryContextSwitchTo(per_query_ctx);

  TupleDesc tupdesc;
  if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("rv_families: function must return a row type");
  }
  tupdesc = BlessTupleDesc(tupdesc);

  Tuplestorestate *tupstore = tuplestore_begin_heap(
    rsinfo->allowedModes & SFRM_Materialize_Random, false, work_mem);
  rsinfo->returnMode = SFRM_Materialize;
  rsinfo->setResult = tupstore;
  rsinfo->setDesc = tupdesc;

  try {
    for (const provsql::DistributionFamily *fam :
         provsql::listDistributionFamilies()) {
      Datum values[4];
      bool nulls[4] = {false, false, false, false};

      values[0] = PointerGetDatum(cstring_to_text(fam->name));
      values[1] = Int32GetDatum(static_cast<int32>(fam->nparams));
      values[2] = param_names_to_text_array(*fam);
      values[3] = PointerGetDatum(cstring_to_text(fam->label ? fam->label
                                                             : ""));

      tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
  } catch (const std::exception &e) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("rv_families: %s", e.what());
  } catch (...) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("rv_families: unknown exception");
  }

  MemoryContextSwitchTo(oldcontext);
  PG_RETURN_NULL();
}
