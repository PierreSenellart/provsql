/**
 * @file ucq_joint_evaluate.cpp
 * @brief SQL entry points for the joint-width UCQ compiler.
 *
 * Exposes @c UCQJointCompiler (see @c UCQJointCompiler.h) to SQL:
 *
 * - @c ucq_joint_evaluate(): exact probability of a Boolean UCQ -- even
 *   one that is @c \#P-hard under the Dalvi-Suciu dichotomy -- evaluated
 *   in time tractable whenever the joint treewidth of the data and its
 *   correlation structure is bounded;
 * - @c ucq_joint_compile_stats(): the same compilation, returning the
 *   probability together with the three width columns (joint width,
 *   data-only and circuit-only degeneracy lower bounds) that
 *   substantiate thesis Prop. 4.2.11 empirically -- the adversarial
 *   family has small data/circuit widths but large joint width -- and
 *   the structural statistics (number of bags, peak DP state count, d-D
 *   size).
 *
 * Both take the query and the facts in flat columnar form (the UCQ
 * structure and the relation rows as parallel arrays); the user-facing
 * @c ucq_joint_evaluate(query jsonb, ...) plpgsql wrapper in
 * @c provsql.common.sql flattens a JSON UCQ specification and gathers
 * the rows.  Element ids are dense integers assigned by the wrapper with
 * a dictionary shared across relations (so join-compatible values match).
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/uuid.h"

#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(ucq_joint_evaluate);
PG_FUNCTION_INFO_V1(ucq_joint_compile_stats);
PG_FUNCTION_INFO_V1(ucq_joint_evaluate_tracked);
PG_FUNCTION_INFO_V1(ucq_joint_compile_stats_tracked);
PG_FUNCTION_INFO_V1(ucq_joint_materialize_tracked);
}

#include "c_cpp_compatibility.h"
#include "JointEncoding.h"
#include "UCQJointCompiler.h"
#include "CircuitFromMMap.h"
#include "CertifiedDDMaterialize.h"
#include "GenericCircuit.h"
#include "provsql_utils_cpp.h"

#include <map>
#include <string>
#include <vector>

namespace {

/** @brief Validate a 1-D, NULL-free array argument and return its length. */
int checkedArrayLength(ArrayType *arr, const char *what)
{
  if (arr == NULL)
    return 0;
  if (ARR_NDIM(arr) > 1)
    provsql_error("ucq_joint: %s must be a one-dimensional array", what);
  if (ARR_HASNULL(arr))
    provsql_error("ucq_joint: %s must not contain NULLs", what);
  return ARR_NDIM(arr) == 0 ? 0 : ARR_DIMS(arr)[0];
}

/** @brief Fetch an int[] argument's data pointer (NULL when absent). */
const int32 *intArray(FunctionCallInfo fcinfo, int argno, const char *what,
                      int &len)
{
  ArrayType *a = PG_ARGISNULL(argno) ? NULL : PG_GETARG_ARRAYTYPE_P(argno);
  len = checkedArrayLength(a, what);
  return (a == NULL || len == 0) ? NULL : (const int32 *) ARR_DATA_PTR(a);
}

/**
 * @brief Decode the ten columnar arguments into a @c UCQ and the facts.
 *
 * Argument layout (all NULL-free 1-D arrays):
 *   0 disjunct_nvars int[]   per disjunct: number of query variables
 *   1 atom_disjunct  int[]   per atom: owning disjunct index
 *   2 atom_rel       int[]   per atom: relation id
 *   3 atom_vars      int[]   flattened atom variable lists
 *   4 atom_arity     int[]   per atom: number of variables
 *   5 fact_rel       int[]   per fact: relation id
 *   6 fact_elems     int[]   flattened fact element lists
 *   7 fact_arity     int[]   per fact: arity
 *   8 fact_tokens    uuid[]  per fact: provenance token (nil = certain)
 *   9 fact_probs     float8[] per fact: probability
 */
