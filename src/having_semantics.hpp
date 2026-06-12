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
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "GenericCircuit.hpp"
#include "provsql_utils.h"
#include "subset.hpp"
#include "Aggregation.h"

/** @cond INTERNAL */
namespace provsql_having_detail {
std::vector<gate_t> collect_sp_cmp_gates(GenericCircuit &c, gate_t start);
bool extract_constant_C(GenericCircuit &c, gate_t x, int &C_out);
bool extract_constant_double(GenericCircuit &c, gate_t x, double &C_out);
bool extract_constant_string(GenericCircuit &c, gate_t x, std::string &C_out);
bool semimod_extract_M_and_K(GenericCircuit &c, gate_t semimod_gate, int &m_out, gate_t &k_gate_out);
bool semimod_extract_string_and_K(GenericCircuit &c, gate_t semimod_gate, std::string &m_out, gate_t &k_gate_out);
bool aggtype_is_text(unsigned oid);
bool aggtype_is_integer(unsigned oid);
bool aggtype_is_boolean(unsigned oid);
bool parse_array_literal(const std::string &s, std::vector<std::string> &out);
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

  // Whether the world enumeration over these contributor annotations can
  // be built *certified*: the semiring persists d-DNNF certificates
  // (circuit-building BoolExpr), every contributor is an independent
  // literal (base Bernoulli or constant) so world terms are decomposable,
  // no literal repeats (a repeat would couple two contributors), and the
  // contributor count is small enough that the complete-worlds
  // enumeration the certificate requires stays affordable.
  auto certifiable_contributors =
    [&](const std::vector<typename SemiringT::value_type> &kv) -> bool {
      if (!S.certifying())
        return false;
      if (kv.size() > 16)
        return false;
      for (size_t i = 0; i < kv.size(); ++i) {
        if (!S.independent_literal(kv[i]))
          return false;
        // Distinctness among non-constant literals (constants are
        // variable-free, so repeats of one() / zero() are harmless).
        if (kv[i] == S.one() || kv[i] == S.zero())
          continue;
        for (size_t j = 0; j < i; ++j)
          if (kv[j] == kv[i])
            return false;
      }
      return true;
    };

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
      // comparison domain (int / numeric / float / text) -- in its low 31 bits;
      // the high bit is the scalar-aggregation flag, masked off here.
      const unsigned aggtype = c.getInfos(agg_side).second & PROVSQL_AGG_TYPE_MASK;
      const bool is_scalar =
        (c.getInfos(agg_side).second & PROVSQL_AGG_SCALAR_FLAG) != 0;
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
        //
        // The disjuncts are mutually exclusive by construction (they
        // differ on the first present index): when the semiring
        // certifies enumerations, build them as certified world terms
        // under a certified deterministic OR instead.
        if (certifiable_contributors(kvals)) {
          std::vector<typename SemiringT::value_type> disjuncts;
          std::vector<typename SemiringT::value_type> before;
          for (size_t i = 0; i < kvals.size(); ++i) {
            bool match = (effective_op == ComparisonOperator::EQ)
                           ? (mvals_str[i] == C_str)
                           : (mvals_str[i] != C_str);
            if (match)
              disjuncts.push_back(S.certified_world_term(
                std::vector<typename SemiringT::value_type>{kvals[i]},
                before));
            before.push_back(kvals[i]);
          }
          pw_out = disjuncts.empty() ? S.zero()
                   : S.certified_exclusive_plus(disjuncts);
          return true;
        }
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

