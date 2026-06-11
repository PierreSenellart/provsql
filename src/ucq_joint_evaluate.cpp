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
PG_FUNCTION_INFO_V1(ucq_joint_answers_swept);
PG_FUNCTION_INFO_V1(ucq_joint_answers_swept_tracked);
PG_FUNCTION_INFO_V1(ucq_joint_answers_onedp);
PG_FUNCTION_INFO_V1(ucq_joint_answers_onedp_tracked);
}

#include "c_cpp_compatibility.h"
#include "JointEncoding.h"
#include "UCQJointCompiler.h"
#include "CircuitFromMMap.h"
#include "CertifiedDDMaterialize.h"
#include "GenericCircuit.h"
#include "provsql_utils_cpp.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

  /// A @c gate_mulinput leaf deferred until @c expandMulBlocks() can see
  /// the whole categorical block it belongs to.  @c block is the UUID of
  /// the shared key gate (all values of one repair_key block share it);
  /// @c value_index orders the values; @c prob is the value's mass.
  struct MulRef { std::string block; unsigned value_index; double prob; };
  std::vector<MulRef> mulrefs;            ///< Deferred mulinput leaves.
  std::vector<int> mul_resolved;          ///< MulRef index -> slice node.
  unsigned mulsb_counter = 0;             ///< Fresh stick-breaking event ids.

  /// Node codes below this are deferred mulinput references; the actual
  /// MulRef index is @c MULREF_BASE - code (so distinct from the -1/-2
  /// constants and from any non-negative slice index).
  static constexpr int MULREF_BASE = -1000000;

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
    case gate_mulinput: {
      // A BID / repair_key categorical value.  We cannot stick-break it
      // here -- that needs the cumulative mass of every value in its block,
      // which is scattered across the facts.  Defer it: record the value
      // and return a placeholder code; expandMulBlocks() resolves the whole
      // block at once (after every fact has been walked) into shared
      // independent IN/NOT/AND slice nodes that enforce the mutual
      // exclusivity (the block's values share stick-breaking events -- the
      // correlation the joint screen must see).
      const auto &w = gc.getWires(g);
      if (w.empty())
        throw JointCompilerException("malformed mulinput gate (no key wire)");
      const std::string key = gc.getUUID(w[0]);
      if (key.empty())
        throw JointCompilerException(
                "mulinput key gate has no UUID (cannot group the block)");
      MulRef m;
      m.block = key;
      m.value_index = gc.getInfos(g).first;
      m.prob = gc.getProb(g);
      mulrefs.push_back(std::move(m));
      res = MULREF_BASE - static_cast<int>(mulrefs.size() - 1);
      break;
    }
    default:
      throw JointCompilerException(
              "unsupported gate type in circuit slice (joint-width "
              "compilation handles input/and/or/not and mulinput only)");
    }
    if (!u.empty())
      uuid2node[u] = res;
    return res;
  }

  /// Append a fresh independent IN (INPUT) slice node of mass @p p.  Its
  /// token is synthetic and unique so JointEncoding treats it as a brand
  /// new event (a stick-breaking coin), never colliding with a real input.
  int emitInput(double p) {
    SliceGate s;
    s.type = SliceGateType::INPUT;
    s.prob = p;
    s.token = "\x01mulsb:" + std::to_string(mulsb_counter++);
    slice.push_back(std::move(s));
    return static_cast<int>(slice.size()) - 1;
  }

  /// Append a NOT slice node over the existing node @p child (>= 0).
  int emitNot(int child) {
    SliceGate s;
    s.type = SliceGateType::NOT;
    s.children = {static_cast<unsigned>(child)};
    slice.push_back(std::move(s));
    return static_cast<int>(slice.size()) - 1;
  }

  /// Stick-break the value range [@p start, @p end] of one block (balanced
  /// bisection, mirroring BooleanCircuit::rewriteMultivaluedGatesRec): the
  /// IN coin at each split is SHARED by both halves (so the values are
  /// mutually exclusive), and each value's slice node is the AND of the
  /// path of coins (positive on the left, negated on the right) that
  /// selects it.  @p prefix carries that path of already-chosen coins.
  void expandRec(const std::vector<int> &idxs, const std::vector<double> &cum,
                 unsigned start, unsigned end, std::vector<int> &prefix) {
    if (start == end) {
      mul_resolved[idxs[start]] =
        prefix.empty() ? emitInput(1.0)               // sole value, mass 1
                       : binarize(SliceGateType::AND, prefix);
      return;
    }
    const unsigned mid = (start + end) / 2;
    const double prev_start = (start == 0) ? 0. : cum[start - 1];
    const int g = emitInput((cum[mid] - prev_start) / (cum[end] - prev_start));
    const int ng = emitNot(g);
    prefix.push_back(g);
    expandRec(idxs, cum, start, mid, prefix);
    prefix.pop_back();
    prefix.push_back(ng);
    expandRec(idxs, cum, mid + 1, end, prefix);
    prefix.pop_back();
  }

  /// Resolve every deferred mulinput leaf.  Group the MulRefs by block,
  /// order each block's values, stick-break it into shared IN/NOT/AND slice
  /// nodes, then rewrite every deferred reference (in slice children) to the
  /// resolved node.  Call once, after every fact token has been walked.
  void expandMulBlocks() {
    if (mulrefs.empty())
      return;
    mul_resolved.assign(mulrefs.size(), -1);
    std::map<std::string, std::vector<int>> by_block;
    for (int i = 0; i < static_cast<int>(mulrefs.size()); ++i)
      by_block[mulrefs[i].block].push_back(i);

    for (auto &kv : by_block) {
      std::vector<int> &idxs = kv.second;
      std::sort(idxs.begin(), idxs.end(), [&](int a, int b) {
        return mulrefs[a].value_index < mulrefs[b].value_index;
      });
      const unsigned n = static_cast<unsigned>(idxs.size());
      std::vector<double> cum(n);
      double c = 0.;
      for (unsigned i = 0; i < n; ++i) {
        c += mulrefs[idxs[i]].prob;
        cum[i] = c;
      }
      std::vector<int> prefix;
      // When the present values do not exhaust the block (their masses sum
      // to less than 1, the rest being "key absent"), gate the whole block
      // behind one block-active coin of that total mass.
      constexpr double eps = std::numeric_limits<double>::epsilon() * 10;
      if (cum[n - 1] < 1. - eps)
        prefix.push_back(emitInput(cum[n - 1]));
      expandRec(idxs, cum, 0, n - 1, prefix);
    }

    // Rewrite deferred references buried as children of AND/OR/NOT nodes.
    for (SliceGate &sg : slice)
      for (unsigned &ch : sg.children) {
        const int ci = static_cast<int>(ch);
        if (ci <= MULREF_BASE)
          ch = static_cast<unsigned>(mul_resolved[MULREF_BASE - ci]);
      }
  }

  /// Map a raw @c walk() code to a final slice node (resolving any deferred
  /// mulinput reference).  Returns -2 (true), -1 (false), or a slice index.
  int resolveCode(int code) const {
    if (code <= MULREF_BASE)
      return mul_resolved[MULREF_BASE - code];
    return code;
  }
};