void decodeArgs(FunctionCallInfo fcinfo, UCQ &ucq,
                std::vector<FactRow> &rows)
{
  int n_disj, n_ad, n_ar, n_av, n_aa;
  const int32 *d_nvars = intArray(fcinfo, 0, "disjunct_nvars", n_disj);
  const int32 *a_disj  = intArray(fcinfo, 1, "atom_disjunct", n_ad);
  const int32 *a_rel   = intArray(fcinfo, 2, "atom_rel", n_ar);
  const int32 *a_vars  = intArray(fcinfo, 3, "atom_vars", n_av);
  const int32 *a_arity = intArray(fcinfo, 4, "atom_arity", n_aa);

  if (n_disj == 0)
    provsql_error("ucq_joint: the UCQ has no disjuncts");
  if (n_ad != n_ar || n_ad != n_aa)
    provsql_error("ucq_joint: atom arrays must have the same length");

  ucq.disjuncts.resize(n_disj);
  for (int d = 0; d < n_disj; ++d)
    ucq.disjuncts[d].n_vars = static_cast<unsigned>(d_nvars[d]);

  int voff = 0;
  for (int i = 0; i < n_ad; ++i) {
    const int d = a_disj[i];
    if (d < 0 || d >= n_disj)
      provsql_error("ucq_joint: atom disjunct index out of range");
    const int ar = a_arity[i];
    if (ar < 0 || voff + ar > n_av)
      provsql_error("ucq_joint: atom_vars shorter than declared arities");
    Atom atom;
    atom.relation_id = static_cast<unsigned>(a_rel[i]);
    atom.vars.reserve(ar);
    for (int k = 0; k < ar; ++k)
      atom.vars.push_back(static_cast<unsigned>(a_vars[voff + k]));
    voff += ar;
    ucq.disjuncts[d].atoms.push_back(std::move(atom));
  }

  int n_fr, n_fe, n_fa, n_fp, n_ft;
  const int32 *f_rel   = intArray(fcinfo, 5, "fact_rel", n_fr);
  const int32 *f_elems = intArray(fcinfo, 6, "fact_elems", n_fe);
  const int32 *f_arity = intArray(fcinfo, 7, "fact_arity", n_fa);
  ArrayType *toks = PG_ARGISNULL(8) ? NULL : PG_GETARG_ARRAYTYPE_P(8);
  ArrayType *prbs = PG_ARGISNULL(9) ? NULL : PG_GETARG_ARRAYTYPE_P(9);
  n_ft = checkedArrayLength(toks, "fact_tokens");
  n_fp = checkedArrayLength(prbs, "fact_probs");

  if (n_fr != n_fa || n_fr != n_ft || n_fr != n_fp)
    provsql_error("ucq_joint: fact arrays must have the same length");
  const pg_uuid_t *tok_data =
    (toks && n_ft) ? (const pg_uuid_t *) ARR_DATA_PTR(toks) : NULL;
  const float8 *prob_data =
    (prbs && n_fp) ? (const float8 *) ARR_DATA_PTR(prbs) : NULL;

  int eoff = 0;
  rows.reserve(n_fr);
  for (int i = 0; i < n_fr; ++i) {
    const int ar = f_arity[i];
    if (ar < 0 || eoff + ar > n_fe)
      provsql_error("ucq_joint: fact_elems shorter than declared arities");
    FactRow row;
    row.relation_id = static_cast<unsigned>(f_rel[i]);
    row.elements.reserve(ar);
    for (int k = 0; k < ar; ++k)
      row.elements.push_back(static_cast<unsigned long>(f_elems[eoff + k]));
    eoff += ar;
    bool nil = true;
    for (int b = 0; b < 16; ++b)
      if (tok_data[i].data[b] != 0)
        nil = false;
    if (!nil)
      row.token = uuid2string(tok_data[i]);
    row.prob = prob_data[i];
    rows.push_back(std::move(row));
  }
}

