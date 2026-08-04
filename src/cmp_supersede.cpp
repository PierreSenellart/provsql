/**
 * @file cmp_supersede.cpp
 * @brief SQL function @c provsql.cmp_surviving_factors() – the factors of a
 *        row annotation that an aggregate comparison does *not* subsume.
 *
 * When a comparison on an aggregate is lifted into the provenance circuit, its
 * @c gate_cmp already entails that the compared group exists: the enumeration
 * behind it ranges over the non-empty worlds of the very same per-row tokens.
 * So the comparison supersedes the group's @c gate_delta rather than
 * multiplying with it -- conjoining both would count group existence twice in
 * a non-idempotent semiring.
 *
 * What it supersedes is precisely that δ, though, and not whatever else the
 * row annotation happens to carry.  A row token reaching the level that owns
 * the comparison may be
 *   - the bare δ (the plain @c gamma then sigma shape),
 *   - a ⊗ mixing the δ with other factors (a view or CTE holding gamma joined
 *     with another relation), whose other factors must survive,
 *   - or something else entirely -- an earlier comparison's @c gate_cmp on the
 *     same group (sigma after sigma), an input -- which the new comparison
 *     does not subsume at all and which must be kept and multiplied.
 * Dropping the whole annotation is right only in the first case; this walk
 * distinguishes them structurally.
 *
 * A δ is subsumed when its ⊕ child's operands are exactly the provenance
 * children of the compared aggregate's @c gate_semimod wires -- that is, when
 * it collapses the multiplicity of the very group the comparison ranges over.
 *
 * The function is read-only: it returns the surviving factors flattened, and
 * the caller rebuilds the product with @c provenance_times, so no gate is
 * minted here.
 */
extern "C"
{
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/uuid.h"
#include "provsql_utils.h"
}

#include <exception>
#include <string>
#include <unordered_set>
#include <vector>

#include "CircuitFromMMap.h"
#include "GenericCircuit.h"
#include "provsql_utils_cpp.h"

extern "C"
{
PG_FUNCTION_INFO_V1(cmp_surviving_factors);
}

namespace {

/** @brief Collect the provenance children of every @c gate_semimod under the
 *         @c gate_agg gates reachable from @p g (directly or under
 *         @c gate_arith): the group the comparison ranges over. */
void collect_group_tokens(const GenericCircuit &gc, gate_t g,
                          std::unordered_set<gate_t> &out,
                          std::unordered_set<gate_t> &seen)
{
  if(!seen.insert(g).second)
    return;

  const gate_type t = gc.getGateType(g);

  if(t == gate_agg) {
    for(gate_t ch : gc.getWires(g)) {
      if(gc.getGateType(ch) != gate_semimod)
        continue;
      const auto &sm = gc.getWires(ch);
      if(sm.size() == 2)
        out.insert(sm[0]);          // [k_gate, value_gate]
    }
    return;
  }

  /* A HAVING with Boolean connectives lifts to a product / sum / difference
   * of comparison gates, so the groups being compared sit under that
   * structure, not directly under a single cmp. */
  if(t == gate_arith || t == gate_cmp || t == gate_times ||
     t == gate_plus || t == gate_monus || t == gate_delta)
    for(gate_t ch : gc.getWires(g))
      collect_group_tokens(gc, ch, out, seen);
}

/** @brief Whether @p g is a δ collapsing exactly the group @p group. */
bool delta_subsumed_by(const GenericCircuit &gc, gate_t g,
                       const std::unordered_set<gate_t> &group)
{
  if(gc.getGateType(g) != gate_delta || group.empty())
    return false;

  const auto &dw = gc.getWires(g);
  if(dw.size() != 1)
    return false;

  // The δ wraps the group's ⊕; a one-row group may carry that row's token
  // directly, with no ⊕ to wrap.
  std::vector<gate_t> operands;
  if(gc.getGateType(dw[0]) == gate_plus) {
    const auto &pw = gc.getWires(dw[0]);
    operands.assign(pw.begin(), pw.end());
  } else {
    operands.push_back(dw[0]);
  }

  if(operands.size() != group.size())
    return false;
  for(gate_t o : operands)
    if(group.find(o) == group.end())
      return false;
  return true;
}

/** @brief Append the factors of @p g that survive the comparison.
 *
 * A ⊗ is flattened so a δ nested inside it can be dropped on its own; a
 * subsumed δ contributes nothing; anything else stands as one factor. */
void surviving_factors(const GenericCircuit &gc, gate_t g,
                       const std::unordered_set<gate_t> &group,
                       std::vector<gate_t> &out)
{
  if(delta_subsumed_by(gc, g, group))
    return;

  if(gc.getGateType(g) == gate_times) {
    for(gate_t ch : gc.getWires(g))
      surviving_factors(gc, ch, group, out);
    return;
  }

  out.push_back(g);
}

} // namespace

/**
 * @brief @c cmp_surviving_factors(tokens uuid[], cmp uuid) -> uuid[]
 *
 * Given the row-annotation factors at the level owning a lifted comparison and
 * that comparison's gate, returns the factors the comparison does not subsume,
 * for the caller to multiply with it.  NULL entries are dropped.
 */
Datum cmp_surviving_factors(PG_FUNCTION_ARGS)
{
  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    PG_RETURN_NULL();

  try {
    ArrayType *arr = PG_GETARG_ARRAYTYPE_P(0);
    pg_uuid_t cmp = *DatumGetUUIDP(PG_GETARG_DATUM(1));
    Datum *elems;
    bool *nulls;
    int nelems;

    if(ARR_NDIM(arr) > 1)
      provsql_error("cmp_surviving_factors: tokens must be a 1-D array");

    deconstruct_array(arr, UUIDOID, 16, false, 'c', &elems, &nulls, &nelems);

    /* The row tokens are siblings of the comparison, not its descendants, so
     * they must be loaded into one circuit with it: only then do a delta and
     * the aggregate's semimod wires resolve to the same gate_t and become
     * comparable. */
    std::vector<pg_uuid_t> roots;
    roots.push_back(cmp);
    for(int i = 0; i < nelems; ++i) {
      if(nulls[i])
        continue;
      roots.push_back(*DatumGetUUIDP(elems[i]));
    }

    std::vector<gate_t> gates;
    GenericCircuit gc = getJointCircuit(roots, gates);

    std::unordered_set<gate_t> group, seen;
    collect_group_tokens(gc, gates[0], group, seen);

    std::vector<Datum> kept;
    std::unordered_set<gate_t> emitted;

    for(std::size_t r = 1; r < gates.size(); ++r) {
      std::vector<gate_t> factors;
      surviving_factors(gc, gates[r], group, factors);
      for(gate_t f : factors) {
        if(!emitted.insert(f).second)
          continue;                 // one copy of a factor shared by two inputs
        pg_uuid_t out = string2uuid(gc.getUUID(f));
        pg_uuid_t *p = (pg_uuid_t *) palloc(sizeof(pg_uuid_t));
        *p = out;
        kept.push_back(UUIDPGetDatum(p));
      }
    }

    {
      ArrayType *res = construct_array(kept.data(), (int) kept.size(),
                                       UUIDOID, 16, false, 'c');
      PG_RETURN_ARRAYTYPE_P(res);
    }
  } catch(const std::exception &e) {
    provsql_error("cmp_surviving_factors: %s", e.what());
  } catch(...) {
    provsql_error("cmp_surviving_factors: Unknown exception");
  }

  PG_RETURN_NULL();   // unreachable: provsql_error does not return
}