      // ---- Boolean comparison domain: bool_or / bool_and (and the every
      //      alias for bool_and) compared with a boolean constant.  A boolean
      //      aggregate has only two possible values, so the worlds yielding the
      //      wanted value are characterised directly in the m-semiring rather
      //      than by a 2^n enumeration.  The rows partition by their value into
      //      a class that must have at least one present member ("someE") and a
      //      class that must be wholly absent ("noneF"):
      //        bool_or  = true  : someE = true-rows                (false free)
      //        bool_or  = false : someE = false-rows, noneF = true-rows
      //        bool_and = true  : someE = true-rows,  noneF = false-rows
      //        bool_and = false : someE = false-rows               (true free)
      //      "at least one of someE present" telescopes by the first present
      //      index (the choose pattern); "none of noneF present" is a product
      //      of complements.  Non-empty groups are enforced by someE being
      //      non-empty. ----
      if (aggtype_is_boolean(aggtype)) {
        if (agg_kind != AggregationOperator::OR &&
            agg_kind != AggregationOperator::AND)
          return false;
        if (effective_op != ComparisonOperator::EQ &&
            effective_op != ComparisonOperator::NE)
          throw std::runtime_error(
            "only = and <> are supported when comparing a boolean aggregate "
            "(bool_or / bool_and / every) with a constant in HAVING");

        auto parse_bool = [](const std::string &s, bool &ok) -> bool {
          ok = true;
          if (s == "t" || s == "true"  || s == "1") return true;
          if (s == "f" || s == "false" || s == "0") return false;
          ok = false; return false;
        };

        std::string C_str;
        if (!extract_constant_string(c, const_side, C_str)) return false;
        bool okc = false;
        const bool C = parse_bool(C_str, okc);
        if (!okc) return false;
        // The aggregate value the satisfying worlds must produce.
        const bool target = (effective_op == ComparisonOperator::EQ) ? C : !C;

        std::vector<bool> vals;
        std::vector<typename SemiringT::value_type> kvals;
        vals.reserve(children.size());
        kvals.reserve(children.size());
        for (gate_t ch : children) {
          if (c.getGateType(ch) != gate_semimod) return false;
          std::string m_str;
          gate_t k_gate{};
          if (!semimod_extract_string_and_K(c, ch, m_str, k_gate)) return false;
          bool okv = false;
          const bool b = parse_bool(m_str, okv);
          if (!okv) return false;
          vals.push_back(b);
          kvals.push_back(c.evaluate<SemiringT>(k_gate, mapping, S));
        }

        const bool want_or = (agg_kind == AggregationOperator::OR);
        std::vector<size_t> someE;   // at least one of these must be present
        std::vector<size_t> noneF;   // all of these must be absent
        if (want_or == target) {
          // bool_or=true / bool_and=false: one trigger row suffices, the other
          // class is free.
          for (size_t i = 0; i < vals.size(); ++i)
            if (vals[i] == want_or) someE.push_back(i);
        } else {
          // bool_or=false / bool_and=true: the trigger class must be wholly
          // absent and the opposite class must have at least one present row.
          for (size_t i = 0; i < vals.size(); ++i)
            (vals[i] == want_or ? noneF : someE).push_back(i);
        }

        if (someE.empty()) { pw_out = S.zero(); return true; }

        if (certifiable_contributors(kvals)) {
          std::vector<typename SemiringT::value_type> disjuncts;
          std::vector<typename SemiringT::value_type> before; // someE rows before i
          for (size_t e : someE) {
            std::vector<typename SemiringT::value_type> missing = before;
            for (size_t f : noneF) missing.push_back(kvals[f]);
            disjuncts.push_back(S.certified_world_term(
              std::vector<typename SemiringT::value_type>{kvals[e]}, missing));
            before.push_back(kvals[e]);
          }
          pw_out = S.certified_exclusive_plus(disjuncts);
          return true;
        }

        const auto one = S.one();
        // "none of noneF present": product of complements.
        auto none_factor = one;
        for (size_t f : noneF) {
          auto absent = S.monus(one, kvals[f]);
          none_factor = (none_factor == one)
            ? absent
            : S.times(std::vector<typename SemiringT::value_type>{none_factor,
                                                                  absent});
        }
        // "at least one of someE present": telescope by first present index.
        std::vector<typename SemiringT::value_type> disjuncts;
        auto prefix = one;
        for (size_t i : someE) {
          if (prefix == one)
            disjuncts.push_back(kvals[i]);
          else if (kvals[i] == one)
            disjuncts.push_back(prefix);
          else
            disjuncts.push_back(S.times(
              std::vector<typename SemiringT::value_type>{kvals[i], prefix}));
          auto absent = S.monus(one, kvals[i]);
          prefix = (prefix == one)
            ? absent
            : S.times(std::vector<typename SemiringT::value_type>{prefix, absent});
        }
        auto some_value = disjuncts.empty() ? S.zero() : S.plus(disjuncts);
        if (none_factor == one)
          pw_out = some_value;
        else if (some_value == one)
          pw_out = none_factor;
        else
          pw_out = S.times(
            std::vector<typename SemiringT::value_type>{none_factor, some_value});
        return true;
      }