/** @brief Run the compiler over the decoded arguments. */
UCQJointCompiler::Result runFromArgs(FunctionCallInfo fcinfo)
{
  UCQ ucq;
  std::vector<FactRow> rows;
  decodeArgs(fcinfo, ucq, rows);

  const JointEncoding enc = JointEncoding::fromFacts(rows);
  const unsigned max_tw =
    static_cast<unsigned>(provsql_joint_max_treewidth);
  const std::size_t max_states =
    static_cast<std::size_t>(provsql_joint_max_states);
  try {
    return UCQJointCompiler::compile(enc, ucq, max_tw, max_states);
  } catch (TreeDecompositionException &) {
    provsql_error(
      "ucq_joint: joint treewidth exceeds the configured maximum (%d); "
      "fall back to the standard probability ladder",
      provsql_joint_max_treewidth);
    throw;   /* unreachable; placate the compiler */
  }
}

// ---------------------------------------------------------------------
// Tracked (correlated) path: walk the mmap circuit slice from the fact
// tokens, then run the merged DP.
// ---------------------------------------------------------------------

/** @brief Decode the five query arrays (arguments 0..4) into a @c UCQ. */
void decodeQuery(FunctionCallInfo fcinfo, UCQ &ucq)
{
  int n_disj, n_ad, n_ar, n_av, n_aa;
  const int32 *d_nvars = intArray(fcinfo, 0, "disjunct_nvars", n_disj);
  const int32 *a_disj  = intArray(fcinfo, 1, "atom_disjunct", n_ad);
  const int32 *a_rel   = intArray(fcinfo, 2, "atom_rel", n_ar);
  const int32 *a_vars  = intArray(fcinfo, 3, "atom_vars", n_av);
  const int32 *a_arity = intArray(fcinfo, 4, "atom_arity", n_aa);

  if (n_disj == 0)
    provsql_error("ucq_joint: the UCQ has no disjuncts");
  if (n_ad != n_ar || n_ad != n_aa)
    provsql_error("ucq_joint: atom arrays must have the same length");

  ucq.disjuncts.resize(n_disj);
  for (int d = 0; d < n_disj; ++d)
    ucq.disjuncts[d].n_vars = static_cast<unsigned>(d_nvars[d]);

  int voff = 0;
  for (int i = 0; i < n_ad; ++i) {
    const int d = a_disj[i];
    if (d < 0 || d >= n_disj)
      provsql_error("ucq_joint: atom disjunct index out of range");
    const int ar = a_arity[i];
    if (ar < 0 || voff + ar > n_av)
      provsql_error("ucq_joint: atom_vars shorter than declared arities");
    Atom atom;
    atom.relation_id = static_cast<unsigned>(a_rel[i]);
    atom.vars.reserve(ar);
    for (int k = 0; k < ar; ++k)
      atom.vars.push_back(static_cast<unsigned>(a_vars[voff + k]));
    voff += ar;
    ucq.disjuncts[d].atoms.push_back(std::move(atom));
  }
}

/**
 * @brief Build the circuit slice reachable from the fact tokens.
 *
 * Walks the mmap circuit (through @c getGenericCircuit) from each
 * distinct token down to @c gate_input leaves, keying slice nodes by
 * UUID so facts whose tokens share an internal gate or an event share
 * the slice node (the correlation the joint screen must see).  Stores
 * are normalised to arity ≤ 2 (a fan-in-@e k @c gate_times / @c gate_plus
 * becomes a left-deep binary tree).  @c gate_monus(one, x) is the
 * Boolean @c NOT x; constants fold; @c gate_mulinput / @c gate_mixture
 * and other non-Boolean gate types are rejected (the caller falls back
 * to the ladder).
 *
 * The returned per-node code is @c -2 for constant true, @c -1 for
 * constant false, and otherwise the slice index.
 */
struct SliceBuilder {
  std::vector<SliceGate> slice;
  std::map<std::string, int> uuid2node;   ///< UUID -> node code (cached).

