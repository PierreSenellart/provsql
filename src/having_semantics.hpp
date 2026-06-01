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

#include <algorithm>
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
bool aggtype_is_text(unsigned oid);
bool parse_decimal_scaled(const std::string &s, long &mantissa, int &scale);
bool rescale_to(long mantissa, int scale, int target_scale, long &out);
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
      if (c.getGateType(agg_side) != gate_agg) return false;

      // info2 of the gate_agg is the aggregate's result type -- the
      // comparison domain (int / numeric / float / text).
      const unsigned aggtype = c.getInfos(agg_side).second;
      AggregationOperator agg_kind = getAggregationOperator(c.getInfos(agg_side).first);
      const auto &children = c.getWires(agg_side);

      // ---- Text comparison domain: choose() over a text constant. ----
      if (aggtype_is_text(aggtype)) {
        std::string C_str;
        if (!extract_constant_string(c, const_side, C_str)) return false;

        std::vector<std::string> mvals_str;
        std::vector<typename SemiringT::value_type> kvals;
        mvals_str.reserve(children.size());
        kvals.reserve(children.size());
        for (gate_t ch : children) {
          if (c.getGateType(ch) != gate_semimod) return false;
          std::string m_str;
          gate_t k_gate{};
          if (!semimod_extract_string_and_K(c, ch, m_str, k_gate)) return false;
          mvals_str.push_back(m_str);
          kvals.push_back(c.evaluate<SemiringT>(k_gate, mapping, S));
        }

        // Only choose() is supported (the only text-valued aggregate whose
        // possible-world value is decided occurrence-by-occurrence), and
        // only = / <> are exposed for text.
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
        // first present element matches; summing the annotations of all such
        // worlds telescopes (free suffix sums to one):
        //   pw = ⊕_{i : vᵢ matches} kᵢ ⊗ (⊗_{j<i} (1 ⊖ kⱼ))
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
          auto absent = S.monus(one, kvals[i]);
          prefix = (prefix == one)
                     ? absent
                     : S.times(std::vector<typename SemiringT::value_type>{
                         prefix, absent});
        }
        pw_out = disjuncts.empty() ? S.zero() : S.plus(disjuncts);
        return true;
      }

      // ---- Numeric comparison domain (int / numeric / float): unify by
      //      scaling every value and the threshold to a common integer
      //      grid by their decimal text, so a numeric(p,d) / finite-decimal
      //      float column is evaluated exactly and fractional thresholds
      //      work.  Integer is the scale-0 case. ----
      std::string C_str;
      if (!extract_constant_string(c, const_side, C_str)) return false;
      long C_mant = 0; int C_scale = 0;
      if (!parse_decimal_scaled(C_str, C_mant, C_scale)) return false;

      std::vector<long> m_mant;
      std::vector<int> m_scale;
      std::vector<typename SemiringT::value_type> kvals;
      m_mant.reserve(children.size());
      m_scale.reserve(children.size());
      kvals.reserve(children.size());
      for (gate_t ch : children) {
        if (c.getGateType(ch) != gate_semimod) return false;
        std::string m_str;
        gate_t k_gate{};
        if (!semimod_extract_string_and_K(c, ch, m_str, k_gate)) return false;
        long mm = 0; int ms = 0;
        if (!parse_decimal_scaled(m_str, mm, ms)) return false;
        m_mant.push_back(mm);
        m_scale.push_back(ms);
        kvals.push_back(c.evaluate<SemiringT>(k_gate, mapping, S));
      }

      // COUNT(*) is SUM of unit 1s; detect on the unscaled values.
      if (agg_kind == AggregationOperator::SUM) {
        bool all_one = true;
        for (size_t i = 0; i < m_mant.size(); ++i)
          if (!(m_mant[i] == 1 && m_scale[i] == 0)) { all_one = false; break; }
        if (all_one) agg_kind = AggregationOperator::COUNT;
      }

      // Common scale: rescale every value and the threshold to integers.
      int target_scale = C_scale;
      for (int ms : m_scale) target_scale = std::max(target_scale, ms);
      long C = 0;
      if (!rescale_to(C_mant, C_scale, target_scale, C)) return false;
      std::vector<long> mvals(m_mant.size());
      for (size_t i = 0; i < m_mant.size(); ++i)
        if (!rescale_to(m_mant[i], m_scale[i], target_scale, mvals[i])) return false;

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
