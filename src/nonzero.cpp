/**
 * @file nonzero.cpp
 * @brief SQL function @c provsql.true_nonzero() – structural universal-zero
 *        test backing the default mode of @c provsql.nonzero().
 *
 * Decides whether a circuit is *provably* the semiring zero – zero in every
 * m-semiring under every leaf valuation – by the sound structural rules:
 * @c gate_zero; a ⊗ with a zero factor; a ⊕ (or δ, or a transparent
 * single-child wrapper) all of whose children are zero; a ⊖ whose left
 * operand is zero; a semimod whose provenance side is zero.  Comparison
 * gates are resolved through the Boolean-abstraction pipeline (the same
 * @c provsql_having world enumeration every evaluator applies): a @c cmp
 * whose satisfying-world set is empty is the empty disjunction, i.e. zero
 * in every semiring, and surfaces in the Boolean circuit as a childless OR
 * (the @c BoolExpr semiring's @c zero()).  Everything else – input gates,
 * undecided probabilistic comparisons, mixtures, conditioning – blocks the
 * proof: the verdict is then @c true (not provably zero), never a dropped
 * row.
 *
 * Exact zero-ness in the free m-semiring is the word problem for the
 * m-semiring axioms, whose decidability is open; drop-only-on-proof is the
 * specified contract, so a sound under-approximation is exactly right.
 */
extern "C"
{
#include "postgres.h"
#include "fmgr.h"
#include "utils/uuid.h"
#include "provsql_utils.h"
}

#include <exception>
#include <unordered_map>

#include "BooleanCircuit.h"
#include "CircuitFromMMap.h"
#include "GenericCircuit.h"
#include "provsql_utils_cpp.h"

extern "C"
{
PG_FUNCTION_INFO_V1(true_nonzero);
}

/**
 * @brief Structural proof of universal zero-ness of @p g, memoized over the
 *        DAG.
 *
 * @param gc        The loaded circuit (post the load-time simplification
 *                  passes, so RangeCheck-decided RV comparisons are already
 *                  constants).
 * @param g         Gate to test.
 * @param bc        Boolean abstraction of @p gc, when it could be built;
 *                  used only to read comparison gates' world-set emptiness.
 * @param gc_to_bc  Gate map into @p bc.
 * @param memo      Verdict cache (the circuit is a DAG).
 */
static bool provably_zero(
  const GenericCircuit &gc, gate_t g,
  const BooleanCircuit *bc,
  const std::unordered_map<gate_t, gate_t> *gc_to_bc,
  std::unordered_map<gate_t, bool> &memo)
{
  auto it = memo.find(g);
  if(it != memo.end())
    return it->second;
  memo[g] = false;    // provisional: blocks pathological cycles safely

  const auto &w = gc.getWires(g);
  bool z = false;

  switch(gc.getGateType(g)) {
  case gate_zero:
    z = true;
    break;

  case gate_plus:
    // The empty ⊕ is 0; any non-zero child blocks the proof.
    z = true;
    for(auto child : w)
      if(!provably_zero(gc, child, bc, gc_to_bc, memo)) {
        z = false;
        break;
      }
    break;

  case gate_times:
    for(auto child : w)
      if(provably_zero(gc, child, bc, gc_to_bc, memo)) {
        z = true;
        break;
      }
    break;

  case gate_monus:
    // 0 ⊖ x = 0 in every m-semiring; nothing else is provable here
    // (x ⊖ x is folded to gate_zero by the load-time simplifier).
    z = !w.empty() && provably_zero(gc, w[0], bc, gc_to_bc, memo);
    break;

  case gate_delta:      // δ(0) = 0
  case gate_project:    // transparent single-child wrappers
  case gate_annotation:
  case gate_assumed:
    z = !w.empty() && provably_zero(gc, w[0], bc, gc_to_bc, memo);
    break;

  case gate_semimod:
    // value ⊛ K with K = 0 is the semimodule zero; wires are [K, value].
    z = !w.empty() && provably_zero(gc, w[0], bc, gc_to_bc, memo);
    break;

  case gate_cmp:
    // Resolved through the Boolean abstraction: the cmp's specified
    // semantics is the disjunction over its satisfying worlds, and the
    // empty disjunction (the BoolExpr semiring's zero(), a childless OR)
    // is zero in every semiring.  Each term of a non-empty disjunction is
    // satisfiable by construction, so a non-childless expansion is never
    // universally zero on its own.
    if(bc && gc_to_bc) {
      auto jt = gc_to_bc->find(g);
      if(jt != gc_to_bc->end() &&
         bc->getGateType(jt->second) == BooleanGate::OR &&
         bc->getWires(jt->second).empty())
        z = true;
    }
    break;

  default:
    // Inputs, values, mulinputs, undecided RV comparisons, mixtures,
    // conditioning, arithmetic…: not provably zero.
    break;
  }

  memo[g] = z;
  return z;
}

/** @brief PostgreSQL-callable wrapper: the default mode of provsql.nonzero(). */
Datum true_nonzero(PG_FUNCTION_ARGS)
{
  if(PG_ARGISNULL(0))
    PG_RETURN_BOOL(true);   // NULL token ≡ the neutral 1 ≠ 0

  try {
    pg_uuid_t token = *DatumGetUUIDP(PG_GETARG_DATUM(0));
    GenericCircuit gc = getGenericCircuit(token);
    gate_t root = gc.getGate(uuid2string(token));

    // The Boolean abstraction is built only to decide comparison gates'
    // world-set emptiness; on circuits it cannot express (an undecided
    // probabilistic comparison, a mixture…) the comparison gates simply
    // stay opaque and the proof works around them.
    bool has_cmp = false;
    for(gate_t i{0}; i < gc.getNbGates() && !has_cmp; ++i)
      has_cmp = (gc.getGateType(i) == gate_cmp);

    bool have_bc = false;
    gate_t broot;
    std::unordered_map<gate_t, gate_t> gc_to_bc;
    BooleanCircuit bc;
    if(has_cmp) {
      try {
        bc = getBooleanCircuit(gc, token, broot, gc_to_bc);
        have_bc = true;
      } catch(const std::exception &) {
        // fall through with opaque cmp gates
      }
    }

    std::unordered_map<gate_t, bool> memo;
    bool zero = provably_zero(gc, root,
                              have_bc ? &bc : nullptr,
                              have_bc ? &gc_to_bc : nullptr,
                              memo);
    PG_RETURN_BOOL(!zero);
  } catch(const std::exception &e) {
    provsql_error("nonzero: %s", e.what());
  } catch(...) {
    provsql_error("nonzero: Unknown exception");
  }

  PG_RETURN_NULL();   // unreachable: provsql_error does not return
}