  int binarize(SliceGateType t, std::vector<int> &children) {
    if (children.empty())
      return t == SliceGateType::AND ? -2 : -1;   // empty AND=true, OR=false
    int acc = children[0];
    for (std::size_t i = 1; i < children.size(); ++i) {
      SliceGate s;
      s.type = t;
      s.children = {static_cast<unsigned>(acc),
                    static_cast<unsigned>(children[i])};
      slice.push_back(std::move(s));
      acc = static_cast<int>(slice.size()) - 1;
    }
    return acc;
  }

  int walk(const GenericCircuit &gc, gate_t g) {
    const std::string u = gc.getUUID(g);
    if (!u.empty()) {
      auto it = uuid2node.find(u);
      if (it != uuid2node.end())
        return it->second;
    }
    int res;
    switch (gc.getGateType(g)) {
    case gate_input: {
      SliceGate s;
      s.type = SliceGateType::INPUT;
      s.prob = gc.getProb(g);
      s.token = u;
      slice.push_back(std::move(s));
      res = static_cast<int>(slice.size()) - 1;
      break;
    }
    case gate_one:
      res = -2;
      break;
    case gate_zero:
      res = -1;
      break;
    case gate_times: {
      std::vector<int> ch;
      bool cfalse = false;
      for (gate_t c : gc.getWires(g)) {
        int r = walk(gc, c);
        if (r == -1) { cfalse = true; break; }
        if (r == -2) continue;            // one: identity for AND
        ch.push_back(r);
      }
      res = cfalse ? -1 : binarize(SliceGateType::AND, ch);
      break;
    }
    case gate_plus: {
      std::vector<int> ch;
      bool ctrue = false;
      for (gate_t c : gc.getWires(g)) {
        int r = walk(gc, c);
        if (r == -2) { ctrue = true; break; }
        if (r == -1) continue;            // zero: identity for OR
        ch.push_back(r);
      }
      res = ctrue ? -2 : binarize(SliceGateType::OR, ch);
      break;
    }
    case gate_monus: {
      const auto &w = gc.getWires(g);
      if (w.size() != 2)
        throw JointCompilerException("malformed monus gate in circuit slice");
      const int a = walk(gc, w[0]);
      const int b = walk(gc, w[1]);
      if (a != -2)
        throw JointCompilerException(
                "non-Boolean monus in circuit slice (only the negation "
                "monus(one, x) is supported)");
      if (b == -2) res = -1;              // NOT true = false
      else if (b == -1) res = -2;         // NOT false = true
      else {
        SliceGate s;
        s.type = SliceGateType::NOT;
        s.children = {static_cast<unsigned>(b)};
        slice.push_back(std::move(s));
        res = static_cast<int>(slice.size()) - 1;
      }
      break;
    }
    default:
      throw JointCompilerException(
              "unsupported gate type in circuit slice (joint-width "
              "compilation handles input/and/or/not only)");
    }
    if (!u.empty())
      uuid2node[u] = res;
    return res;
  }
};

