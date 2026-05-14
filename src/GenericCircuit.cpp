/**
 * @file GenericCircuit.cpp
 * @brief GenericCircuit method implementations.
 *
 * Implements the virtual methods of @c GenericCircuit that override the
 * @c Circuit<gate_type> base class:
 * - @c addGate(): allocates a new gate and extends the @c prob vector.
 * - @c setGate(gate_type): creates a new gate, registering it as an
 *   input gate when the type is @c gate_input or @c gate_update.
 * - @c setGate(const uuid&, gate_type): same with UUID binding.
 *
 * The template method @c evaluate() is defined in @c GenericCircuit.hpp.
 */
#include "GenericCircuit.h"

gate_t GenericCircuit::setGate(gate_type type)
{
  auto id = Circuit::setGate(type);
  if(type == gate_input || type==gate_update) {
    inputs.insert(id);
  }
  return id;
}

gate_t GenericCircuit::setGate(const uuid &u, gate_type type)
{
  auto id = Circuit::setGate(u, type);
  if(type == gate_input || type==gate_update) {
    inputs.insert(id);
  }
  return id;
}

gate_t GenericCircuit::addGate()
{
  auto id=Circuit::addGate();
  prob.push_back(1);
  return id;
}

#include <unordered_map>

void GenericCircuit::foldSemiringIdentities()
{
  GenericCircuit &gc = *this;
  const std::size_t n = gc.getNbGates();

  /* Wrap the three phases in an outer fixpoint loop.  Phase 3's
   * substitutions can mutate a gate into @c gate_zero / @c gate_one
   * (multiplicative absorber, single-wire collapse), which is a fresh
   * identity-wire opportunity for any parent that referenced the
   * mutated gate.  @c createGenericCircuit loads gates in BFS-from-
   * root order (parents before children), so a single pass through
   * Phase 1 would propagate identity-collapse in the wrong direction
   * anyway -- the outer loop handles both effects with the same
   * machinery.  Terminates after at most one iteration per DAG
   * level. */
  bool changed = true;
  while (changed) {
    changed = false;

    /* Phase 1: drop identity wires in place.  Multiplicative identity
     * is @c gate_one (drop from @c gate_times); additive identity is
     * @c gate_zero (drop from @c gate_plus).  If every wire of a
     * @c gate_plus / @c gate_times was the identity, the gate is left
     * with an empty wire list: collapse it to the identity (empty sum
     * is @c gate_zero, empty product is @c gate_one) by mutating the
     * gate type in place. */
    for (std::size_t i = 0; i < n; ++i) {
      auto g = static_cast<gate_t>(i);
      auto t = gc.getGateType(g);
      if (t != gate_times && t != gate_plus) continue;
      gate_type identity = (t == gate_times) ? gate_one : gate_zero;
      auto &wires = gc.getWires(g);
      std::vector<gate_t> kept;
      kept.reserve(wires.size());
      for (gate_t c : wires) {
        if (gc.getGateType(c) != identity) kept.push_back(c);
      }
      if (kept.size() != wires.size()) {
        wires = std::move(kept);
        changed = true;
      }
      if (wires.empty()) {
        gc.setGateType(g, identity);
        changed = true;
      }
    }

    /* Phase 2: build a substitution map for gates that should be
     * collapsed to a single descendant after phase 1:
     *   - @c gate_times that still has a @c gate_zero wire ->
     *     substitute to that absorber (multiplicative zero is the
     *     universal absorber across semirings);
     *   - any @c gate_times / @c gate_plus left with exactly one wire
     *     -> substitute to that wire (identity collapse).
     * The plus-with-one absorber rewrite is intentionally omitted:
     * @c gate_one is the additive absorber only in idempotent
     * semirings (Boolean, MinMax) and would silently change the
     * semantics for @c Counting / @c Formula / etc. */
    std::unordered_map<gate_t, gate_t> subst;
    for (std::size_t i = 0; i < n; ++i) {
      auto g = static_cast<gate_t>(i);
      auto t = gc.getGateType(g);
      if (t != gate_times && t != gate_plus) continue;
      const auto &wires = gc.getWires(g);
      if (t == gate_times) {
        bool absorbed = false;
        for (gate_t c : wires) {
          if (gc.getGateType(c) == gate_zero) {
            subst[g] = c;
            absorbed = true;
            break;
          }
        }
        if (absorbed) continue;
      }
      if (wires.size() == 1) subst[g] = wires[0];
    }
    /* Resolve transitively so a chain of singleton wrappers maps to
     * its bottom endpoint in one lookup. */
    for (auto &kv : subst) {
      gate_t cur = kv.second;
      while (subst.count(cur)) cur = subst[cur];
      kv.second = cur;
    }

    /* Phase 3: mutate every substituted gate to carry its target's
     * type / wires / info / extra.  In-place transformation keeps the
     * original gate's UUID (so downstream consumers that key on the
     * caller-supplied root UUID still find it) while every walk from
     * here sees the target's content.  Mutually-substituted gates all
     * resolve to the same endpoint (phase 2 above), so the order in
     * which we apply them within the loop doesn't matter: copying
     * from the endpoint always yields the correct content. */
    for (const auto &kv : subst) {
      gate_t g = kv.first;
      gate_t target = kv.second;
      if (g == target) continue;
      gc.setGateType(g, gc.getGateType(target));
      gc.getWires(g) = gc.getWires(target);
      auto [ti1, ti2] = gc.getInfos(target);
      const unsigned NO_INFO = static_cast<unsigned>(-1);
      if (ti1 != NO_INFO || ti2 != NO_INFO) {
        gc.setInfos(g, ti1, ti2);
      }
      try {
        const std::string e = gc.getExtra(target);
        gc.setExtra(g, e);
      } catch (const CircuitException &) {
        /* target has no extra; leave g's extra as-is (it was already
         * cleared if it was an internal gate; values_t entry persisted
         * for value gates we don't reach here). */
      }
      changed = true;
    }
  }
}
