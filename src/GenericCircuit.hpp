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

#include <deque>
#include <functional>

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
       *   either truncated at the absorptive value fixpoint (cyclic
       *   recursion stopped once every minimal,
       *   tuple-repetition-free, derivation is covered) or compiled
       *   by the bounded-treewidth reachability route (whose world
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
      if(it == systems.end())
        it = systems.emplace(
          w[0], solveFixSystem(w[0], provenance_mapping, semiring)).first;
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
                       if(a == zero) return b;
                       if(b == zero) return a;
                       return semiring.plus(std::vector<V>{a, b});
                     };
  const auto times2 = [&](const V &a, const V &b) -> V {
                        if(a == zero || b == zero) return zero;
                        if(a == one) return b;
                        if(b == one) return a;
                        return semiring.times(std::vector<V>{a, b});
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

  /* Linear form b ⊕ ⨁_j a_j ⊗ x_j of a gate that depends on the unknowns.
   * Only the gates between an equation's root and its unknowns are visited
   * here, a part of the circuit as shallow as the recursive term itself, so
   * the recursion is bounded by the query, not the data. */
  struct Lin {
    V c;
    std::vector<std::pair<std::size_t, V> > terms;
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
      r.terms.emplace_back(unknown.at(u), one);
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
      V k = one;
      gate_t var = u;
      for(const auto &c : getWires(u)) {
        if(dep.at(c)) {
          if(var != u)
            throw CircuitException(
                    "This recursion's equations are not linear: a product "
                    "has two factors depending on the recursive relation");
          var = c;
        } else
          k = times2(k, evaluate(c, provenance_mapping, semiring));
      }
      const Lin &l = linear(var);
      if(!(k == zero)) {
        r.c = times2(l.c, k);
        for(const auto &[j, a] : l.terms)
          r.terms.emplace_back(j, times2(a, k));
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
      throw CircuitException(
              std::string("This recursion's equations are not linear "
                          "semiring expressions in the recursive relation "
                          "(gate of type ") + gate_type_name[t] +
              " over it)");
    }
    return lin.emplace(u, std::move(r)).first->second;
  };

  /* x_i = b[i] ⊕ ⨁ a ⊗ x_j over in[i] = {(j, a)}. */
  std::vector<V> b(n, zero);
  std::vector<std::vector<std::pair<std::size_t, V> > > in(n), out(n);
  for(std::size_t i = 0; i < n; ++i) {
    const gate_t f = w[n + i];
    if(dep.at(f)) {
      const Lin &l = linear(f);
      b[i] = l.c;
      for(const auto &[j, a] : l.terms)
        if(!(a == zero)) {
          in[i].emplace_back(j, a);
          out[j].emplace_back(i, a);
        }
    } else
      b[i] = evaluate(f, provenance_mapping, semiring);
  }

  /* Topological order of the dependency graph j -> i (Kahn). */
  std::vector<std::size_t> indeg(n, 0), order;
  for(std::size_t i = 0; i < n; ++i)
    indeg[i] = in[i].size();
  for(std::size_t i = 0; i < n; ++i)
    if(indeg[i] == 0)
      order.push_back(i);
  for(std::size_t k = 0; k < order.size(); ++k)
    for(const auto &e : out[order[k]])
      if(--indeg[e.first] == 0)
        order.push_back(e.first);

  std::vector<V> x(n, zero);

  if(order.size() == n) {
    /* Acyclic: every derivation is finite, and the topological order
     * evaluates each equation once its right-hand side is known.  Exact
     * in every semiring. */
    for(const auto i : order) {
      V v = b[i];
      for(const auto &[j, a] : in[i])
        v = plus2(v, times2(a, x[j]));
      x[i] = v;
    }
    return x;
  }

  if(!semiring.absorptive())
    throw CircuitException(
            "This recursion's equations are cyclic (a tuple is derived "
            "through itself) and the requested semiring is not absorptive: "
            "the least solution is an infinite sum, which counting or "
            "why-provenance cannot give a value to.  Use an absorptive "
            "semiring (boolean, nonnegative tropical, Viterbi, ...).");

  /* Cyclic, absorptive: value iteration from b with a work list (Mohri's
   * generic single-source algorithm, the residual being superfluous in an
   * idempotent semiring).  A value only changes towards the solution, and
   * absorption cuts every derivation repeating a tuple, so the iteration
   * converges; the bound below only guards against a semiring declaring
   * absorption it does not have. */
  std::size_t nedges = 0;
  for(std::size_t i = 0; i < n; ++i)
    nedges += in[i].size();
  const std::size_t bound = (n + 1) * (nedges + 1);
  std::size_t steps = 0;

  std::vector<char> queued(n, 0);
  std::deque<std::size_t> queue;
  for(std::size_t i = 0; i < n; ++i) {
    x[i] = b[i];
    if(!(x[i] == zero)) {
      queue.push_back(i);
      queued[i] = 1;
    }
  }
  while(!queue.empty()) {
    provsql_poll_interrupt();
    const std::size_t j = queue.front();
    queue.pop_front();
    queued[j] = 0;
    for(const auto &[i, a] : out[j]) {
      if(++steps > bound)
        throw CircuitException(
                "Value iteration over this recursion's equations does not "
                "converge; the semiring does not behave absorptively");
      const V v = plus2(x[i], times2(a, x[j]));
      if(!(v == x[i])) {
        x[i] = v;
        if(!queued[i]) {
          queue.push_back(i);
          queued[i] = 1;
        }
      }
    }
  }
  return x;
}