/** @brief Run the correlated (tracked) compilation from the arguments. */
UCQJointCompiler::Result runTrackedFromArgs(FunctionCallInfo fcinfo)
{
  UCQ ucq;
  decodeQuery(fcinfo, ucq);

  int n_fr, n_fe, n_fa, n_ft;
  const int32 *f_rel   = intArray(fcinfo, 5, "fact_rel", n_fr);
  const int32 *f_elems = intArray(fcinfo, 6, "fact_elems", n_fe);
  const int32 *f_arity = intArray(fcinfo, 7, "fact_arity", n_fa);
  ArrayType *toks = PG_ARGISNULL(8) ? NULL : PG_GETARG_ARRAYTYPE_P(8);
  n_ft = checkedArrayLength(toks, "fact_tokens");
  if (n_fr != n_fa || n_fr != n_ft)
    provsql_error("ucq_joint: fact arrays must have the same length");
  const pg_uuid_t *tok_data =
    (toks && n_ft) ? (const pg_uuid_t *) ARR_DATA_PTR(toks) : NULL;

  SliceBuilder sb;
  std::vector<Fact> facts;
  unsigned long n_elements = 0;
  int eoff = 0;
  for (int i = 0; i < n_fr; ++i) {
    const int ar = f_arity[i];
    if (ar < 0 || eoff + ar > n_fe)
      provsql_error("ucq_joint: fact_elems shorter than declared arities");
    Fact f;
    f.relation_id = static_cast<unsigned>(f_rel[i]);
    for (int k = 0; k < ar; ++k) {
      unsigned long e = static_cast<unsigned long>(f_elems[eoff + k]);
      f.elements.push_back(e);
      n_elements = std::max(n_elements, e + 1);
    }
    eoff += ar;
    // Walk the token's slice.
    GenericCircuit gc = getGenericCircuit(tok_data[i]);
    const int node = sb.walk(gc, gc.getGate(uuid2string(tok_data[i])));
    if (node == -1)
      continue;   // constant-false token: the fact is never present
    if (node == -2)
      f.kind = FactGateKind::CERTAIN;
    else {
      f.kind = FactGateKind::GATE;
      f.gate = static_cast<std::size_t>(node);
    }
    facts.push_back(std::move(f));
  }

  const JointEncoding enc =
    JointEncoding::fromCorrelated(std::move(facts), std::move(sb.slice),
                                  n_elements);
  const unsigned max_tw = static_cast<unsigned>(provsql_joint_max_treewidth);
  const std::size_t max_states =
    static_cast<std::size_t>(provsql_joint_max_states);
  try {
    return UCQJointCompiler::compile(enc, ucq, max_tw, max_states);
  } catch (TreeDecompositionException &) {
    provsql_error(
      "ucq_joint: joint treewidth exceeds the configured maximum (%d); "
      "fall back to the standard probability ladder",
      provsql_joint_max_treewidth);
    throw;
  }
}

} // namespace

/**
 * @brief PostgreSQL-callable entry point: exact Boolean UCQ probability.
 *
 * See @c decodeArgs() for the argument layout.
 */
Datum ucq_joint_evaluate(PG_FUNCTION_ARGS)
{
  try {
    auto result = runFromArgs(fcinfo);
    PG_RETURN_FLOAT8(result.dd.probabilityEvaluation());
  } catch (const std::exception &e) {
    provsql_error("ucq_joint: %s", e.what());
  } catch (...) {
    provsql_error("ucq_joint: unknown exception");
  }
  PG_RETURN_NULL();
}

/**
 * @brief PostgreSQL-callable entry point: UCQ probability plus
 *        compilation statistics (the three width columns and the
 *        structural stats).
 */
Datum ucq_joint_compile_stats(PG_FUNCTION_ARGS)
{
  try {
    auto result = runFromArgs(fcinfo);

    TupleDesc tupdesc;
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
      provsql_error("ucq_joint_compile_stats: expected composite return type");
    tupdesc = BlessTupleDesc(tupdesc);

    Datum values[7];
    bool nulls[7] = {false, false, false, false, false, false, false};
    values[0] = Float8GetDatum(result.dd.probabilityEvaluation());
    values[1] = Int32GetDatum(static_cast<int32>(result.stats.joint_treewidth));
    values[2] = Int32GetDatum(static_cast<int32>(result.stats.data_treewidth_lb));
    values[3] = Int32GetDatum(static_cast<int32>(result.stats.circuit_treewidth_lb));
    values[4] = Int64GetDatum(static_cast<int64>(result.stats.nb_bags));
    values[5] = Int64GetDatum(static_cast<int64>(result.stats.max_states));
    values[6] = Int64GetDatum(static_cast<int64>(result.stats.dd_size));

    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
  } catch (const std::exception &e) {
    provsql_error("ucq_joint_compile_stats: %s", e.what());
  } catch (...) {
    provsql_error("ucq_joint_compile_stats: unknown exception");
  }
  PG_RETURN_NULL();
}

