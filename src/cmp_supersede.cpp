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
 *
 * The walk needs the type and the children of a few gates, all of which this
 * backend created moments earlier in the same query (the comparison, the
 * aggregates under it, their semimod wires, the row tokens and their δ and
 * ⊕).  They are read one by one through @c provsql_fetch_gate, which answers
 * from the per-session cache and asks the worker only on a miss; loading the
 * subcircuit under all these roots at once, as was done before, cost a
 * synchronous round trip and a serialisation per group.
 */
extern "C"
{
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/uuid.h"
#include "provsql_utils.h"
#include "provsql_mmap.h"
}

#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C"
{
PG_FUNCTION_INFO_V1(cmp_surviving_factors);
}

namespace {

struct UuidHash {
  std::size_t operator()(const pg_uuid_t &u) const {
    std::size_t h;
    std::memcpy(&h, u.data, sizeof(h));
    return h;
  }
};
struct UuidEq {
  bool operator()(const pg_uuid_t &a, const pg_uuid_t &b) const {
    return std::memcmp(a.data, b.data, UUID_LEN) == 0;
  }
};
using UuidSet = std::unordered_set<pg_uuid_t, UuidHash, UuidEq>;

/** @brief A gate as the walk sees it: its type and its children, fetched
 *         once per call. */
struct Gate {
  gate_type type;
  std::vector<pg_uuid_t> wires;
};

class Gates {
public:
  const Gate &operator[](const pg_uuid_t &u) {
    auto it = memo.find(u);
    if(it != memo.end())
      return it->second;
    Gate g;
    unsigned n = 0;
    pg_uuid_t *children = nullptr;
    g.type = provsql_fetch_gate(&u, &n, &children);
    if(children) {
      g.wires.assign(children, children + n);
      free(children);
    }
    return memo.emplace(u, std::move(g)).first->second;
  }
private:
  std::unordered_map<pg_uuid_t, Gate, UuidHash, UuidEq> memo;
};

/** @brief Collect the provenance children of every @c gate_semimod under the
 *         @c gate_agg gates reachable from @p g (directly or under
 *         @c gate_arith): the group the comparison ranges over. */
void collect_group_tokens(Gates &gates, const pg_uuid_t &g,
                          UuidSet &out, UuidSet &seen)
{
  if(!seen.insert(g).second)
    return;

  const Gate &gate = gates[g];

  if(gate.type == gate_agg) {
    for(const pg_uuid_t &ch : gate.wires) {
      const Gate &sm = gates[ch];
      if(sm.type != gate_semimod)
        continue;
      if(sm.wires.size() == 2)
        out.insert(sm.wires[0]);    // [k_gate, value_gate]
    }
    return;
  }

  /* A HAVING with Boolean connectives lifts to a product / sum / difference
   * of comparison gates, so the groups being compared sit under that
   * structure, not directly under a single cmp. */
  const gate_type t = gate.type;
  if(t == gate_arith || t == gate_cmp || t == gate_times ||
     t == gate_plus || t == gate_monus || t == gate_delta) {
    const std::vector<pg_uuid_t> wires = gate.wires;   // the memo may grow
    for(const pg_uuid_t &ch : wires)
      collect_group_tokens(gates, ch, out, seen);
  }
}

/** @brief Whether @p g is a δ collapsing exactly the group @p group. */
bool delta_subsumed_by(Gates &gates, const pg_uuid_t &g, const UuidSet &group)
{
  const Gate &d = gates[g];
  if(d.type != gate_delta || group.empty())
    return false;
  if(d.wires.size() != 1)
    return false;

  // The δ wraps the group's ⊕; a one-row group may carry that row's token
  // directly, with no ⊕ to wrap.
  const pg_uuid_t child = d.wires[0];
  std::vector<pg_uuid_t> operands;
  const Gate &c = gates[child];
  if(c.type == gate_plus)
    operands = c.wires;
  else
    operands.push_back(child);

  if(operands.size() != group.size())
    return false;
  for(const pg_uuid_t &o : operands)
    if(group.find(o) == group.end())
      return false;
  return true;
}

/** @brief Append the factors of @p g that survive the comparison.
 *
 * A ⊗ is flattened so a δ nested inside it can be dropped on its own; a
 * subsumed δ contributes nothing; anything else stands as one factor. */
void surviving_factors(Gates &gates, const pg_uuid_t &g, const UuidSet &group,
                       std::vector<pg_uuid_t> &out)
{
  if(delta_subsumed_by(gates, g, group))
    return;

  if(gates[g].type == gate_times) {
    const std::vector<pg_uuid_t> wires = gates[g].wires;
    for(const pg_uuid_t &ch : wires)
      surviving_factors(gates, ch, group, out);
    return;
  }

  out.push_back(g);
}

} // namespace

/**
 * @brief @c cmp_surviving_factors(tokens uuid[], cmp uuid) -> uuid[]
 *
 * @param tokens  The row-annotation factors at the level owning the comparison.
 * @param cmp     The lifted comparison gate.
 * @return        The factors of @p tokens the comparison does not subsume,
 *                flattened; NULL on a NULL argument.
 */
Datum cmp_surviving_factors(PG_FUNCTION_ARGS)
{
  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    PG_RETURN_NULL();

  try {
    ArrayType *arr = PG_GETARG_ARRAYTYPE_P(0);
    const pg_uuid_t cmp = *DatumGetUUIDP(PG_GETARG_DATUM(1));
    Datum *elems;
    bool *nulls;
    int nelems;

    if(ARR_NDIM(arr) > 1)
      provsql_error("cmp_surviving_factors: tokens must be a 1-D array");

    deconstruct_array(arr, UUIDOID, 16, false, 'c', &elems, &nulls, &nelems);

    Gates gates;
    UuidSet group, seen;
    collect_group_tokens(gates, cmp, group, seen);

    std::vector<Datum> kept;
    UuidSet emitted;

    for(int i = 0; i < nelems; ++i) {
      if(nulls[i])
        continue;
      std::vector<pg_uuid_t> factors;
      surviving_factors(gates, *DatumGetUUIDP(elems[i]), group, factors);
      for(const pg_uuid_t &f : factors) {
        if(!emitted.insert(f).second)
          continue;                 // one copy of a factor shared by two inputs
        pg_uuid_t *p = (pg_uuid_t *) palloc(sizeof(pg_uuid_t));
        *p = f;
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
