/**
 * @file fix_system.cpp
 * @brief Resolution of the equation system of a recursive query.
 *
 * @c eval_recursive_system records a recursion as one equation per derived
 * tuple, x_i = f_i(x), gathered in a @c gate_fixsystem.  Only the tuples
 * derived through themselves need an unknown: a tuple outside every cycle
 * of the dependency graph has a value that its right-hand side gives once
 * the tuples it reads have theirs.  @c resolve_fix_system walks the
 * strongly connected components of that graph in solving order and gives
 * each tuple its final token:
 *
 * - a tuple outside every cycle gets its right-hand side rebuilt with the
 *   unknowns replaced by the tokens of the tuples they stand for, by the
 *   rewriter's own gate builders: the gates, and their content addresses,
 *   are those a fixpoint iteration of the recursion would reach;
 * - the tuples of a cyclic component get a @c gate_fixsystem of their own,
 *   whose right-hand sides read the tokens already resolved, and each a
 *   @c gate_fixpoint over it, solved at evaluation time.
 *
 * A right-hand side with a gate the builders cannot rebuild leaves the
 * system as it was: one @c gate_fixpoint per tuple over the whole system,
 * which evaluation solves all the same.
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/uuid.h"
#include "provsql_utils.h"
#include "provsql_mmap.h"
#include "gate_builders.h"

PG_FUNCTION_INFO_V1(resolve_fix_system);
}

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "CertifiedDDMaterialize.h"
#include "CircuitFromMMap.h"
#include "FixSystemGraph.h"
#include "GenericCircuit.h"
#include "provsql_interrupt.h"
#include "provsql_utils_cpp.h"

namespace {

/** A gate the rewriter's builders cannot rebuild. */
struct NotRebuildable {};

/** Address of the @c gate_fixpoint reading component @p i (1-based) of
 *  system @p sys; the same as the SQL @c fixpoint_token. */
pg_uuid_t fixpoint_address(const pg_uuid_t &sys, std::size_t i)
{
  return provsqlUuidV5("fixpoint:" + uuid2string(sys) + ":" +
                       std::to_string(i));
}

/** Create a system over @p unknowns and @p rhs, and its fixpoint gates;
 *  @return the address of each fixpoint gate, in order. */
std::vector<pg_uuid_t> make_system(const std::vector<pg_uuid_t> &unknowns,
                                   const std::vector<pg_uuid_t> &rhs)
{
  std::vector<pg_uuid_t> wires(unknowns);
  wires.insert(wires.end(), rhs.begin(), rhs.end());
  std::string name = "fixsystem{";
  for(std::size_t k = 0; k < wires.size(); ++k)
    name += (k ? "," : "") + uuid2string(wires[k]);
  name += "}";
  const pg_uuid_t sys = provsqlUuidV5(name);
  provsql_internal_create_gate(&sys, gate_fixsystem, wires.size(),
                               wires.data());

  std::vector<pg_uuid_t> result;
  for(std::size_t k = 0; k < unknowns.size(); ++k) {
    const pg_uuid_t p = fixpoint_address(sys, k + 1);
    provsql_internal_create_gate_with(&p, gate_fixpoint, 1, &sys, true,
                                      k + 1, 0, nullptr);
    result.push_back(p);
  }
  return result;
}