/**
 * @brief Walk the fact tokens at arguments @p base..base+3 into a fact list
 *        and its circuit slice.
 *
 * @p base+0 fact_rel, @p base+1 fact_elems, @p base+2 fact_arity,
 * @p base+3 fact_tokens (uuid[]).  Each token is walked through the mmap
 * circuit (shared slice nodes for shared gates -- the correlation the joint
 * screen must see).  On return @p facts holds the present facts (their gate
 * indices into @p slice), @p n_elements is the element-id bound, and
 * @p has_internal is true iff the slice has a non-INPUT gate (correlated).
 * Shared by the Boolean tracked compile and the per-answer tracked sweep.
 */
void buildTrackedFacts(FunctionCallInfo fcinfo, int base,
                       std::vector<Fact> &facts, std::vector<SliceGate> &slice,
                       unsigned long &n_elements, bool &has_internal)
{
  int n_fr, n_fe, n_fa, n_ft;
  const int32 *f_rel   = intArray(fcinfo, base,     "fact_rel", n_fr);
  const int32 *f_elems = intArray(fcinfo, base + 1, "fact_elems", n_fe);
  const int32 *f_arity = intArray(fcinfo, base + 2, "fact_arity", n_fa);
  ArrayType *toks =
    PG_ARGISNULL(base + 3) ? NULL : PG_GETARG_ARRAYTYPE_P(base + 3);
  n_ft = checkedArrayLength(toks, "fact_tokens");
  if (n_fr != n_fa || n_fr != n_ft)
    provsql_error("ucq_joint: fact arrays must have the same length");
  const pg_uuid_t *tok_data =
    (toks && n_ft) ? (const pg_uuid_t *) ARR_DATA_PTR(toks) : NULL;

  SliceBuilder sb;
  // Walk every fact's token first, keeping its raw slice code; mulinput
  // (BID) leaves resolve only after the whole batch is seen (a value's
  // block-mates live in other facts), so we finalise the facts in a second
  // pass once expandMulBlocks() has stick-broken the blocks.
  std::vector<std::pair<Fact, int>> pending;
  pending.reserve(n_fr);
  n_elements = 0;
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
    pending.emplace_back(std::move(f), node);
  }

  sb.expandMulBlocks();

  facts.clear();
  facts.reserve(pending.size());
  for (auto &pf : pending) {
    const int node = sb.resolveCode(pf.second);
    if (node == -1)
      continue;   // constant-false token: the fact is never present
    Fact &f = pf.first;
    if (node == -2)
      f.kind = FactGateKind::CERTAIN;
    else {
      f.kind = FactGateKind::GATE;
      f.gate = static_cast<std::size_t>(node);
    }
    facts.push_back(std::move(f));
  }

  has_internal = false;
  for (const auto &sg : sb.slice)
    if (sg.type != SliceGateType::INPUT) {
      has_internal = true;
      break;
    }
  slice = std::move(sb.slice);
}

