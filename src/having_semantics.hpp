/**
 * @file having_semantics.hpp
 * @brief Provenance evaluation helper for HAVING-clause circuits.
 *
 * When a query includes a HAVING clause, ProvSQL creates a special
 * sub-circuit that encodes the aggregate predicate.  Before the main
 * provenance circuit can be evaluated over a semiring, these HAVING
 * sub-circuits must first be evaluated to determine which groups
 * pass the filter.
 *
 * The single public entry point @c provsql_having() rewrites HAVING
 * comparison gates in the circuit by enumerating possible worlds, for
 * any compiled semiring.  If the HAVING gate type is incompatible with
 * the requested semiring, the function is a no-op.
 */
#ifndef PROVSQL_HAVING_SEMANTICS_HPP
#define PROVSQL_HAVING_SEMANTICS_HPP

#include <stdexcept>
#include <string>
#include <vector>

#include "GenericCircuit.hpp"
#include "subset.hpp"

/** @cond INTERNAL */
namespace provsql_having_detail {
std::vector<gate_t> collect_sp_cmp_gates(GenericCircuit &c, gate_t start);
bool extract_constant_C(GenericCircuit &c, gate_t x, int &C_out);
bool extract_constant_double(GenericCircuit &c, gate_t x, double &C_out);
bool extract_constant_string(GenericCircuit &c, gate_t x, std::string &C_out);
bool semimod_extract_M_and_K(GenericCircuit &c, gate_t semimod_gate, int &m_out, gate_t &k_gate_out);
bool semimod_extract_string_and_K(GenericCircuit &c, gate_t semimod_gate, std::string &m_out, gate_t &k_gate_out);
ComparisonOperator map_cmp_op(GenericCircuit &c, gate_t cmp_gate, bool &ok);
ComparisonOperator flip_op(ComparisonOperator op);
}
/** @endcond */

/**
 * @brief Rewrite HAVING comparison gates in the circuit by enumerating possible worlds.
 *
 * @tparam SemiringT  The semiring type used for evaluation.
 * @tparam MapT       The provenance mapping type (gate_t → semiring value).
 * @param c        Circuit to rewrite.
 * @param g        Root gate of the sub-circuit to inspect.
 * @param mapping  Provenance mapping updated in place.
 * @param S        Semiring instance (default-constructed for stateless semirings).
 */
