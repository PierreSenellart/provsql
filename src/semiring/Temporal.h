/**
 * @file semiring/Temporal.h
 * @brief Interval-union (temporal) m-semiring over @c tstzmultirange.
 *
 * The temporal m-semiring associates each gate with a finite union of
 * pairwise-disjoint timestamp intervals (a PostgreSQL @c tstzmultirange).
 * It is the compiled counterpart of the SQL :sqlfunc:`union_tstzintervals`
 * helper, but unlike the PL/pgSQL evaluator it supports the full set
 * of circuit gate types (including @c cmp from HAVING clauses, @c agg,
 * @c semimod, and @c eq / @c project for where-provenance).
 *
 * Operations:
 * - @c zero()   → @c '{}'::tstzmultirange (empty)
 * - @c one()    → @c '{(,)}'::tstzmultirange (universal range)
 * - @c plus()   → multirange union (@c multirange_union)
 * - @c times()  → multirange intersection (@c multirange_intersect)
 * - @c monus()  → multirange set difference (@c multirange_minus)
 * - @c delta()  → @c zero() if input is empty, @c one() otherwise
 *
 * Absorptivity: `absorptive()` returns `true`. With inputs being subsets
 * of the universal range, @f$\mathbb{1} \oplus a = (-\infty,+\infty) \cup a
 * = (-\infty,+\infty) = \mathbb{1}@f$.
 *
 * @note Requires PostgreSQL 14+ (for @c tstzmultirange). Multirange
 * functions like @c multirange_union access @c fcinfo->flinfo->fn_extra
 * for type-cache lookups, so they must be invoked through
 * @c OidFunctionCall* (which builds a proper @c FmgrInfo) rather than
 * @c DirectFunctionCall* (which leaves @c flinfo NULL and would crash).
 *
 * @see https://provsql.org/lean-docs/Provenance/Semirings/IntervalUnion.html
 *      Lean 4 verified instance: @c instSemiringWithMonusIntervalUnion,
 *      with proofs of @c IntervalUnion.absorptive and
 *      @c IntervalUnion.mul_sub_left_distributive.
 */
#ifndef TEMPORAL_H
#define TEMPORAL_H

#if PG_VERSION_NUM >= 140000

extern "C" {
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/fmgroids.h"
#include "utils/multirangetypes.h"
#include "utils/lsyscache.h"
}

#include <vector>

#include "Semiring.h"

namespace semiring {
/**
 * @brief Temporal (interval-union) m-semiring with @c Datum carrier.
 *
 * Each gate evaluates to a @c tstzmultirange Datum allocated in the
 * current memory context.  The class caches function OIDs and the
 * zero/one Datum values in its constructor so that operations dispatch
 * cheaply during circuit traversal.
 */
class Temporal : public semiring::Semiring<Datum>
{
Oid in_func;
Oid typioparam;
Datum cached_zero;
Datum cached_one;

public:
Temporal() {
  getTypeInputInfo(TSTZMULTIRANGEOID, &in_func, &typioparam);
  cached_zero = OidInputFunctionCall(in_func, const_cast<char *>("{}"), typioparam, -1);
  cached_one = OidInputFunctionCall(in_func, const_cast<char *>("{(,)}"), typioparam, -1);
}

virtual value_type zero() const override {
  return cached_zero;
}
virtual value_type one() const override {
  return cached_one;
}
virtual value_type plus(const std::vector<value_type> &v) const override {
  if(v.empty()) return zero();
  Datum r = v[0];
  for(size_t i = 1; i < v.size(); ++i)
    r = OidFunctionCall2(F_MULTIRANGE_UNION, r, v[i]);
  return r;
}
virtual value_type times(const std::vector<value_type> &v) const override {
  if(v.empty()) return one();
  Datum r = v[0];
  for(size_t i = 1; i < v.size(); ++i)
    r = OidFunctionCall2(F_MULTIRANGE_INTERSECT, r, v[i]);
  return r;
}
virtual value_type monus(value_type x, value_type y) const override
{
  return OidFunctionCall2(F_MULTIRANGE_MINUS, x, y);
}
virtual value_type delta(value_type x) const override
{
  MultirangeType *mr = DatumGetMultirangeTypeP(x);
  return MultirangeIsEmpty(mr) ? zero() : one();
}
virtual bool absorptive() const override {
  return true;
}

/**
 * @brief Parse a tstzmultirange text literal to a Datum.
 *
 * Used by @c pec_temporal() to build the input mapping; exposed here
 * so the parser can share the cached @c in_func / @c typioparam.
 */
Datum parse(const char *str) const {
  return OidInputFunctionCall(in_func, const_cast<char *>(str), typioparam, -1);
}

};
}

#endif /* PG_VERSION_NUM >= 140000 */

#endif /* TEMPORAL_H */