/** @brief Run the correlated (tracked) compilation from the arguments. */
UCQJointCompiler::Result runTrackedFromArgs(FunctionCallInfo fcinfo)
{
  UCQ ucq;
  decodeQuery(fcinfo, ucq);

  std::vector<Fact> facts;
  std::vector<SliceGate> slice;
  unsigned long n_elements;
  bool has_internal;
  buildTrackedFacts(fcinfo, 5, facts, slice, n_elements, has_internal);

  const unsigned max_tw = static_cast<unsigned>(provsql_joint_max_treewidth);
  const std::size_t max_states =
    static_cast<std::size_t>(provsql_joint_max_states);

  // Dispatch by input class: when the slice has no internal gates (every
  // fact gated by a bare gate_input leaf -- the non-correlated / TID
  // regime), use the data-graph fast path, which decomposes only the
  // data (no gate vertices); the joint screen there is the data
  // treewidth.  Internal gates (correlated inputs) need the full joint
  // data+circuit decomposition.  Both give the same probability; the
  // fast path is just cheaper.  A shared leaf across *different* element
  // tuples is a real correlation that fromFacts rejects -- fall back to
  // the joint path then.
  if (!has_internal) {
    std::vector<FactRow> rows;
    rows.reserve(facts.size());
    for (const auto &f : facts) {
      FactRow row;
      row.relation_id = f.relation_id;
      row.elements = f.elements;
      if (f.kind == FactGateKind::GATE) {
        row.token = slice[f.gate].token;
        row.prob = slice[f.gate].prob;
      }   // CERTAIN: empty token, prob 1
      rows.push_back(std::move(row));
    }
    try {
      const JointEncoding enc = JointEncoding::fromFacts(rows);
      try {
        return UCQJointCompiler::compile(enc, ucq, max_tw, max_states);
      } catch (TreeDecompositionException &) {
        provsql_error(
          "ucq_joint: data treewidth exceeds the configured maximum (%d); "
          "fall back to the standard probability ladder",
          provsql_joint_max_treewidth);
      }
    } catch (const JointCompilerException &) {
      // A gate_input shared across different element tuples: genuinely
      // correlated; fall through to the joint path below.
    }
  }

  const JointEncoding enc =
    JointEncoding::fromCorrelated(std::move(facts), std::move(slice),
                                  n_elements);
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

/**
 * @brief PostgreSQL-callable SRF: per-answer probabilities in a SINGLE
 *        PASS over the shared encoding + tree decomposition.
 *
 * Argument layout: the columnar query + facts (args 0-9, as
 * @c ucq_joint_evaluate / @c decodeArgs), then
 *
 *   10 head_vars       int[]  : query-variable indices of the head
 *   11 candidate_heads int[]  : candidate head tuples, row-major,
 *                               |head_vars| element ids each
 *
 * Returns @c SETOF (head int[], probability float8): one row per answer
 * with non-zero probability.  The encoding and the joint tree
 * decomposition are built once; each candidate is evaluated by a
 * head-pinned bottom-up sweep (see @c UCQJointCompiler::compileAnswers).
 */
Datum ucq_joint_answers_swept(PG_FUNCTION_ARGS)
{
  ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
  MemoryContext per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
  MemoryContext oldcontext = MemoryContextSwitchTo(per_query_ctx);
  TupleDesc tupdesc = rsinfo->expectedDesc;
  Tuplestorestate *tupstore =
    tuplestore_begin_heap(rsinfo->allowedModes & SFRM_Materialize_Random,
                          false, work_mem);
  rsinfo->returnMode = SFRM_Materialize;
  rsinfo->setResult = tupstore;

  try {
    UCQ ucq;
    std::vector<FactRow> rows;
    decodeArgs(fcinfo, ucq, rows);

    int n_hv, n_cand;
    const int32 *hv  = intArray(fcinfo, 10, "head_vars", n_hv);
    const int32 *cnd = intArray(fcinfo, 11, "candidate_heads", n_cand);
    if (n_hv <= 0)
      provsql_error("ucq_joint_answers_swept: empty head_vars");
    if (n_cand % n_hv != 0)
      provsql_error("ucq_joint_answers_swept: candidate_heads length is not "
                    "a multiple of the head arity");

    std::vector<unsigned> head_vars(n_hv);
    for (int i = 0; i < n_hv; ++i)
      head_vars[i] = static_cast<unsigned>(hv[i]);
    std::vector<std::vector<unsigned long> > candidates;
    candidates.reserve(static_cast<std::size_t>(n_cand / n_hv));
    for (int i = 0; i + n_hv <= n_cand; i += n_hv) {
      std::vector<unsigned long> t(n_hv);
      for (int k = 0; k < n_hv; ++k)
        t[k] = static_cast<unsigned long>(cnd[i + k]);
      candidates.push_back(std::move(t));
    }

    const JointEncoding enc = JointEncoding::fromFacts(rows);
    const unsigned max_tw =
      static_cast<unsigned>(provsql_joint_max_treewidth);
    const std::size_t max_states =
      static_cast<std::size_t>(provsql_joint_max_states);

    std::vector<UCQJointCompiler::Answer> answers;
    try {
      answers = UCQJointCompiler::compileAnswers(enc, ucq, head_vars,
                                                 candidates, max_tw, max_states);
    } catch (TreeDecompositionException &) {
      provsql_error(
        "ucq_joint_answers_swept: joint treewidth exceeds the configured "
        "maximum (%d)", provsql_joint_max_treewidth);
    }

    for (const auto &a : answers) {
      Datum *elems = (Datum *) palloc(a.head.size() * sizeof(Datum));
      for (std::size_t i = 0; i < a.head.size(); ++i)
        elems[i] = Int32GetDatum(static_cast<int32>(a.head[i]));
      ArrayType *arr = construct_array(elems, static_cast<int>(a.head.size()),
                                       INT4OID, sizeof(int32), true,
                                       TYPALIGN_INT);
      Datum values[2] = { PointerGetDatum(arr), Float8GetDatum(a.probability) };
      bool nulls[2] = { false, false };
      tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
  } catch (const std::exception &e) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("ucq_joint_answers_swept: %s", e.what());
  } catch (...) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("ucq_joint_answers_swept: unknown exception");
  }

  MemoryContextSwitchTo(oldcontext);
  PG_RETURN_NULL();
}

