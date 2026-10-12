/**
 * @file GenericCircuit.hpp
 * @brief Template implementation of @c GenericCircuit::evaluate().
 *
 * Provides the out-of-line definition of the @c evaluate() template method
 * declared in @c GenericCircuit.h.  This file must be included (directly
 * or transitively) by any translation unit that instantiates
 * @c GenericCircuit::evaluate<S>() for a specific semiring type @c S.
 *
 * The @c evaluate() method performs a post-order traversal of the sub-circuit
 * rooted at gate @p g, looking up input-gate values from @p provenance_mapping
 * and combining them using the semiring operations:
 *
 * | Gate type   | Semiring operation             |
 * |-------------|-------------------------------|
 * | gate_input  | lookup in @p provenance_mapping (else @c unmapped_input) |
 * | gate_plus   | @c semiring.plus(children)     |
 * | gate_times  | @c semiring.times(children)    |
 * | gate_monus  | @c semiring.monus(left, right) |
 * | gate_delta  | @c semiring.delta(child)       |
 * | gate_cmp    | @c semiring.cmp(left, op, right)|
 * | gate_semimod| @c semiring.semimod(x, s)      |
 * | gate_agg    | @c semiring.agg(op, children)  |
 * | gate_value  | @c semiring.value(string)      |
 * | gate_one    | @c semiring.one()              |
 * | gate_zero   | @c semiring.zero()             |
 * | gate_rv     | @c semiring.rv(spec, params)   |
 * | gate_arith  | @c semiring.arith(op, children, extra) |
 * | gate_mixture| @c semiring.mixture(p, x, y) / @c semiring.categorical(…) |
 * | gate_case   | @c semiring.guarded_case(children) |
 * | gate_observe| @c semiring.observe(child, datum) |
 * | gate_conditioned | @c semiring.conditioned(children) |
 *
 * The last six are measure-carrier gates with no algebraic reading: the
 * @c Semiring base class refuses them, and only the symbolic @c Formula
 * pseudo-semiring overrides the hooks (to render them rather than
 * interpret them).
 */
#include "GenericCircuit.h"
#include "FixSystemGraph.h"

#include <deque>
#include <map>
#include <set>
#include <functional>
#include <queue>

extern "C" {
#include "utils/lsyscache.h"
#include "miscadmin.h"        // check_stack_depth
#include "provsql_interrupt.h"
}