/**
 * @brief PostgreSQL-callable entry point: compile the UCQ over correlated
 *        inputs and **materialise** its certified d-D into the store,
 *        returning the root provenance token.
 *
 * This is the architecturally-primary route: the joint-width compiler's
 * job is to build the deterministic, decomposable circuit; the answer --
 * probability, Shapley, expectation, any provenance-store evaluation --
 * is then obtained through the single standard entry point on the
 * returned token (e.g. @c probability_evaluate(token)), exploiting the
 * d-D certificate for linear-time evaluation.  Unlike the reachability
 * route, the token is NOT wrapped in the @c 'absorptive' marker: the d-D
 * is the exact Boolean provenance of the (non-recursive) UCQ.
 */
Datum ucq_joint_materialize_tracked(PG_FUNCTION_ARGS)
{
  try {
    auto result = runTrackedFromArgs(fcinfo);
    const auto uuid_of =
      materializeCertifiedDD(result.dd, {result.dd.getRoot()});
    pg_uuid_t *u = (pg_uuid_t *) palloc(sizeof(pg_uuid_t));
    *u = uuid_of.at(result.dd.getRoot());
    PG_RETURN_UUID_P(u);
  } catch (const std::exception &e) {
    provsql_error("ucq_joint_materialize: %s", e.what());
  } catch (...) {
    provsql_error("ucq_joint_materialize: unknown exception");
  }
  PG_RETURN_NULL();
}

/**
 * @brief PostgreSQL-callable entry point: exact Boolean UCQ probability
 *        with **correlated** inputs (tokens are real provenance gates).
 *
 * Like @c ucq_joint_evaluate, but the facts carry real provenance tokens
 * (no explicit probabilities): the circuit slice is walked from the
 * mmap store, so tokens that are internal gates over shared events --
 * tracked views, user constraint circuits -- are handled natively, the
 * capability that no other exact ProvSQL method offers with a width
 * guarantee.  Argument layout: the five query arrays (0..4), then
 * @c fact_rel (5), @c fact_elems (6), @c fact_arity (7), @c fact_tokens
 * (8).
 */
Datum ucq_joint_evaluate_tracked(PG_FUNCTION_ARGS)
{
  try {
    auto result = runTrackedFromArgs(fcinfo);
    PG_RETURN_FLOAT8(result.dd.probabilityEvaluation());
  } catch (const std::exception &e) {
    provsql_error("ucq_joint: %s", e.what());
  } catch (...) {
    provsql_error("ucq_joint: unknown exception");
  }
  PG_RETURN_NULL();
}

/**
 * @brief PostgreSQL-callable entry point: correlated UCQ probability plus
 *        compilation statistics (the three width columns substantiate
 *        Prop. 4.2.11: a correlated instance can have small data and
 *        circuit widths but large joint width).
 */
Datum ucq_joint_compile_stats_tracked(PG_FUNCTION_ARGS)
{
  try {
    auto result = runTrackedFromArgs(fcinfo);

    TupleDesc tupdesc;
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
      provsql_error("ucq_joint_compile_stats: expected composite return type");
    tupdesc = BlessTupleDesc(tupdesc);

    Datum values[7];
    bool nulls[7] = {false, false, false, false, false, false, false};
    values[0] = Float8GetDatum(result.dd.probabilityEvaluation());
    values[1] = Int32GetDatum(static_cast<int32>(result.stats.joint_treewidth));
    values[2] = Int32GetDatum(static_cast<int32>(result.stats.data_treewidth_lb));
    values[3] = Int32GetDatum(static_cast<int32>(result.stats.circuit_treewidth_lb));
    values[4] = Int64GetDatum(static_cast<int64>(result.stats.nb_bags));
    values[5] = Int64GetDatum(static_cast<int64>(result.stats.max_states));
    values[6] = Int64GetDatum(static_cast<int64>(result.stats.dd_size));

    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
  } catch (const std::exception &e) {
    provsql_error("ucq_joint_compile_stats: %s", e.what());
  } catch (...) {
    provsql_error("ucq_joint_compile_stats: unknown exception");
  }
  PG_RETURN_NULL();
}