std::vector<pg_uuid_t> resolve(pg_uuid_t sys_token)
{
  /* The circuit as stored: the load-time simplifications would rewrite the
   * right-hand sides, which are to be rebuilt as the rewriter built them. */
  const bool s1 = provsql_simplify_on_load;
  const bool s2 = provsql_boolean_provenance;
  const bool s3 = provsql_absorptive_provenance;
  provsql_simplify_on_load = false;
  provsql_boolean_provenance = false;
  provsql_absorptive_provenance = false;
  GenericCircuit c;
  try {
    c = getGenericCircuit(sys_token);
  } catch(...) {
    provsql_simplify_on_load = s1;
    provsql_boolean_provenance = s2;
    provsql_absorptive_provenance = s3;
    throw;
  }
  provsql_simplify_on_load = s1;
  provsql_boolean_provenance = s2;
  provsql_absorptive_provenance = s3;

  const gate_t sys = c.getGate(uuid2string(sys_token));
  const auto &w = c.getWires(sys);
  const std::size_t n = w.size() / 2;

  std::unordered_map<gate_t, std::size_t> unknown;
  for(std::size_t i = 0; i < n; ++i)
    unknown.emplace(w[i], i);

  /* For each gate depending on an unknown, the unknowns it reads (an
   * iterative post-order: the sub-circuits that read none can be as deep
   * as the data). */
  std::unordered_map<gate_t, std::vector<std::size_t> > reads;
  std::unordered_map<gate_t, bool> dep;
  {
    std::vector<gate_t> st(w.begin() + n, w.end());
    while(!st.empty()) {
      const gate_t u = st.back();
      if(dep.count(u)) { st.pop_back(); continue; }
      auto it = unknown.find(u);
      if(it != unknown.end()) {
        dep.emplace(u, true);
        reads[u] = {it->second};
        st.pop_back();
        continue;
      }
      bool ready = true;
      for(const auto &ch : c.getWires(u))
        if(!dep.count(ch)) { st.push_back(ch); ready = false; }
      if(!ready) continue;
      std::vector<std::size_t> r;
      for(const auto &ch : c.getWires(u))
        if(dep.at(ch)) {
          const auto &rc = reads.at(ch);
          r.insert(r.end(), rc.begin(), rc.end());
        }
      std::sort(r.begin(), r.end());
      r.erase(std::unique(r.begin(), r.end()), r.end());
      dep.emplace(u, !r.empty());
      if(!r.empty())
        reads.emplace(u, std::move(r));
      st.pop_back();
    }
  }

  std::vector<std::vector<std::size_t> > out(n);
  std::vector<bool> self_loop(n, false);
  for(std::size_t i = 0; i < n; ++i)
    if(dep.at(w[n + i]))
      for(const auto j : reads.at(w[n + i])) {
        out[j].push_back(i);
        if(j == i)
          self_loop[i] = true;
      }
  const auto comps = fixSystemComponents(out);

  std::vector<pg_uuid_t> token(n);
  std::vector<bool> resolved(n, false);

  /* The right-hand side @p g rebuilt with the resolved unknowns replaced by
   * their tokens; the others, those of the component being resolved, are
   * kept.  Memoised per component, the replacement being fixed there. */
  std::unordered_map<gate_t, pg_uuid_t> memo;
  std::function<pg_uuid_t(gate_t)> rebuild = [&](gate_t g) -> pg_uuid_t {
    if(!dep.at(g))
      return string2uuid(c.getUUID(g));
    auto m = memo.find(g);
    if(m != memo.end())
      return m->second;
    pg_uuid_t r;
    auto it = unknown.find(g);
    if(it != unknown.end())
      r = resolved[it->second] ? token[it->second] : string2uuid(c.getUUID(g));
    else {
      std::vector<pg_uuid_t> ch;
      for(const auto &x : c.getWires(g))
        ch.push_back(rebuild(x));
      switch(c.getGateType(g)) {
      case gate_plus:
        r = provsql_build_plus(ch.data(), ch.size());
        break;
      case gate_times:
        r = provsql_build_times(ch.data(), ch.size());
        break;
      case gate_monus:
        if(ch.size() != 2)
          throw NotRebuildable();
        r = provsql_build_monus(&ch[0], &ch[1]);
        break;
      case gate_delta:
        if(ch.size() != 1)
          throw NotRebuildable();
        r = provsql_build_delta(&ch[0]);
        break;
      default:
        throw NotRebuildable();
      }
    }
    memo.emplace(g, r);
    return r;
  };

  try {
    for(const auto &comp : comps) {
      provsql_poll_interrupt();
      memo.clear();
      if(comp.size() == 1 && !self_loop[comp[0]]) {
        const std::size_t i = comp[0];
        token[i] = rebuild(w[n + i]);
        resolved[i] = true;
        continue;
      }
      std::vector<pg_uuid_t> unknowns, rhs;
      for(const auto i : comp) {
        unknowns.push_back(string2uuid(c.getUUID(w[i])));
        rhs.push_back(rebuild(w[n + i]));
      }
      const auto fix = make_system(unknowns, rhs);
      for(std::size_t k = 0; k < comp.size(); ++k) {
        token[comp[k]] = fix[k];
        resolved[comp[k]] = true;
      }
    }
  } catch(const NotRebuildable &) {
    /* Keep the whole system: one fixpoint gate per tuple over it. */
    for(std::size_t i = 0; i < n; ++i) {
      token[i] = fixpoint_address(sys_token, i + 1);
      provsql_internal_create_gate_with(&token[i], gate_fixpoint, 1,
                                        &sys_token, true, i + 1, 0, nullptr);
    }
  }
  return token;
}

}

/**
 * @brief The token of each tuple of the equation system @p sys, in order.
 *
 * SQL: @c resolve_fix_system(sys uuid) @c RETURNS @c uuid[].  See the file
 * documentation.
 */
Datum resolve_fix_system(PG_FUNCTION_ARGS)
{
  try {
    provsql_interrupt_scope interrupt_scope;
    const pg_uuid_t sys = *DatumGetUUIDP(PG_GETARG_DATUM(0));
    const std::vector<pg_uuid_t> tokens = resolve(sys);

    Datum *elems = (Datum *) palloc(sizeof(Datum) * (tokens.size() ? tokens.size() : 1));
    for(std::size_t i = 0; i < tokens.size(); ++i) {
      pg_uuid_t *u = (pg_uuid_t *) palloc(sizeof(pg_uuid_t));
      *u = tokens[i];
      elems[i] = UUIDPGetDatum(u);
    }
    PG_RETURN_ARRAYTYPE_P(construct_array(elems, tokens.size(), UUIDOID,
                                          UUID_LEN, false, 'c'));
  } catch(const std::exception &e) {
    provsql_cancel_if_interrupted();
    provsql_error("resolve_fix_system: %s", e.what());
  } catch(...) {
    provsql_cancel_if_interrupted();
    provsql_error("resolve_fix_system: Unknown exception");
  }
  PG_RETURN_NULL();
}
