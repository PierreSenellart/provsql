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

#include <unordered_set>

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

bool GenericCircuit::foldSemiringIdentities()
{
  GenericCircuit &gc = *this;
  const std::size_t n = gc.getNbGates();
  bool ever = false;        ///< Whether any phase mutated the circuit.

  /* Wrap the three phases in an outer fixpoint loop.  Phase 3's
   * substitutions can re-expose identity wires to a parent that
   * referenced the collapsed gate (a single-wire @c gate_times / plus
   * whose sole wire was itself collapsed, a @c gate_zero absorber…),
   * which is a fresh Phase-1 opportunity.  @c createGenericCircuit
   * loads gates in BFS-from-root order (parents before children), so a
   * single pass through Phase 1 would propagate identity-collapse in
   * the wrong direction anyway -- the outer loop handles both effects
   * with the same machinery.  Terminates after at most one iteration
   * per DAG level. */

  /* Cumulative @c gate -> final-target redirection accumulated across
   * iterations.  We do NOT mutate a collapsed gate to carry its
   * target's content (the naive content-copy approach): for a target
   * that is a shared @c gate_input that would duplicate the Bernoulli variable
   * into an independent copy under the wrong UUID, over-counting the
   * probability of every non-read-once circuit (a single shared edge
   * in a recursive reachability lineage, say).  Instead we rewire each
   * parent's wire straight to the target, leaving the collapsed gate
   * orphaned, and -- after the loop -- patch the UUID map so a gate
   * resolved by UUID (notably the caller-supplied root) follows the
   * collapse rather than landing on the orphan. */
  std::unordered_map<gate_t, gate_t> redirect;

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
     * semantics for @c Counting / @c Formula / etc.  Already-redirected
     * (orphaned) gates are skipped so the fixpoint terminates. */
    std::unordered_map<gate_t, gate_t> subst;
    for (std::size_t i = 0; i < n; ++i) {
      auto g = static_cast<gate_t>(i);
      if (redirect.count(g)) continue;
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
     * its bottom endpoint in one lookup, chasing both this iteration's
     * substitutions and earlier ones recorded in @c redirect. */
    for (auto &kv : subst) {
      gate_t cur = kv.second;
      while (subst.count(cur) || redirect.count(cur))
        cur = subst.count(cur) ? subst[cur] : redirect[cur];
      kv.second = cur;
    }

    /* Phase 3: rewire every gate's wires through @c subst so parents
     * point straight at the collapse target.  Shared leaves stay a
     * single gate (no content-copy, no UUID-aliased duplicate in the
     * @c inputs set), so probability evaluators see the correct number
     * of independent variables.  Collapsed gates are left orphaned. */
    if (!subst.empty()) {
      for (std::size_t i = 0; i < n; ++i) {
        auto &wires = gc.getWires(static_cast<gate_t>(i));
        for (gate_t &w : wires) {
          auto it = subst.find(w);
          if (it != subst.end()) {
            w = it->second;
            changed = true;
          }
        }
      }
      for (const auto &kv : subst) redirect[kv.first] = kv.second;
    }
    ever |= changed;
  }

  /* Patch the UUID map and the Boolean-assumption set so a gate
   * resolved by UUID (Circuit::getGate, used for the root and the
   * joint-circuit roots) follows the collapse instead of landing on an
   * orphaned wrapper, and so a non-Boolean semiring still refuses on a
   * circuit whose marked gate was collapsed away. */
  if (!redirect.empty()) {
    for (auto &kv : redirect) {
      gate_t cur = kv.second;
      while (redirect.count(cur)) cur = redirect[cur];
      kv.second = cur;
    }
    for (const auto &kv : redirect) {
      gate_t g = kv.first, target = kv.second;
      auto uit = id2uuid.find(g);
      if (uit != id2uuid.end()) uuid2id[uit->second] = target;
      if (boolean_assumed_gates.count(g)) boolean_assumed_gates.insert(target);
      if (absorptive_assumed_gates.count(g))
        absorptive_assumed_gates.insert(target);
    }
  }

  return ever;
}