      // ---- Array comparison domain: array_agg(x) against a constant array.
      //      No aggregate-specific optimization (the general pipeline): scan the
      //      non-empty worlds whose ordered present elements equal (=) or differ
      //      (<>) from the constant array, then combine those worlds in the
      //      m-semiring.  Elements are compared as their text representations,
      //      so any element type works. ----
      if (agg_kind == AggregationOperator::ARRAY_AGG) {
        if (effective_op != ComparisonOperator::EQ &&
            effective_op != ComparisonOperator::NE)
          throw std::runtime_error(
            "only = and <> are supported when comparing array_agg() with a "
            "constant array in HAVING");

        std::string C_str;
        if (!extract_constant_string(c, const_side, C_str)) return false;
        std::vector<std::string> target;
        if (!parse_array_literal(C_str, target)) return false;

        std::vector<std::string> vals;
        std::vector<typename SemiringT::value_type> kvals;
        vals.reserve(children.size());
        kvals.reserve(children.size());
        for (gate_t ch : children) {
          if (c.getGateType(ch) != gate_semimod) return false;
          std::string m_str;
          gate_t k_gate{};
          if (!semimod_extract_string_and_K(c, ch, m_str, k_gate)) return false;
          vals.push_back(m_str);
          kvals.push_back(c.evaluate<SemiringT>(k_gate, mapping, S));
        }

        auto worlds = enumerate_array_agg_worlds(
          vals, target, effective_op == ComparisonOperator::EQ);
        if (worlds.empty()) { pw_out = S.zero(); return true; }

        if (certifiable_contributors(kvals)) {
          std::vector<typename SemiringT::value_type> disjuncts;
          disjuncts.reserve(worlds.size());
          for (const auto &mask : worlds) {
            std::vector<typename SemiringT::value_type> present, missing;
            for (size_t i = 0; i < kvals.size(); ++i)
              (mask[i] ? present : missing).push_back(kvals[i]);
            disjuncts.push_back(S.certified_world_term(present, missing));
          }
          pw_out = S.certified_exclusive_plus(disjuncts);
          return true;
        }

        const auto one = S.one();
        std::vector<typename SemiringT::value_type> disjuncts;
        disjuncts.reserve(worlds.size());
        for (const auto &mask : worlds) {
          std::vector<typename SemiringT::value_type> present, missing;
          for (size_t i = 0; i < kvals.size(); ++i) {
            if (mask[i]) {
              if (kvals[i] != one) present.push_back(kvals[i]);
            } else if (kvals[i] != S.zero())
              missing.push_back(kvals[i]);
          }
          auto present_prod = S.times(present);
          if (missing.empty())
            disjuncts.push_back(present_prod);
          else {
            auto term = S.monus(one, S.plus(missing));
            if (present_prod != one)
              term = S.times(std::vector<typename SemiringT::value_type>{
                present_prod, term});
            disjuncts.push_back(term);
          }
        }
        pw_out = S.plus(disjuncts);
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

      // A certified enumeration needs the *complete* valid worlds (every
      // contributor present or explicitly negated): the upset shortcut
      // and the monotone MIN/MAX skips below produce overlapping,
      // non-exclusive disjuncts, sound for absorptive evaluation but
      // unmarkable.  When certifying, request the full enumeration --
      // the same one non-absorptive semirings already use.
      const bool certify = certifiable_contributors(kvals);

      bool upset = false;
      auto worlds = enumerate_valid_worlds(mvals, C, effective_op, agg_kind,
                                           certify ? false : S.absorptive(),
                                           upset, is_scalar);

      if (worlds.empty()) {
        pw_out = S.zero();
        return true;
      }

      if (certify) {
        std::vector<typename SemiringT::value_type> disjuncts;
        disjuncts.reserve(worlds.size());
        for (const auto &mask : worlds) {
          std::vector<typename SemiringT::value_type> present, missing;
          for (size_t i = 0; i < kvals.size(); ++i)
            (mask[i] ? present : missing).push_back(kvals[i]);
          disjuncts.push_back(S.certified_world_term(present, missing));
        }
        pw_out = S.certified_exclusive_plus(disjuncts);
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

    // General possible-worlds evaluation for a comparison over an *arithmetic
    // expression of one or more aggregates* (sum(x)*sum(y) > k, sum(x) > sum(y),
    // 100/sum(x) > 5, ...), which the single-aggregate path above cannot fold.
    // We enumerate the joint possible worlds over the union of the aggregates'
    // contributors, evaluate the arithmetic numerically in each world, and
    // combine the worlds where the comparison holds in the semiring exactly as
    // the single-aggregate path does (present_prod ⊗ (1 ⊖ missing_sum)).  This
    // is exponential in the number of distinct contributors, so it bails out
    // (returning false, leaving the gate unresolved) beyond a small bound.
    auto build_general = [&](gate_t Lx, gate_t Rx,
                             ComparisonOperator opx) -> bool {
      struct AggInfo {
        AggregationOperator kind;
        bool is_int;                                    // integer-typed result?
        std::vector<std::pair<int, double> > contribs;  // (distinct-K index, value)
      };
      // A decimal text denotes an integer iff it has no fractional/exponent part.
      auto text_is_int = [](const std::string &s) -> bool {
        if (s.empty()) return false;
        for (char ch : s)
          if (!((ch >= '0' && ch <= '9') || ch == '+' || ch == '-'))
            return false;
        return true;
      };
      std::map<gate_t, AggInfo> aggs;
      std::map<gate_t, int> kindex;
      std::vector<gate_t> kgates;

      std::function<bool(gate_t)> collect = [&](gate_t gx) -> bool {
        gate_type gt = c.getGateType(gx);
        if (gt == gate_agg) {
          if (aggs.count(gx))
            return true;
          AggInfo ai;
          ai.kind = getAggregationOperator(c.getInfos(gx).first);
          ai.is_int = aggtype_is_integer(c.getInfos(gx).second & PROVSQL_AGG_TYPE_MASK);
          for (gate_t ch : c.getWires(gx)) {
            if (c.getGateType(ch) != gate_semimod)
              return false;
            std::string ms;
            gate_t kg{};
            if (!semimod_extract_string_and_K(c, ch, ms, kg))
              return false;
            int idx;
            auto it = kindex.find(kg);
            if (it == kindex.end()) {
              idx = static_cast<int>(kgates.size());
              kindex[kg] = idx;
              kgates.push_back(kg);
            } else
              idx = it->second;
            double m;
            try { m = std::stod(ms); } catch (...) { return false; }
            ai.contribs.emplace_back(idx, m);
          }
          aggs.emplace(gx, std::move(ai));
          return true;
        }
        if (gt == gate_arith) {
          for (gate_t ch : c.getWires(gx))
            if (!collect(ch))
              return false;
          return true;
        }
        if (gt == gate_value)
          return true;
        if (gt == gate_semimod) {       // a constant threshold: semimod(1, value)
          std::string ms;
          gate_t kg{};
          if (!semimod_extract_string_and_K(c, gx, ms, kg))
            return false;
          return c.getGateType(kg) == gate_one;
        }
        return false;
      };

      if (!collect(Lx) || !collect(Rx))
        return false;
      if (aggs.empty())
        return false;
      const size_t n = kgates.size();
      if (n == 0 || n > 24)            // 2^n enumeration: keep it bounded
        return false;

      // Numeric value of a subexpression in a given world, tracking whether it
      // is integer-valued so that division floors as SQL does (NULL -> false).
      std::function<bool(gate_t, uint64_t, double &, bool &)> eval =
        [&](gate_t gx, uint64_t world, double &out, bool &is_int) -> bool {
        gate_type gt = c.getGateType(gx);
        if (gt == gate_value) {
          std::string s = c.getExtra(gx);
          try { out = std::stod(s); } catch (...) { return false; }
          is_int = text_is_int(s);
          return true;
        }
        if (gt == gate_semimod) {       // constant threshold
          std::string ms; gate_t kg{};
          if (!semimod_extract_string_and_K(c, gx, ms, kg)) return false;
          try { out = std::stod(ms); } catch (...) { return false; }
          is_int = text_is_int(ms);
          return true;
        }
        if (gt == gate_agg) {
          const AggInfo &ai = aggs.at(gx);
          double acc = 0, mn = 0, mx = 0;
          long cnt = 0;
          bool first = true;
          for (const auto &pr : ai.contribs)
            if (world & (uint64_t(1) << pr.first)) {
              double m = pr.second;
              acc += m; ++cnt;
              if (first) { mn = mx = m; first = false; }
              else { mn = std::min(mn, m); mx = std::max(mx, m); }
            }
          is_int = ai.is_int;
          switch (ai.kind) {
          case AggregationOperator::SUM:   out = acc; return true;
          case AggregationOperator::COUNT: out = acc; return true;  // values 1 or 0/1
          case AggregationOperator::AVG:   if (cnt == 0) return false; out = acc / cnt; return true;
          case AggregationOperator::MIN:   if (cnt == 0) return false; out = mn; return true;
          case AggregationOperator::MAX:   if (cnt == 0) return false; out = mx; return true;
          default: return false;
          }
        }
        if (gt == gate_arith) {
          const auto &w = c.getWires(gx);
          unsigned aop = static_cast<unsigned>(c.getInfos(gx).first);
          if (aop == PROVSQL_ARITH_PLUS || aop == PROVSQL_ARITH_TIMES) {
            double r = (aop == PROVSQL_ARITH_PLUS) ? 0 : 1;
            bool all_int = true;
            for (gate_t ch : w) {
              double v; bool vi;
              if (!eval(ch, world, v, vi)) return false;
              if (aop == PROVSQL_ARITH_PLUS) r += v; else r *= v;
              all_int = all_int && vi;
            }
            out = r; is_int = all_int; return true;
          }
          if (aop == PROVSQL_ARITH_MINUS) {
            if (w.size() != 2) return false;
            double a, b; bool ai, bi;
            if (!eval(w[0], world, a, ai) || !eval(w[1], world, b, bi)) return false;
            out = a - b; is_int = ai && bi; return true;
          }
          if (aop == PROVSQL_ARITH_DIV) {
            if (w.size() != 2) return false;
            double a, b; bool ai, bi;
            if (!eval(w[0], world, a, ai) || !eval(w[1], world, b, bi)) return false;
            if (b == 0) return false;
            if (ai && bi) {     // SQL integer division truncates toward zero
              out = static_cast<double>(static_cast<long long>(a) /
                                        static_cast<long long>(b));
              is_int = true;
            } else {
              out = a / b; is_int = false;
            }
            return true;
          }
          if (aop == PROVSQL_ARITH_NEG) {
            if (w.size() != 1) return false;
            double a; bool ai;
            if (!eval(w[0], world, a, ai)) return false;
            out = -a; is_int = ai; return true;
          }
          return false;
        }
        return false;
      };

      std::vector<typename SemiringT::value_type> kval(n);
      for (size_t i = 0; i < n; ++i)
        kval[i] = c.evaluate<SemiringT>(kgates[i], mapping, S);

      // The joint enumeration below is over complete worlds already:
      // certify the disjuncts when the semiring and contributors allow.
      const bool certify = certifiable_contributors(kval);

      std::vector<typename SemiringT::value_type> disjuncts;
      const uint64_t total = uint64_t(1) << n;
      for (uint64_t world = 1; world < total; ++world) {  // skip the empty world
        double lv, rv;
        bool lint, rint;
        if (!eval(Lx, world, lv, lint) || !eval(Rx, world, rv, rint))
          continue;                                       // NULL comparison: false
        bool holds = false;
        switch (opx) {
        case ComparisonOperator::EQ: holds = (lv == rv); break;
        case ComparisonOperator::NE: holds = (lv != rv); break;
        case ComparisonOperator::LT: holds = (lv <  rv); break;
        case ComparisonOperator::LE: holds = (lv <= rv); break;
        case ComparisonOperator::GT: holds = (lv >  rv); break;
        case ComparisonOperator::GE: holds = (lv >= rv); break;
        }
        if (!holds)
          continue;

        std::vector<typename SemiringT::value_type> present, missing;
        for (size_t i = 0; i < n; ++i) {
          if (world & (uint64_t(1) << i)) {
            if (certify || kval[i] != S.one()) present.push_back(kval[i]);
          } else {
            if (certify || kval[i] != S.zero()) missing.push_back(kval[i]);
          }
        }
        if (certify) {
          disjuncts.push_back(S.certified_world_term(present, missing));
          continue;
        }
        auto present_prod = S.times(present);
        if (missing.empty())
          disjuncts.push_back(std::move(present_prod));
        else {
          auto monus_factor = S.monus(S.one(), S.plus(missing));
          disjuncts.push_back(
            present_prod == S.one()
              ? monus_factor
              : S.times(std::vector<typename SemiringT::value_type>{
                  present_prod, monus_factor}));
        }
      }

      pw_out = disjuncts.empty() ? S.zero()
               : certify ? S.certified_exclusive_plus(disjuncts)
               : S.plus(disjuncts);
      return true;
    };

    if (c.getGateType(L) == gate_agg && build_from(L, R, op))
      return true;
    if (c.getGateType(R) == gate_agg && build_from(R, L, flip_op(op)))
      return true;

    return build_general(L, R, op);
  };

  for (gate_t cmp_gate : cmp_gates) {
    typename SemiringT::value_type pw;
    if (!pw_from_cmp_gate(cmp_gate, pw))
      return;

    mapping[cmp_gate] = std::move(pw);
  }
}

#endif