/**
 * @brief PostgreSQL-callable SRF: per-answer probabilities for the
 *        CORRELATED regime in a SINGLE PASS over the shared encoding.
 *
 * The per-answer counterpart of @c ucq_joint_evaluate_tracked: the fact
 * tokens are walked through the circuit ONCE, the correlated joint encoding
 * (data + circuit slice) and its tree decomposition are built ONCE, and each
 * candidate head tuple is evaluated by a head-pinned merged DP sweep.  This
 * shares the gather/walk/encode/decompose stages across all answers; only the
 * pinned DP is rerun per answer.
 *
 * Argument layout:
 *
 *    0-4 query             (as @c decodeQuery)
 *    5   head_vars       int[]  : query-variable indices of the head
 *    6   candidate_heads int[]  : candidate head tuples, row-major
 *    7   fact_rel        int[]
 *    8   fact_elems      int[]
 *    9   fact_arity      int[]
 *   10   fact_tokens     uuid[] : real provenance tokens (walked, not probs)
 *
 * Returns @c SETOF (head int[], probability float8): one row per answer with
 * non-zero probability.
 */
Datum ucq_joint_answers_swept_tracked(PG_FUNCTION_ARGS)
{
  ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
  MemoryContext per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
  MemoryContext oldcontext = MemoryContextSwitchTo(per_query_ctx);
  TupleDesc tupdesc = rsinfo->expectedDesc;
  Tuplestorestate *tupstore =
    tuplestore_begin_heap(rsinfo->allowedModes & SFRM_Materialize_Random,
                          false, work_mem);
  rsinfo->returnMode = SFRM_Materialize;
  rsinfo->setResult = tupstore;

  try {
    UCQ ucq;
    decodeQuery(fcinfo, ucq);

    int n_hv, n_cand;
    const int32 *hv  = intArray(fcinfo, 5, "head_vars", n_hv);
    const int32 *cnd = intArray(fcinfo, 6, "candidate_heads", n_cand);
    if (n_hv <= 0)
      provsql_error("ucq_joint_answers_swept_tracked: empty head_vars");
    if (n_cand % n_hv != 0)
      provsql_error("ucq_joint_answers_swept_tracked: candidate_heads length "
                    "is not a multiple of the head arity");

    std::vector<unsigned> head_vars(n_hv);
    for (int i = 0; i < n_hv; ++i)
      head_vars[i] = static_cast<unsigned>(hv[i]);
    std::vector<std::vector<unsigned long> > candidates;
    candidates.reserve(static_cast<std::size_t>(n_cand / n_hv));
    for (int i = 0; i + n_hv <= n_cand; i += n_hv) {
      std::vector<unsigned long> t(n_hv);
      for (int k = 0; k < n_hv; ++k)
        t[k] = static_cast<unsigned long>(cnd[i + k]);
      candidates.push_back(std::move(t));
    }

    std::vector<Fact> facts;
    std::vector<SliceGate> slice;
    unsigned long n_elements;
    bool has_internal;
    buildTrackedFacts(fcinfo, 7, facts, slice, n_elements, has_internal);

    const unsigned max_tw =
      static_cast<unsigned>(provsql_joint_max_treewidth);
    const std::size_t max_states =
      static_cast<std::size_t>(provsql_joint_max_states);

    // The correlated joint encoding (data + circuit slice).  An all-INPUT
    // slice (the TID/BID regime reached through a tracked token) is handled
    // here too, just without the data-graph fast path -- the columnar
    // ucq_joint_answers_swept is the fast path for that case.
    const JointEncoding enc =
      JointEncoding::fromCorrelated(std::move(facts), std::move(slice),
                                    n_elements);

    std::vector<UCQJointCompiler::Answer> answers;
    try {
      answers = UCQJointCompiler::compileAnswers(enc, ucq, head_vars,
                                                 candidates, max_tw, max_states);
    } catch (TreeDecompositionException &) {
      provsql_error(
        "ucq_joint_answers_swept_tracked: joint treewidth exceeds the "
        "configured maximum (%d)", provsql_joint_max_treewidth);
    }

    for (const auto &a : answers) {
      Datum *elems = (Datum *) palloc(a.head.size() * sizeof(Datum));
      for (std::size_t i = 0; i < a.head.size(); ++i)
        elems[i] = Int32GetDatum(static_cast<int32>(a.head[i]));
      ArrayType *arr = construct_array(elems, static_cast<int>(a.head.size()),
                                       INT4OID, sizeof(int32), true,
                                       TYPALIGN_INT);
      Datum values[2] = { PointerGetDatum(arr), Float8GetDatum(a.probability) };
      bool nulls[2] = { false, false };
      tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
  } catch (const std::exception &e) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("ucq_joint_answers_swept_tracked: %s", e.what());
  } catch (...) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("ucq_joint_answers_swept_tracked: unknown exception");
  }

  MemoryContextSwitchTo(oldcontext);
  PG_RETURN_NULL();
}