bool GenericCircuit::applyFoldRuleSweep(bool boolean_level)
{
  GenericCircuit &gc = *this;
  bool fired = false;
  const std::size_t n = gc.getNbGates();
  for (std::size_t i = 0; i < n; ++i) {
    auto g = static_cast<gate_t>(i);
    auto t = gc.getGateType(g);
    if (t != gate_plus && t != gate_times) continue;

    /* Which class of rule fired on this gate decides the side-band
     * marker: the absorptive rules are sound in every absorptive
     * semiring (1 + a = 1, hence a + a = a and a + ab = a), while
     * times-idempotence and times-absorbs-plus genuinely need the
     * Boolean interpretation (both fail in tropical). */
    bool absorptive_rule_fired = false;
    bool boolean_rule_fired = false;

    /* Rule 1 : idempotence.  a + a = a is absorptive
     * (a + a*1 = a); a * a = a is Boolean-only (tropical: a + a = 2a).
     * Drop repeated child wires preserving order of first
     * occurrence. */
    if (t == gate_plus || boolean_level) {
      auto &wires = gc.getWires(g);
      std::unordered_set<gate_t> seen;
      std::vector<gate_t> deduped;
      deduped.reserve(wires.size());
      for (gate_t c : wires) {
        if (seen.insert(c).second) deduped.push_back(c);
      }
      if (deduped.size() != wires.size()) {
        wires = std::move(deduped);
        if (t == gate_plus)
          absorptive_rule_fired = true;
        else
          boolean_rule_fired = true;
      }
    }

    /* Rule 2 : plus-with-one absorber (the defining absorptive
     * identity 1 + a = 1).  Collapse
     * @c gate_plus(..., @c gate_one, ...) directly to @c gate_one,
     * preserving @p g's UUID. */
    if (t == gate_plus) {
      bool has_one = false;
      for (gate_t c : gc.getWires(g)) {
        if (gc.getGateType(c) == gate_one) { has_one = true; break; }
      }
      if (has_one) {
        gc.setGateType(g, gate_one);
        gc.setWires(g, std::vector<gate_t>{});
        infos.erase(g);
        extra.erase(g);
        absorptive_rule_fired = true;
      }
    }

    /* Rule 3 : absorption.
     *   gate_plus (x, gate_times(x, y, ...), ...)  ->  gate_plus (x, ...)
     *     -- the defining absorptive identity a + ab = a;
     *   gate_times(x, gate_plus (x, y, ...), ...)  ->  gate_times(x, ...)
     *     -- the lattice dual, Boolean-only (tropical:
     *        x + min(x, y) != x in general).
     * The absorbed times / plus child is dominated by its sibling
     * @c x present in the parent's children set.  Implemented as a
     * single pass : build a set view of the parent's children, then
     * drop every opposite-type child whose wires intersect that set.
     * The set view captures the wires snapshot ; in case B2 already
     * mutated @p g into @c gate_one above, @c t still holds the
     * pre-mutation type so absorption skips correctly via the
     * type-mismatch check at the top.  The dominating literal @c x is
     * often only exposed as a direct sibling after a single-wire
     * collapse (a diagonal @c gate_times(x, x) deduped by B1 then
     * rewritten to @c x by @c foldSemiringIdentities) : the joint
     * fixpoint in @c foldBooleanIdentities re-runs this sweep after
     * each collapse so the exposed literal is seen on the next pass. */
    if (t == gate_plus || boolean_level) {
      const gate_type opposite = (t == gate_plus) ? gate_times : gate_plus;
      const auto &wires_now = gc.getWires(g);
      if (wires_now.size() >= 2) {
        std::unordered_set<gate_t> sibling_set(wires_now.begin(),
                                               wires_now.end());
        std::vector<gate_t> kept;
        kept.reserve(wires_now.size());
        bool dropped = false;
        for (gate_t c : wires_now) {
          if (gc.getGateType(c) == opposite) {
            bool absorb = false;
            for (gate_t w : gc.getWires(c)) {
              if (w != c && sibling_set.count(w)) {
                absorb = true;
                break;
              }
            }
            if (absorb) { dropped = true; continue; }
          }
          kept.push_back(c);
        }
        if (dropped) {
          gc.setWires(g, std::move(kept));
          if (t == gate_plus)
            absorptive_rule_fired = true;
          else
            boolean_rule_fired = true;
        }
      }
    }

    if (absorptive_rule_fired)
      markAbsorptiveAssumed(g);
    if (boolean_rule_fired)
      markBooleanAssumed(g);
    if (absorptive_rule_fired || boolean_rule_fired)
      fired = true;
  }
  return fired;
}

void GenericCircuit::foldBooleanIdentities()
{
  /* Interleave the Boolean rule sweep with the semiring-safe collapse
   * to a JOINT fixpoint.  Running the Boolean rules to their own
   * fixpoint and only THEN collapsing once would miss absorptions
   * whose dominating literal is exposed by a collapse: a diagonal
   * @c gate_times(x, x) is deduped to a single-wire @c gate_times by
   * B1, but only @c foldSemiringIdentities rewrites that wrapper to
   * @c x, so a later B3 pass would never see @c x as a sibling.  Alternating until neither pass changes anything closes
   * that gap and leaves a circuit on which no rule applies.
   *
   * Termination: every rule (idempotence, plus-with-one, absorption,
   * identity-drop, single-wire / empty collapse, zero-absorber) strictly
   * removes at least one wire or gate and none ever adds one, so the
   * well-founded (gates, wires) measure decreases; the loop exits only
   * when both passes report no change, i.e. no rule applies to any gate.
   * Collapses propagate one DAG level per iteration, so the sweep count
   * is O(circuit depth) and each sweep is linear in the circuit size. */
  bool changed = true;
  while (changed) {
    changed = applyFoldRuleSweep(true);
    changed |= foldSemiringIdentities();
  }
}

void GenericCircuit::foldAbsorptiveIdentities()
{
  /* Same joint fixpoint as foldBooleanIdentities, restricted to the
   * rules sound in every absorptive semiring; see the header note. */
  bool changed = true;
  while (changed) {
    changed = applyFoldRuleSweep(false);
    changed |= foldSemiringIdentities();
  }
}
