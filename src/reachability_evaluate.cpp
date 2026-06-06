/**
 * @file reachability_evaluate.cpp
 * @brief SQL entry points for decomposition-aligned reachability
 *        compilation over bounded-treewidth data.
 *
 * Exposes @c ReachabilityCompiler (see @c ReachabilityCompiler.h) to SQL:
 *
 * - @c reachability_evaluate(): exact probability that the target vertex
 *   is reachable from the source vertex (two-terminal network
 *   reliability), in time linear in the number of edges for data of
 *   bounded treewidth;
 * - @c reachability_compile_stats(): same compilation, returning the
 *   probability together with structural statistics (data treewidth,
 *   number of bags, maximum DP state count, d-DNNF size) that
 *   substantiate the linear-size claim in tests and benchmarks.
 *
 * Both take the edge relation in columnar form (parallel arrays of
 * source vertices, target vertices, provenance tokens, and
 * probabilities); the user-facing wrappers in @c provsql.sql gather
 * those arrays from an arbitrary provenance-tracked edge relation.
 * Vertices are dense integer IDs (the wrappers map arbitrary vertex
 * values onto them).
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/uuid.h"

#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(reachability_evaluate);
PG_FUNCTION_INFO_V1(reachability_compile_stats);
}

#include "c_cpp_compatibility.h"
#include "ReachabilityCompiler.h"
#include "provsql_utils_cpp.h"

#include <string>
#include <vector>

namespace {

/**
 * @brief Validate a 1-D, NULL-free array argument and return its length.
 * @param arr   The array (may be null for "no edges").
 * @param what  Argument name for error messages.
 * @return      Number of elements (0 when @p arr is null).
 */
int checkedArrayLength(ArrayType *arr, const char *what)
{
  if (arr == NULL)
    return 0;
  if (ARR_NDIM(arr) > 1)
    provsql_error("reachability: %s must be a one-dimensional array", what);
  if (ARR_HASNULL(arr))
    provsql_error("reachability: %s must not contain NULLs", what);
  return ARR_NDIM(arr) == 0 ? 0 : ARR_DIMS(arr)[0];
}

/**
 * @brief Decode the columnar edge arguments shared by both entry points
 *        and run the compilation.
 *
 * Argument layout (both functions): @c srcs @c int[], @c dsts @c int[],
 * @c tokens @c uuid[], @c probs @c float8[], @c source @c int,
 * @c target @c int, @c directed @c boolean.
 *
 * @param fcinfo  PostgreSQL function-call info.
 * @return        The compiled d-DNNF and statistics.
 */
ReachabilityCompiler::Result compileFromArgs(FunctionCallInfo fcinfo)
{
  for (int i = 4; i < 7; ++i)
    if (PG_ARGISNULL(i))
      provsql_error("reachability: source, target and directed must not be NULL");

  ArrayType *srcs   = PG_ARGISNULL(0) ? NULL : PG_GETARG_ARRAYTYPE_P(0);
  ArrayType *dsts   = PG_ARGISNULL(1) ? NULL : PG_GETARG_ARRAYTYPE_P(1);
  ArrayType *tokens = PG_ARGISNULL(2) ? NULL : PG_GETARG_ARRAYTYPE_P(2);
  ArrayType *probs  = PG_ARGISNULL(3) ? NULL : PG_GETARG_ARRAYTYPE_P(3);

  const int n = checkedArrayLength(srcs, "sources");
  if (checkedArrayLength(dsts, "destinations") != n ||
      checkedArrayLength(tokens, "tokens") != n ||
      checkedArrayLength(probs, "probabilities") != n)
    provsql_error("reachability: edge arrays must have the same length");

  std::vector<ReachabilityCompiler::EdgeRow> rows;
  rows.reserve(n);

  if (n > 0) {
    /* All four element types are fixed-length and NULL-free (checked
     * above), so the data areas are packed and can be read directly,
     * as create_gate does for uuid[]. */
    const int32 *src_data = (const int32 *) ARR_DATA_PTR(srcs);
    const int32 *dst_data = (const int32 *) ARR_DATA_PTR(dsts);
    const pg_uuid_t *token_data = (const pg_uuid_t *) ARR_DATA_PTR(tokens);
    const float8 *prob_data = (const float8 *) ARR_DATA_PTR(probs);

    for (int i = 0; i < n; ++i) {
      ReachabilityCompiler::EdgeRow row;
      row.src = static_cast<unsigned long>(src_data[i]);
      row.dst = static_cast<unsigned long>(dst_data[i]);
      row.token = uuid2string(token_data[i]);
      row.prob = prob_data[i];
      if (row.prob < 0. || row.prob > 1.)
        provsql_error("reachability: edge probability %f out of [0,1]",
                      row.prob);
      rows.push_back(std::move(row));
    }
  }

  const unsigned long source = static_cast<unsigned long>(PG_GETARG_INT32(4));
  const unsigned long target = static_cast<unsigned long>(PG_GETARG_INT32(5));
  const bool directed = PG_GETARG_BOOL(6);

  try {
    return ReachabilityCompiler::compile(rows, source, target, directed);
  } catch (TreeDecompositionException &) {
    provsql_error(
      "reachability: data treewidth exceeds the supported limit (%d)",
      TreeDecomposition::MAX_TREEWIDTH);
    throw; /* unreachable; placate the compiler */
  }
}

} // namespace

/**
 * @brief PostgreSQL-callable entry point: exact reachability probability.
 *
 * Arguments: see @c compileFromArgs().
 * Returns: the probability that @c target is reachable from @c source.
 */
Datum reachability_evaluate(PG_FUNCTION_ARGS)
{
  try {
    auto result = compileFromArgs(fcinfo);
    PG_RETURN_FLOAT8(result.dd.probabilityEvaluation());
  } catch (const std::exception &e) {
    provsql_error("reachability: %s", e.what());
  } catch (...) {
    provsql_error("reachability: unknown exception");
  }
  PG_RETURN_NULL();
}

/**
 * @brief PostgreSQL-callable entry point: probability plus compilation
 *        statistics.
 *
 * Arguments: see @c compileFromArgs().
 * Returns: composite @c (probability, data_treewidth, nb_bags,
 * max_states, nb_gates, nb_variables).
 */
Datum reachability_compile_stats(PG_FUNCTION_ARGS)
{
  try {
    auto result = compileFromArgs(fcinfo);

    TupleDesc tupdesc;
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
      provsql_error("reachability_compile_stats: expected composite return type");
    tupdesc = BlessTupleDesc(tupdesc);

    Datum values[6];
    bool nulls[6] = {false, false, false, false, false, false};
    values[0] = Float8GetDatum(result.dd.probabilityEvaluation());
    values[1] = Int32GetDatum(static_cast<int32>(result.stats.data_treewidth));
    values[2] = Int64GetDatum(static_cast<int64>(result.stats.nb_bags));
    values[3] = Int64GetDatum(static_cast<int64>(result.stats.max_states));
    values[4] = Int64GetDatum(static_cast<int64>(result.stats.nb_gates));
    values[5] = Int64GetDatum(static_cast<int64>(result.stats.nb_variables));

    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
  } catch (const std::exception &e) {
    provsql_error("reachability_compile_stats: %s", e.what());
  } catch (...) {
    provsql_error("reachability_compile_stats: unknown exception");
  }
  PG_RETURN_NULL();
}