/**
 * @brief PostgreSQL-callable SRF: per-answer probabilities via the full
 *        TOP-DOWN single DP (data-graph regime).
 *
 * Unlike @c ucq_joint_answers_swept (k head-pinned sweeps sharing only the
 * decomposition), this runs ONE bottom-up sweep that emits one circuit root
 * per answer -- the head variables are a state-level key and each answer is
 * emitted when its head elements leave the tree decomposition.  The answers
 * are DISCOVERED by the sweep, so no candidate list is taken.
 *
 * Argument layout: the columnar query + facts (args 0-9, as
 * @c ucq_joint_evaluate / @c decodeArgs), then
 *
 *   10 head_vars int[] : query-variable indices of the head.
 *
 * Returns @c SETOF (head int[], probability float8).
 */
Datum ucq_joint_answers_onedp(PG_FUNCTION_ARGS)
{
  ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
  MemoryContext per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
  MemoryContext oldcontext = MemoryContextSwitchTo(per_query_ctx);
  TupleDesc tupdesc = rsinfo->expectedDesc;
  Tuplestorestate *tupstore =
    tuplestore_begin_heap(rsinfo->allowedModes & SFRM_Materialize_Random,
                          false, work_mem);
  rsinfo->returnMode = SFRM_Materialize;
  rsinfo->setResult = tupstore;

  try {
    UCQ ucq;
    std::vector<FactRow> rows;
    decodeArgs(fcinfo, ucq, rows);

    int n_hv;
    const int32 *hv = intArray(fcinfo, 10, "head_vars", n_hv);
    if (n_hv <= 0)
      provsql_error("ucq_joint_answers_onedp: empty head_vars");
    std::vector<unsigned> head_vars(n_hv);
    for (int i = 0; i < n_hv; ++i)
      head_vars[i] = static_cast<unsigned>(hv[i]);

    const JointEncoding enc = JointEncoding::fromFacts(rows);
    const unsigned max_tw =
      static_cast<unsigned>(provsql_joint_max_treewidth);
    const std::size_t max_states =
      static_cast<std::size_t>(provsql_joint_max_states);

    std::vector<UCQJointCompiler::Answer> answers;
    try {
      answers = UCQJointCompiler::compileAnswersOneDP(enc, ucq, head_vars,
                                                      max_tw, max_states);
    } catch (TreeDecompositionException &) {
      provsql_error(
        "ucq_joint_answers_onedp: joint treewidth exceeds the configured "
        "maximum (%d)", provsql_joint_max_treewidth);
    }

    for (const auto &a : answers) {
      Datum *elems = (Datum *) palloc(a.head.size() * sizeof(Datum));
      for (std::size_t i = 0; i < a.head.size(); ++i)
        elems[i] = Int32GetDatum(static_cast<int32>(a.head[i]));
      ArrayType *arr = construct_array(elems, static_cast<int>(a.head.size()),
                                       INT4OID, sizeof(int32), true,
                                       TYPALIGN_INT);
      Datum values[2] = { PointerGetDatum(arr), Float8GetDatum(a.probability) };
      bool nulls[2] = { false, false };
      tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
  } catch (const std::exception &e) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("ucq_joint_answers_onedp: %s", e.what());
  } catch (...) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("ucq_joint_answers_onedp: unknown exception");
  }

  MemoryContextSwitchTo(oldcontext);
  PG_RETURN_NULL();
}