template<typename S, std::enable_if_t<std::is_base_of_v<semiring::Semiring<typename S::value_type>, S>, int> >
typename S::value_type GenericCircuit::evaluate(gate_t g, std::unordered_map<gate_t, typename S::value_type> &provenance_mapping, S semiring) const
{
  /* Iterative post-order evaluation with @p provenance_mapping doubling as
   * the memoisation table.  Provenance circuits can be as deep as the data
   * (a recursive fixpoint's times/plus chain, the decomposition-aligned
   * reachability circuits of path-like graphs), so recursion on wires
   * would overflow the C stack -- the previous implementation turned that
   * into a "stack depth limit exceeded" error at a few thousand levels;
   * the explicit stack removes the ceiling altogether.  Every computed
   * gate is memoised (a gate's semiring value is a pure function of the
   * gate), so shared sub-DAGs are evaluated once and gate-creating
   * semirings (BoolExpr, formula) preserve the sharing structurally. */
  std::vector<gate_t> stack{g};
  /* Equation systems already solved in this evaluation, one value per
   * unknown: every gate_fixpoint over the same system reads from it. */
  std::unordered_map<gate_t, std::vector<typename S::value_type> > systems;

  while(!stack.empty()) {
    const gate_t u = stack.back();
    provsql_poll_interrupt();

    /* The side-band assumption checks run BEFORE the memoisation
     * lookup: input leaves are preloaded into @p provenance_mapping
     * from the mapping table, and a fold collapse can redirect a
     * marked gate onto such a leaf -- the marker must still refuse
     * incompatible semirings there. */

    /* In-memory Boolean-assumption marker (set by
     * @c foldBooleanIdentities on gates whose wires were rewritten
     * under a Boolean-only rule).  Mirrors the @c gate_assumed
     * structural-marker check below but applies to gates that keep
     * their original type (the rule mutated their wires in place ;
     * the persistent mmap was not touched).  Same compatibility
     * predicate, same failure mode. */
    if(isBooleanAssumed(u) && !semiring.compatibleWithBooleanRewrite())
      throw CircuitException(
              "The requested semiring does not admit a homomorphism "
              "from Boolean functions; this gate's wires were rewritten "
              "under a Boolean-only rule (times-idempotence or "
              "times-absorbs-plus, applied under the 'boolean' "
              "provenance class) and the evaluation is unsound under "
              "this semiring.  Re-run under a more general provenance "
              "class, or pick a Boolean-compatible semiring (boolean, "
              "boolexpr, formula, ...).");

    /* In-memory absorptive-assumption marker (set by the absorptive
     * fold rules: plus-idempotence, plus-with-one absorber,
     * plus-absorbs-times).  Sound in every absorptive semiring; a
     * semiring tolerating the stronger Boolean rewrite tolerates this
     * weaker, Boolean-function-preserving one as well. */
    if(isAbsorptiveAssumed(u) && !semiring.absorptive()
       && !semiring.compatibleWithBooleanRewrite())
      throw CircuitException(
              "The requested semiring is not absorptive; this gate's "
              "wires were rewritten under an absorptive rule "
              "(plus-idempotence, plus-with-one absorber or "
              "plus-absorbs-times, applied under the 'absorptive' or "
              "'boolean' provenance class) and the evaluation is "
              "unsound under this semiring.  Re-run under the "
              "'semiring' provenance class, or pick an absorptive "
              "semiring (probability, boolean, nonnegative "
              "tropical, ...).");

    if(provenance_mapping.find(u) != provenance_mapping.end()) {
      stack.pop_back();
      continue;
    }

    const auto t = getGateType(u);

    /* Leaves. */
    switch(t) {
    case gate_one:
    case gate_update:
      provenance_mapping.emplace(u, semiring.one());
      stack.pop_back();
      continue;
    case gate_input:
    case gate_mulinput:
      // A variable leaf the provenance mapping did not name.  By default
      // it contributes no provenance (the semiring's one); a rendering
      // semiring overrides unmapped_input to identify it instead.
      provenance_mapping.emplace(u, semiring.unmapped_input(getUUID(u)));
      stack.pop_back();
      continue;
    case gate_zero:
      provenance_mapping.emplace(u, semiring.zero());
      stack.pop_back();
      continue;
    case gate_value:
      provenance_mapping.emplace(u, semiring.value(getExtra(u)));
      stack.pop_back();
      continue;
    case gate_assumed:
      /* Structural assumption marker: the wrapped sub-circuit was
       * computed under the assumption named by the gate's label (the
       * extra string; a gate stored without a label defaults to
       * 'boolean').  Identity for semirings satisfying
       * the assumption; fatal for the rest, since otherwise we would
       * silently return a value the semiring's semantics does not
       * justify.
       *
       * - 'boolean': the sub-circuit only preserves the Boolean
       *   function of the lineage (e.g. the safe-query rewrite
       *   collapses derivation multiplicities into a single witness);
       *   sound for semirings admitting a homomorphism from Boolean
       *   functions.
       * - 'absorptive': the sub-circuit only represents the
       *   absorptive (Sorp) quotient of the recursive provenance --
       *   compiled by the bounded-treewidth reachability route (whose world
       *   enumeration surfaces exactly the minimal derivation
       *   supports); longer derivations are absorbed in any
       *   absorptive semiring but genuinely missing for the rest
       *   (Deutch, Milo, Roy & Tannen, ICDT 2014). */
      {
        const std::string assumption = getExtra(u);
        if(assumption.empty() || assumption == "boolean") {
          if(!semiring.compatibleWithBooleanRewrite())
            throw CircuitException(
                    "The requested semiring does not admit a homomorphism "
                    "from Boolean functions; the wrapped sub-circuit was "
                    "computed under a Boolean-provenance assumption "
                    "(typically by the safe-query rewrite, "
                    "provenance class 'boolean') and the evaluation is "
                    "unsound under this semiring.  Re-run the query under "
                    "a more general provenance class, or pick a "
                    "Boolean-compatible semiring (boolean, boolexpr, "
                    "formula, ...).");
        } else if(assumption == "absorptive") {
          if(semiring.absorptive() && !semiring.commutative())
            throw CircuitRefusal(
                    PROVSQL_GAP, "absorptive-noncommutative",
                    "The wrapped sub-circuit only represents the absorptive "
                    "quotient of a recursive query's provenance (fixpoint "
                    "truncation or compiled reachability circuit), whose "
                    "products are not in the order of the query, and the "
                    "requested semiring is not commutative.  Re-run the "
                    "query under the 'semiring' provenance class.");
          if(!semiring.absorptive())
            throw CircuitException(
                    "The requested semiring is not absorptive; the "
                    "wrapped sub-circuit only represents the absorptive "
                    "quotient of a recursive query's provenance "
                    "(fixpoint truncation or compiled reachability "
                    "circuit), so its value is only defined for "
                    "absorptive semirings (probability, boolean, "
                    "formula-with-absorption, nonnegative tropical, "
                    "...).  Counting and why-provenance of such a "
                    "recursion are genuinely infinite: a tuple derived "
                    "through itself gains a derivation per round, which "
                    "cyclic data does and so does a null-padded row that "
                    "re-derives itself or a projection onto constants, on "
                    "acyclic data.");
          /* CAVEAT: absorptive() is a coarser gate than the compiled
           * reachability route's actual soundness condition.  That route
           * materialises its world enumeration with genuine negation: each
           * absent edge surfaces as monus(one, edge) (BooleanGate::NOT
           * lowered to gate_monus; see ReachabilityCompiler.cpp and
           * CertifiedDDMaterialize.cpp).  The absorptive-quotient value
           * comes out right only because, in every absorptive semiring we
           * currently ship, (i) monus(one, x) is the times-neutral 'one' on
           * a present-priced leaf, so the negative literals do not perturb
           * the path-products, and (ii) any world a negative literal would
           * kill is dominated by an edge-superset of equal value, hence
           * absorbed.  semiring.absorptive() checks NEITHER property.  A
           * future or user-defined absorptive m-semiring whose monus(one, .)
           * is not the times-neutral, or whose monus is not
           * "drop-if-dominated", would pass this gate yet read those
           * monus(one, edge) gates with a value the path-sum argument does
           * not justify -- a silently wrong result.  If such a semiring is
           * added, strengthen this guard (e.g. assert monus(one, x) == one
           * for present-priced leaves, or add a dedicated capability flag)
           * rather than relying on absorptive() alone.  (Truncated cyclic
           * recursion, the other 'absorptive' producer, ships only minimal
           * derivations and carries no such negation, so it is unaffected.) */
        } else
          throw CircuitException(
                  "Unknown assumption marker '" + assumption + "'");
      }
      break;
    case gate_fixpoint: {
      /* A component of the least solution of an equation system: solved
       * once per evaluation, for the whole system, without visiting the
       * system's wires as an ordinary sub-circuit (its unknowns have no
       * value of their own). */
      const auto &w = getWires(u);
      if(w.size() != 1 || getGateType(w[0]) != gate_fixsystem)
        throw CircuitException(
                "gate_fixpoint must have exactly one child, a gate_fixsystem");
      auto it = systems.find(w[0]);
      if(it == systems.end()) {
        it = systems.emplace(
          w[0], solveFixSystem(w[0], provenance_mapping, semiring)).first;
        solved_systems.emplace_back(
          w[0], std::make_shared<std::vector<typename S::value_type> >(it->second));
      }
      const unsigned idx = getInfos(u).first;
      if(idx < 1 || idx > it->second.size())
        throw CircuitException("gate_fixpoint index out of range");
      provenance_mapping.emplace(u, it->second[idx-1]);
      stack.pop_back();
      continue;
    }
    case gate_fixvar:
    case gate_fixsystem:
      throw CircuitException(
              "An equation system or one of its unknowns has no value of "
              "its own; it is read through a gate_fixpoint");
    case gate_cmp:
    {
      bool ok;
      cmpOpFromOid(getInfos(u).first, ok);
      if(!ok)
        throw CircuitException(
                "Comparison operator OID " +
                std::to_string(getInfos(u).first) +
                " not supported");
      break;
    }
    default:
      break;
    }

    /* Internal gate: make sure every child is computed first. */
    {
      bool ready = true;
      for(const auto &c : getWires(u))
        if(provenance_mapping.find(c) == provenance_mapping.end()) {
          stack.push_back(c);
          ready = false;
        }
      if(!ready)
        continue;
    }

    const auto childValue = [&](int i) -> const typename S::value_type & {
                              return provenance_mapping.at(getWires(u)[i]);
                            };

    switch(t) {
    case gate_plus:
    case gate_times:
    case gate_monus: {
      std::vector<typename S::value_type> childrenResult;
      for(const auto &c : getWires(u))
        childrenResult.push_back(provenance_mapping.at(c));
      if(t==gate_plus) {
        childrenResult.erase(std::remove(std::begin(childrenResult), std::end(childrenResult), semiring.zero()),
                             childrenResult.end());
        provenance_mapping.emplace(u, semiring.plus(childrenResult));
      } else if(t==gate_times) {
        bool zero = false;
        for(const auto &c: childrenResult) {
          if(c==semiring.zero()) {
            zero = true;
            break;
          }
        }
        if(zero)
          provenance_mapping.emplace(u, semiring.zero());
        else {
          childrenResult.erase(std::remove(std::begin(childrenResult), std::end(childrenResult), semiring.one()),
                               childrenResult.end());
          provenance_mapping.emplace(u, semiring.times(childrenResult));
        }
      } else {
        if(childrenResult[0]==semiring.zero() || childrenResult[0]==childrenResult[1])
          provenance_mapping.emplace(u, semiring.zero());
        else
          provenance_mapping.emplace(u, semiring.monus(childrenResult[0], childrenResult[1]));
      }
      break;
    }

    case gate_delta:
      provenance_mapping.emplace(u, semiring.delta(childValue(0)));
      break;

    case gate_project:
    case gate_eq:
    case gate_annotation:
    case gate_assumed:
      // Where-provenance gates, the transparent annotation wrapper and the
      // (compatibility-checked above) Boolean-assumption marker: identity
      // for every admissible semiring.  The annotation's extra string is
      // inert metadata at evaluation time.
      provenance_mapping.emplace(u, childValue(0));
      break;

    case gate_cmp:
    {
      bool ok;
      ComparisonOperator op = cmpOpFromOid(getInfos(u).first, ok);
      provenance_mapping.emplace(u, semiring.cmp(childValue(0), op, childValue(1)));
      break;
    }

    case gate_semimod:
      provenance_mapping.emplace(u, semiring.semimod(childValue(0), childValue(1)));
      break;

    case gate_agg:
    {
      auto infos = getInfos(u);

      AggregationOperator op = getAggregationOperator(infos.first);

      std::vector<typename S::value_type> vec;
      for(const auto &c : getWires(u))
        vec.push_back(provenance_mapping.at(c));
      provenance_mapping.emplace(u, semiring.agg(op, vec));
      break;
    }

    case gate_conditioned: {
      /* Conditioning marker: P(·|C) requires a normalising division that
       * no general semiring provides (m-semirings have monus, not a
       * multiplicative inverse).  A conditioned token is evaluable only
       * in the measure interpretation (probability_evaluate, special-
       * cased at the root, or the random-variable / agg_token
       * distribution evaluators); the base-class hook refuses it for
       * every semiring but the symbolic Formula, which renders the
       * marker instead of interpreting it. */
      std::vector<typename S::value_type> vec;
      for(const auto &c : getWires(u))
        vec.push_back(provenance_mapping.at(c));
      provenance_mapping.emplace(u, semiring.conditioned(vec));
      break;
    }

    case gate_mobius: {
      /* The signed Möbius combination is a probability-only shortcut layered
       * over the normal provenance: the gate carries the literal lineage as a
       * designated child marked "L:<uuid>" in extra.  Every non-probability
       * evaluator (this semiring path, hence Shapley / Banzhaf / PROV export)
       * is TRANSPARENT to that lineage, so the token behaves like the ordinary
       * provenance of the query.  A nested gate_mobius (an inner
       * inclusion-exclusion step) carries no lineage: its value is never used
       * (the root passes through to the top lineage), but it must not throw, so
       * it falls back to its first child. */
      const std::string ex = getExtra(u);
      gate_t lineage = u;   // sentinel: not found
      const std::string key = "L:";
      std::size_t p = ex.find(key);
      if(p != std::string::npos) {
        std::size_t e = ex.find(' ', p);
        const std::string luid =
          ex.substr(p + key.size(),
                    e == std::string::npos ? std::string::npos : e - p - key.size());
        for(const auto &c : getWires(u))
          if(getUUID(c) == luid) { lineage = c; break; }
      }
      if(lineage != u)
        provenance_mapping.emplace(u, provenance_mapping.at(lineage));
      else
        provenance_mapping.emplace(u, childValue(0));
      break;
    }

    case gate_case: {
      /* Guarded selection over scalar (RV) children: a value chosen by the
       * first satisfied guard event.  This is a measure/RV-carrier operation
       * (the guards are probabilistic events, the values random variables), not
       * a semiring one -- evaluable only through the random-variable / measure
       * evaluators (expected / variance / support / probability / sample),
       * exactly like gate_rv and gate_arith over RVs. */
      std::vector<typename S::value_type> vec;
      for(const auto &c : getWires(u))
        vec.push_back(provenance_mapping.at(c));
      provenance_mapping.emplace(u, semiring.guarded_case(vec));
      break;
    }

    /* The measure-carrier gates below have no algebraic reading either:
     * their base-class hooks refuse them for every proper semiring, and
     * Formula overrides them to render the sub-circuit symbolically. */
    case gate_rv: {
      /* A gate_rv is a leaf unless one of its distribution parameters is
       * wired ("$i" in the extra encoding), which makes it a compound
       * (latent-variable) leaf over the values of its wires. */
      std::vector<typename S::value_type> params;
      for(const auto &c : getWires(u))
        params.push_back(provenance_mapping.at(c));
      provenance_mapping.emplace(u, semiring.rv(getExtra(u), params));
      break;
    }

    case gate_arith: {
      bool ok;
      ArithmeticOperator op = arithOpFromTag(getInfos(u).first, ok);
      if(!ok)
        throw CircuitException(
                "Arithmetic operator tag " +
                std::to_string(getInfos(u).first) +
                " not supported");
      std::vector<typename S::value_type> vec;
      for(const auto &c : getWires(u))
        vec.push_back(provenance_mapping.at(c));
      provenance_mapping.emplace(u, semiring.arith(op, vec, getExtra(u)));
      break;
    }

    case gate_mixture: {
      const auto &w = getWires(u);
      if(isCategoricalMixture(u)) {
        /* [key, mul_1, …, mul_n]: each outcome's probability lives in the
         * mulinput's prob and its value in the mulinput's extra (those
         * leaves evaluate to one() on their own, so the payload has to be
         * read off the gate here). */
        std::vector<double> probs;
        std::vector<std::string> outcomes;
        for(std::size_t i = 1; i < w.size(); ++i) {
          probs.push_back(getProb(w[i]));
          outcomes.push_back(getExtra(w[i]));
        }
        provenance_mapping.emplace(
          u, semiring.categorical(childValue(0), probs, outcomes));
      } else {
        if(w.size() != 3)
          throw CircuitException(
                  "gate_mixture must have exactly three children "
                  "[p_token, x_token, y_token]");
        provenance_mapping.emplace(
          u, semiring.mixture(childValue(0), childValue(1), childValue(2)));
      }
      break;
    }

    case gate_observe: {
      const auto &w = getWires(u);
      if(w.size() != 1)
        throw CircuitException(
                "gate_observe must have exactly one child (the observed leaf)");
      provenance_mapping.emplace(u, semiring.observe(childValue(0), getExtra(u)));
      break;
    }

    default:
      throw CircuitException("Invalid gate type for semiring evaluation");
    }

    stack.pop_back();
  }

  return provenance_mapping.at(g);
}

