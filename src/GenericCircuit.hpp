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
 * | gate_input  | lookup in @p provenance_mapping|
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
 */
#include "GenericCircuit.h"

extern "C" {
#include "utils/lsyscache.h"
#include "miscadmin.h"        // check_stack_depth
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

  while(!stack.empty()) {
    const gate_t u = stack.back();

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
    case gate_input:
    case gate_update:
    case gate_mulinput:
      // If not in provenance mapping, return no provenance (one of the semiring)
      provenance_mapping.emplace(u, semiring.one());
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
       * extra string; gates from stores predating the label default to
       * the historical 'boolean').  Identity for semirings satisfying
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
                    "...).  Counting and why-provenance of cyclic "
                    "recursion are genuinely infinite; on acyclic "
                    "data, re-run under the 'semiring' provenance "
                    "class.");
        } else
          throw CircuitException(
                  "Unknown assumption marker '" + assumption + "'");
      }
      break;
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

    case gate_conditioned:
      /* Conditioning marker: P(·|C) requires a normalising division that
       * no general semiring provides (m-semirings have monus, not a
       * multiplicative inverse).  A conditioned token is evaluable only
       * in the measure interpretation (probability_evaluate, special-
       * cased at the root, or the random-variable / agg_token
       * distribution evaluators), never under a generic sr_* semiring. */
      throw CircuitException(
              "The requested semiring does not support conditioning: "
              "P(·|C) = P(·∧C)/P(C) needs a normalising division "
              "no general semiring provides.  A conditioned token is "
              "evaluable only in the measure interpretation "
              "(probability_evaluate, or the random-variable / agg_token "
              "distribution evaluators).");

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

    default:
      throw CircuitException("Invalid gate type for semiring evaluation");
    }

    stack.pop_back();
  }

  return provenance_mapping.at(g);
}