/**
 * @brief PostgreSQL-callable SRF: per-answer probabilities via the full
 *        TOP-DOWN single DP over CORRELATED inputs.
 *
 * The correlated counterpart of @c ucq_joint_answers_onedp: the fact tokens
 * are real provenance gates, walked through the circuit ONCE, the correlated
 * joint encoding (data + circuit slice) and its decomposition are built ONCE,
 * and a single bottom-up sweep emits one d-DNNF root per answer.  Answers are
 * discovered (no candidate list).
 *
 * Argument layout: 0-4 query (as @c decodeQuery), then
 *   5 head_vars  int[]  : query-variable indices of the head
 *   6 fact_rel   int[]
 *   7 fact_elems int[]
 *   8 fact_arity int[]
 *   9 fact_tokens uuid[] : real provenance tokens (walked, not probs)
 *
 * Returns @c SETOF (head int[], probability float8).
 */
Datum ucq_joint_answers_onedp_tracked(PG_FUNCTION_ARGS)
{
  ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
  MemoryContext per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
  MemoryContext oldcontext = MemoryContextSwitchTo(per_query_ctx);
  TupleDesc tupdesc = rsinfo->expectedDesc;
  Tuplestorestate *tupstore =
    tuplestore_begin_heap(rsinfo->allowedModes & SFRM_Materialize_Random,
                          false, work_mem);
  rsinfo->returnMode = SFRM_Materialize;
  rsinfo->setResult = tupstore;

  try {
    UCQ ucq;
    decodeQuery(fcinfo, ucq);

    int n_hv;
    const int32 *hv = intArray(fcinfo, 5, "head_vars", n_hv);
    if (n_hv <= 0)
      provsql_error("ucq_joint_answers_onedp_tracked: empty head_vars");
    std::vector<unsigned> head_vars(n_hv);
    for (int i = 0; i < n_hv; ++i)
      head_vars[i] = static_cast<unsigned>(hv[i]);

    std::vector<Fact> facts;
    std::vector<SliceGate> slice;
    unsigned long n_elements;
    bool has_internal;
    buildTrackedFacts(fcinfo, 6, facts, slice, n_elements, has_internal);

    const unsigned max_tw =
      static_cast<unsigned>(provsql_joint_max_treewidth);
    const std::size_t max_states =
      static_cast<std::size_t>(provsql_joint_max_states);

    const JointEncoding enc =
      JointEncoding::fromCorrelated(std::move(facts), std::move(slice),
                                    n_elements);

    std::vector<UCQJointCompiler::Answer> answers;
    try {
      answers = UCQJointCompiler::compileAnswersOneDP(enc, ucq, head_vars,
                                                      max_tw, max_states);
    } catch (TreeDecompositionException &) {
      provsql_error(
        "ucq_joint_answers_onedp_tracked: joint treewidth exceeds the "
        "configured maximum (%d)", provsql_joint_max_treewidth);
    }

    for (const auto &a : answers) {
      Datum *elems = (Datum *) palloc(a.head.size() * sizeof(Datum));
      for (std::size_t i = 0; i < a.head.size(); ++i)
        elems[i] = Int32GetDatum(static_cast<int32>(a.head[i]));
      ArrayType *arr = construct_array(elems, static_cast<int>(a.head.size()),
                                       INT4OID, sizeof(int32), true,
                                       TYPALIGN_INT);
      Datum values[2] = { PointerGetDatum(arr), Float8GetDatum(a.probability) };
      bool nulls[2] = { false, false };
      tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
  } catch (const std::exception &e) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("ucq_joint_answers_onedp_tracked: %s", e.what());
  } catch (...) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("ucq_joint_answers_onedp_tracked: unknown exception");
  }

  MemoryContextSwitchTo(oldcontext);
  PG_RETURN_NULL();
}