template<typename S, std::enable_if_t<std::is_base_of_v<semiring::Semiring<typename S::value_type>, S>, int> >
std::vector<typename S::value_type> GenericCircuit::solveFixSystem(gate_t sys, std::unordered_map<gate_t, typename S::value_type> &provenance_mapping, S semiring) const
{
  using V = typename S::value_type;

  const auto &w = getWires(sys);
  if(w.size() % 2)
    throw CircuitException("gate_fixsystem must have an even number of wires");
  const std::size_t n = w.size() / 2;

  std::unordered_map<gate_t, std::size_t> unknown;
  for(std::size_t i = 0; i < n; ++i) {
    if(getGateType(w[i]) != gate_fixvar)
      throw CircuitException(
              "The first half of a gate_fixsystem's wires must be its "
              "gate_fixvar unknowns");
    unknown.emplace(w[i], i);
  }

  const V zero = semiring.zero(), one = semiring.one();
  const auto plus2 = [&](const V &a, const V &b) -> V {
                       if(semiring.equal(a, zero)) return b;
                       if(semiring.equal(b, zero)) return a;
                       return semiring.plus(std::vector<V>{a, b});
                     };
  const auto times2 = [&](const V &a, const V &b) -> V {
                        if(semiring.equal(a, zero) || semiring.equal(b, zero)) return zero;
                        if(semiring.equal(a, one)) return b;
                        if(semiring.equal(b, one)) return a;
                        return semiring.times(std::vector<V>{a, b});
                      };
  /* l ⊗ x ⊗ r, in this order: ⊗ need not commute. */
  const auto times3 = [&](const V &l, const V &x, const V &r) -> V {
                        return times2(times2(l, x), r);
                      };

  /* Which gates depend on an unknown: an iterative post-order, since the
   * sub-circuits that do not (the input tokens' own provenance) can be as
   * deep as the data.  Another system's gate_fixpoint is a constant here;
   * an unknown of another system cannot occur. */
  std::unordered_map<gate_t, bool> dep;
  {
    std::vector<gate_t> st;
    for(std::size_t i = n; i < w.size(); ++i)
      st.push_back(w[i]);
    while(!st.empty()) {
      const gate_t u = st.back();
      if(dep.count(u)) { st.pop_back(); continue; }
      const auto t = getGateType(u);
      if(t == gate_fixvar) {
        if(!unknown.count(u))
          throw CircuitException(
                  "An unknown of another equation system occurs in this one");
        dep.emplace(u, true);
        st.pop_back();
        continue;
      }
      if(t == gate_fixpoint) {
        dep.emplace(u, false);
        st.pop_back();
        continue;
      }
      bool ready = true;
      for(const auto &c : getWires(u))
        if(!dep.count(c)) { st.push_back(c); ready = false; }
      if(!ready) continue;
      bool d = false;
      for(const auto &c : getWires(u))
        d = d || dep.at(c);
      dep.emplace(u, d);
      st.pop_back();
    }
  }

  /* Linear form b ⊕ ⨁_j l_j ⊗ x_j ⊗ r_j of a gate that depends on the
   * unknowns, the factors on either side of an unknown kept apart, as ⊗
   * need not commute.  Only the gates between an equation's root and its
   * unknowns are visited here, a part of the circuit as shallow as the
   * recursive term itself, so the recursion is bounded by the query, not
   * the data. */
  struct Term {
    std::size_t j;
    V l, r;
  };
  struct Lin {
    V c;
    std::vector<Term> terms;
  };
  std::unordered_map<gate_t, Lin> lin;
  std::function<const Lin &(gate_t)> linear = [&](gate_t u) -> const Lin & {
    auto it = lin.find(u);
    if(it != lin.end())
      return it->second;

    if(isBooleanAssumed(u) && !semiring.compatibleWithBooleanRewrite())
      throw CircuitException(
              "The requested semiring does not admit a homomorphism from "
              "Boolean functions; a gate of this recursion's equations was "
              "rewritten under a Boolean-only rule");
    if(isAbsorptiveAssumed(u) && !semiring.absorptive()
       && !semiring.compatibleWithBooleanRewrite())
      throw CircuitException(
              "The requested semiring is not absorptive; a gate of this "
              "recursion's equations was rewritten under an absorptive rule");

    Lin r{zero, {}};
    const auto t = getGateType(u);
    switch(t) {
    case gate_fixvar:
      r.terms.push_back(Term{unknown.at(u), one, one});
      break;

    case gate_plus:
      for(const auto &c : getWires(u)) {
        if(dep.at(c)) {
          const Lin &l = linear(c);
          r.c = plus2(r.c, l.c);
          r.terms.insert(r.terms.end(), l.terms.begin(), l.terms.end());
        } else
          r.c = plus2(r.c, evaluate(c, provenance_mapping, semiring));
      }
      break;

    case gate_times: {
      /* The factors before the unknown, and those after it. */
      V kl = one, kr = one;
      gate_t var = u;
      for(const auto &c : getWires(u)) {
        if(dep.at(c)) {
          if(var != u)
            throw CircuitRefusal(
                    PROVSQL_GAP, "recursion-nonlinear",
                    "This recursion's equations are not linear: a product "
                    "has two factors depending on the recursive relation");
          var = c;
        } else if(var == u)
          kl = times2(kl, evaluate(c, provenance_mapping, semiring));
        else
          kr = times2(kr, evaluate(c, provenance_mapping, semiring));
      }
      const Lin &l = linear(var);
      if(!semiring.equal(kl, zero) && !semiring.equal(kr, zero)) {
        r.c = times3(kl, l.c, kr);
        for(const auto &t : l.terms)
          r.terms.push_back(Term{t.j, times2(kl, t.l), times2(t.r, kr)});
      }
      break;
    }

    case gate_assumed: {
      const std::string assumption = getExtra(u);
      if((assumption.empty() || assumption == "boolean")
         && !semiring.compatibleWithBooleanRewrite())
        throw CircuitException(
                "The requested semiring does not admit a homomorphism from "
                "Boolean functions; part of this recursion's equations was "
                "computed under a Boolean-provenance assumption");
      if(assumption == "absorptive" && semiring.absorptive() &&
         !semiring.commutative())
        throw CircuitRefusal(
                PROVSQL_GAP, "absorptive-noncommutative",
                "Part of this recursion's equations was computed under an "
                "absorptive assumption, whose products are not in the "
                "order of the query, and the requested semiring is not "
                "commutative.  Re-run the query under the 'semiring' "
                "provenance class.");
      if(assumption == "absorptive" && !semiring.absorptive())
        throw CircuitException(
                "The requested semiring is not absorptive; part of this "
                "recursion's equations was computed under an absorptive "
                "assumption");
      r = linear(getWires(u)[0]);
      break;
    }

    case gate_project:
    case gate_eq:
    case gate_annotation:
      r = linear(getWires(u)[0]);
      break;

    default:
      throw CircuitRefusal(
              PROVSQL_GAP, "recursion-nonlinear",
              std::string("This recursion's equations are not linear "
                          "semiring expressions in the recursive relation "
                          "(gate of type ") + gate_type_name[t] +
              " over it)");
    }
    return lin.emplace(u, std::move(r)).first->second;
  };

  /* x_i = b[i] ⊕ ⨁ l ⊗ x_j ⊗ r over in[i] = {(j, l, r)}; out[j] holds
   * the same terms, indexed by i. */
  std::vector<V> b(n, zero);
  std::vector<std::vector<Term> > in(n), out(n);
  for(std::size_t i = 0; i < n; ++i) {
    const gate_t f = w[n + i];
    if(dep.at(f)) {
      const Lin &l = linear(f);
      b[i] = l.c;
      for(const auto &t : l.terms)
        if(!semiring.equal(t.l, zero) && !semiring.equal(t.r, zero)) {
          in[i].push_back(t);
          out[t.j].push_back(Term{i, t.l, t.r});
        }
    } else
      b[i] = evaluate(f, provenance_mapping, semiring);
  }

  /* Strongly connected components of the dependency graph j -> i, in
   * solving order. */
  std::vector<std::vector<std::size_t> > comps;
  {
    std::vector<std::vector<std::size_t> > adj(n);
    for(std::size_t j = 0; j < n; ++j)
      for(const auto &e : out[j])
        adj[j].push_back(e.j);
    comps = fixSystemComponents(adj);
  }

  std::vector<V> x(n, zero);
  std::vector<std::size_t> comp_of(n);
  for(std::size_t c = 0; c < comps.size(); ++c)
    for(const auto v : comps[c])
      comp_of[v] = c;

  /* Better-than in the natural order of an absorptive, selective
   * semiring: a ⊕ b = a, with a ≠ b. */
  const auto better = [&](const V &a, const V &b) -> bool {
                        return !semiring.equal(a, b) && semiring.equal(plus2(a, b), a);
                      };

  for(std::size_t ci = 0; ci < comps.size(); ++ci) {
    provsql_poll_interrupt();
    const auto &comp = comps[ci];

    /* What flows into the component: b, and the components already
     * solved. */
    std::vector<V> entry;
    entry.reserve(comp.size());
    bool cyclic = comp.size() > 1;
    for(const auto i : comp) {
      V v = b[i];
      for(const auto &t : in[i]) {
        if(comp_of[t.j] != ci)
          v = plus2(v, times3(t.l, x[t.j], t.r));
        else
          cyclic = true;    // a self-loop makes a singleton cyclic too
      }
      entry.push_back(v);
    }

    if(!cyclic) {
      /* A tuple not derived through itself: its equation is its value,
       * exact in every semiring. */
      x[comp[0]] = entry[0];
      continue;
    }

    if(semiring.symbolic()) {
      /* A rendering semiring: the component's tuples become named unknowns,
       * and their equations are recorded with them. */
      for(const auto i : comp)
        x[i] = semiring.symbolic_unknown();
      for(std::size_t k = 0; k < comp.size(); ++k) {
        V v = entry[k];
        for(const auto &t : in[comp[k]])
          if(comp_of[t.j] == ci)
            v = plus2(v, times3(t.l, x[t.j], t.r));
        semiring.define(x[comp[k]], v);
      }
      continue;
    }

    if(!semiring.absorptive()) {
      if(!semiring.has_star())
        throw CircuitRefusal(
                PROVSQL_DELIBERATE, "recursion-cycle-no-value",
                "This recursion's equations are cyclic (a tuple is derived "
                "through itself), and the requested semiring is neither "
                "absorptive nor gives the sum of the powers of a value: the "
                "least solution is an infinite sum it has no value for.");

      /* Gaussian elimination with star needs every unknown on the same
       * side of its coefficient: x_i = ⨁ a ⊗ x_j ⊕ e (left-linear), or
       * x_i = ⨁ x_j ⊗ a ⊕ e (right-linear), which is left-linear in the
       * semiring with ⊗ reversed, of the same star.  An unknown between two
       * factors, or a mix of both sides, makes the derivations a
       * context-free language, with no closed form by star.  In a
       * commutative semiring, the side does not matter. */
      bool right = false;
      if(!semiring.commutative()) {
        bool left_ok = true, right_ok = true;
        for(const auto i : comp)
          for(const auto &t : in[i])
            if(comp_of[t.j] == ci) {
              left_ok = left_ok && semiring.equal(t.r, one);
              right_ok = right_ok && semiring.equal(t.l, one);
            }
        if(!left_ok && !right_ok)
          throw CircuitRefusal(
                  PROVSQL_GAP, "recursion-two-sided",
                  "This recursion's equations put the recursive relation "
                  "between factors, or on both sides of them, and the "
                  "requested semiring is not commutative: its least "
                  "solution is not given by the star of a value.  Write the "
                  "recursive relation at the same place of every recursive "
                  "FROM clause (first, or last).");
        right = !left_ok;
      }
      /* a ⊗ b, or b ⊗ a in the reversed semiring. */
      const auto mul = [&](const V &a, const V &b) -> V {
                         return right ? times2(b, a) : times2(a, b);
                       };

      /* Gaussian elimination with star (node elimination; Ramusat, Maniu &
       * Senellart, EDBT 2021, after Lehmann 1977): x_v = a_vv* ⊗ (⨁ a_vj
       * x_j ⊕ e_v) is substituted into the equations reading x_v, one
       * unknown at a time, then the unknowns are read back in reverse
       * order.  Each step eliminates the unknown adding the fewest new
       * coefficients, which follows the component's width where it is
       * small. */
      const std::size_t k = comp.size();
      std::unordered_map<std::size_t, std::size_t> at;
      for(std::size_t p = 0; p < k; ++p)
        at.emplace(comp[p], p);
      std::vector<std::map<std::size_t, V> > row(k);
      std::vector<std::set<std::size_t> > col(k);
      std::vector<V> e(entry);
      for(std::size_t p = 0; p < k; ++p)
        for(const auto &t : in[comp[p]]) {
          if(comp_of[t.j] != ci)
            continue;
          const V a = semiring.commutative() ? times2(t.l, t.r)
                                              : right ? t.r : t.l;
          const std::size_t q = at.at(t.j);
          auto it = row[p].find(q);
          row[p][q] = it == row[p].end() ? a : plus2(it->second, a);
          col[q].insert(p);
        }

      std::vector<char> gone(k, 0);
      std::vector<std::size_t> order;
      for(std::size_t step = 0; step < k; ++step) {
        provsql_poll_interrupt();
        std::size_t v = k, best = 0;
        for(std::size_t p = 0; p < k; ++p) {
          if(gone[p])
            continue;
          const std::size_t cost = row[p].size() * col[p].size();
          if(v == k || cost < best) {
            v = p;
            best = cost;
          }
        }
        V loop = zero;
        auto self = row[v].find(v);
        if(self != row[v].end()) {
          loop = self->second;
          row[v].erase(self);
          col[v].erase(v);
        }
        const V s = semiring.star(loop);
        for(auto &[j, b] : row[v])
          b = mul(s, b);
        e[v] = mul(s, e[v]);
        for(const auto p : std::vector<std::size_t>(col[v].begin(), col[v].end())) {
          if(gone[p] || p == v)
            continue;
          const V c = row[p].at(v);
          row[p].erase(v);
          for(const auto &[j, b] : row[v]) {
            auto it = row[p].find(j);
            const V t = mul(c, b);
            row[p][j] = it == row[p].end() ? t : plus2(it->second, t);
            col[j].insert(p);
          }
          e[p] = plus2(e[p], mul(c, e[v]));
        }
        gone[v] = 1;
        order.push_back(v);
      }
      for(std::size_t r = order.size(); r-- > 0; ) {
        const std::size_t v = order[r];
        V val = e[v];
        for(const auto &[j, b] : row[v])
          val = plus2(val, mul(b, x[comp[j]]));
        x[comp[v]] = val;
      }
      continue;
    }

    std::unordered_map<std::size_t, std::size_t> pos;
    for(std::size_t k = 0; k < comp.size(); ++k) {
      pos.emplace(comp[k], k);
      x[comp[k]] = entry[k];
    }

    if(semiring.selective() && semiring.exact_equality()) {
      /* Dijkstra from the entry values: in an absorptive semiring
       * extending a derivation never improves it, so with a selective plus
       * the best unsettled value is final (Ramusat, Maniu & Senellart,
       * EDBT 2021). */
      const auto worse = [&](const std::pair<V, std::size_t> &p,
                             const std::pair<V, std::size_t> &q) {
                           return better(q.first, p.first);
                         };
      std::priority_queue<std::pair<V, std::size_t>,
                          std::vector<std::pair<V, std::size_t> >,
                          decltype(worse)> heap(worse);
      std::vector<char> settled(comp.size(), 0);
      for(const auto i : comp)
        if(!semiring.equal(x[i], zero))
          heap.emplace(x[i], i);
      while(!heap.empty()) {
        auto [v, j] = heap.top();
        heap.pop();
        const std::size_t pj = pos.at(j);
        if(settled[pj] || !semiring.equal(v, x[j]))
          continue;
        settled[pj] = 1;
        for(const auto &t : out[j]) {
          const std::size_t i = t.j;
          if(comp_of[i] != ci || settled[pos.at(i)])
            continue;
          const V w = plus2(x[i], times3(t.l, x[j], t.r));
          if(!semiring.equal(w, x[i])) {
            x[i] = w;
            heap.emplace(w, i);
          }
        }
      }
    } else if(semiring.exact_equality()) {
      /* Value iteration with a work list (Mohri's generic single-source
       * algorithm, the residual being superfluous in an idempotent
       * semiring): a value only moves towards the solution, and
       * absorption cuts every derivation repeating a tuple, so the
       * iteration converges.  The bound only guards against a semiring
       * declaring absorption it does not have. */
      std::size_t nedges = 0;
      for(const auto i : comp)
        nedges += in[i].size();
      const std::size_t bound = (comp.size() + 1) * (nedges + 1);
      std::size_t steps = 0;
      std::vector<char> queued(comp.size(), 0);
      std::deque<std::size_t> queue;
      for(const auto i : comp)
        if(!semiring.equal(x[i], zero)) {
          queue.push_back(i);
          queued[pos.at(i)] = 1;
        }
      while(!queue.empty()) {
        provsql_poll_interrupt();
        const std::size_t j = queue.front();
        queue.pop_front();
        queued[pos.at(j)] = 0;
        for(const auto &t : out[j]) {
          const std::size_t i = t.j;
          if(comp_of[i] != ci)
            continue;
          if(++steps > bound)
            throw CircuitException(
                    "Value iteration over this recursion's equations does "
                    "not converge; the semiring does not behave "
                    "absorptively");
          const V w = plus2(x[i], times3(t.l, x[j], t.r));
          if(!semiring.equal(w, x[i])) {
            x[i] = w;
            if(!queued[pos.at(i)]) {
              queue.push_back(i);
              queued[pos.at(i)] = 1;
            }
          }
        }
      }
    } else {
      /* Values whose equality cannot be tested (gates of a circuit being
       * built, ...): run the rounds absorption guarantees to suffice.
       * Round r covers the derivations entering the component and taking
       * at most r steps inside it; one repeating a tuple is absorbed by
       * the shorter one skipping the repetition, and a step-wise
       * repetition-free one takes at most |comp| - 1 steps. */
      for(std::size_t r = 1; r < comp.size(); ++r) {
        provsql_poll_interrupt();
        std::vector<V> next(entry);
        for(std::size_t k = 0; k < comp.size(); ++k)
          for(const auto &t : in[comp[k]])
            if(comp_of[t.j] == ci)
              next[k] = plus2(next[k], times3(t.l, x[t.j], t.r));
        for(std::size_t k = 0; k < comp.size(); ++k)
          x[comp[k]] = next[k];
      }
    }
  }
  return x;
}