template <typename SemiringT, typename MapT>
void provsql_having(
  GenericCircuit &c,
  gate_t g,
  MapT &mapping,
  SemiringT S = SemiringT{})
{
  using namespace provsql_having_detail;

  std::vector<gate_t> cmp_gates = collect_sp_cmp_gates(c, g);

  if (cmp_gates.empty())
    return;

  auto pw_from_cmp_gate = [&](gate_t cmp_gate, typename SemiringT::value_type &pw_out) -> bool {
    const auto &cw = c.getWires(cmp_gate);
    if (cw.size() != 2) return false;

    gate_t L = cw[0];
    gate_t R = cw[1];

    bool okop = false;
    ComparisonOperator op = map_cmp_op(c, cmp_gate, okop);
    if (!okop) return false;

    auto build_from = [&](gate_t agg_side, gate_t const_side, ComparisonOperator effective_op) -> bool {
      int C = 0;
      std::string C_str;
      bool is_string = false;

      if (!extract_constant_C(c, const_side, C)) {
        // The constant may instead be a text value (agg_token = text).
        if (!extract_constant_string(c, const_side, C_str)) return false;
        is_string = true;
      }

      if (c.getGateType(agg_side) != gate_agg) return false;

      const auto &children = c.getWires(agg_side);

      std::vector<long> mvals;
      mvals.reserve(children.size());

      std::vector<std::string> mvals_str;
      mvals_str.reserve(children.size());

      std::vector<typename SemiringT::value_type> kvals;
      kvals.reserve(children.size());

      for (gate_t ch : children) {
        if (c.getGateType(ch) != gate_semimod) return false;

        gate_t k_gate{};

        if (is_string) {
          std::string m_str;
          if (!semimod_extract_string_and_K(c, ch, m_str, k_gate)) return false;
          mvals_str.push_back(m_str);
        } else {
          int m = 0;
          if (!semimod_extract_M_and_K(c, ch, m, k_gate)) return false;
          mvals.push_back(m);
        }

        auto kval = c.evaluate<SemiringT>(k_gate, mapping, S);

        kvals.push_back(std::move(kval));
      }

      AggregationOperator agg_kind = getAggregationOperator(c.getInfos(agg_side).first);

      if (is_string) {
        // Comparison of an aggregate against a text constant.  Only choose()
        // is supported (it is the only text-valued aggregate whose
        // possible-world value is decided occurrence-by-occurrence), and only
        // = / <> are exposed for text.  Other aggregates would need their own
        // text semantics and are refused.
        if (agg_kind != AggregationOperator::CHOOSE)
          throw std::runtime_error(
            "comparing an aggregate with a text constant in HAVING "
            "is only implemented for choose()");
        if (effective_op != ComparisonOperator::EQ &&
            effective_op != ComparisonOperator::NE)
          throw std::runtime_error(
            "only = and <> are supported when comparing choose() "
            "with a text constant");

        // choose() is PICKFIRST: in a world W its value is that of the
        // lowest-index present element.  So choose(W) op C holds iff the
        // first present element matches.  Summing the annotations of all such
        // worlds telescopes (the free suffix of later elements sums to one),
        // giving the exact possible-worlds provenance in a single O(N) scan,
        // valid even when the group's elements are not mutually exclusive:
        //
        //   pw = ⊕_{i : vᵢ matches} kᵢ ⊗ (⊗_{j<i} (1 ⊖ kⱼ))
        //
        // where (⊗_{j<i} (1 ⊖ kⱼ)) ("no earlier element present") is carried
        // in @c prefix.
        const auto one  = S.one();
        std::vector<typename SemiringT::value_type> disjuncts;
        auto prefix = one;

        for (size_t i = 0; i < kvals.size(); ++i) {
          bool match = (effective_op == ComparisonOperator::EQ)
                         ? (mvals_str[i] == C_str)
                         : (mvals_str[i] != C_str);
          if (match) {
            if (prefix == one)
              disjuncts.push_back(kvals[i]);
            else if (kvals[i] == one)
              disjuncts.push_back(prefix);
            else
              disjuncts.push_back(S.times(
                std::vector<typename SemiringT::value_type>{kvals[i], prefix}));
          }

          // prefix ⊗= (1 ⊖ kᵢ): condition that element i is absent.
          auto absent = S.monus(one, kvals[i]);
          prefix = (prefix == one)
                     ? absent
                     : S.times(std::vector<typename SemiringT::value_type>{
                         prefix, absent});
        }

        pw_out = disjuncts.empty() ? S.zero() : S.plus(disjuncts);
        return true;
      }

      if(agg_kind==AggregationOperator::SUM) {
        // COUNT(*) is simulated by SUM of 1s
        bool all_one_mvals = true;
        for (int m : mvals) {
          if (m != 1) { all_one_mvals = false; break; }
        }
        agg_kind = all_one_mvals ? AggregationOperator::COUNT : AggregationOperator::SUM;
      }

      bool upset = false;
      auto worlds = enumerate_valid_worlds(mvals, C, effective_op, agg_kind, S.absorptive(), upset);

      if (worlds.empty()) {
        pw_out = S.zero();
        return true;
      }

      std::vector<typename SemiringT::value_type> disjuncts;
      disjuncts.reserve(worlds.size());

      const size_t n = kvals.size();

      for (auto mask : worlds) {
        std::vector<typename SemiringT::value_type> present;
        std::vector<typename SemiringT::value_type> missing;
        present.reserve(n);
        missing.reserve(n);

        for (size_t i = 0; i < n; ++i) {
          if (mask[i]) {
            if(kvals[i]!=S.one())
              present.push_back(kvals[i]);
          } else if(upset) {
            // The world enumeration produced an upset generated by a subset
          } else if ((op==ComparisonOperator::GE || op==ComparisonOperator::GT) && S.absorptive() &&
                     agg_kind==AggregationOperator::MAX) {
            // Monotonously increasing behavior: do not add anything to missing
          } else if((op==ComparisonOperator::LE || op==ComparisonOperator::LT) && S.absorptive() &&
                    agg_kind==AggregationOperator::MIN) {
            // Monotonously decreasing behavior: do not add anything to missing
          } else {
            if(kvals[i]!=S.zero())
              missing.push_back(kvals[i]);
          }
        }

        auto present_prod = S.times(present);

        if (missing.empty()) {
          disjuncts.push_back(std::move(present_prod));
        } else {
          auto missing_sum = S.plus(missing);
          auto monus_factor = S.monus(S.one(), missing_sum);
          auto term = monus_factor;
          if(present_prod!=S.one())
            term = S.times(std::vector<typename SemiringT::value_type>{present_prod, monus_factor});
          disjuncts.push_back(std::move(term));
        }
      }

      pw_out = S.plus(disjuncts);
      return true;
    };

    if (c.getGateType(L) == gate_agg)
      return build_from(L, R, op);

    if (c.getGateType(R) == gate_agg)
      return build_from(R, L, flip_op(op));

    return false;
  };

  for (gate_t cmp_gate : cmp_gates) {
    typename SemiringT::value_type pw;
    if (!pw_from_cmp_gate(cmp_gate, pw))
      return;

    mapping[cmp_gate] = std::move(pw);
  }
}

#endif
