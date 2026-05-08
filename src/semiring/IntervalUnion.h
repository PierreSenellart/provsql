/**
 * @file semiring/IntervalUnion.h
 * @brief Interval-union m-semiring over PostgreSQL @c multirange types.
 *
 * The interval-union m-semiring associates each gate with a finite
 * union of pairwise-disjoint intervals over a densely-ordered linear
 * domain (timestamp, numeric, integer, ...).  Addition is multirange
 * union, multiplication is intersection, and monus is set difference.
 * The class is parameterised by a multirange type OID, so a single
 * implementation covers @c tstzmultirange (the temporal instance),
 * @c nummultirange (numeric validity ranges), and @c int4multirange
 * (integer page/line ranges) — and any other multirange type the user
 * happens to define.
 *
 * Operations:
 * - @c zero()   → <tt>'{}'</tt> (empty multirange)
 * - @c one()    → <tt>'{(,)}'</tt> (universal multirange)
 * - @c plus()   → multirange union (@c multirange_union)
 * - @c times()  → multirange intersection (@c multirange_intersect)
 * - @c monus()  → multirange set difference (@c multirange_minus)
 * - @c delta()  → identity (preserves the supporting multirange so that
 *                 aggregated groups carry the actual time/parameter region
 *                 of contributing rows rather than the universal range)
 *
 * Absorptivity: `absorptive()` returns `true`. With inputs being subsets
 * of the universal range, @f$\mathbb{1} \oplus a = (-\infty,+\infty) \cup a
 * = (-\infty,+\infty) = \mathbb{1}@f$.
 *
 * @note Requires PostgreSQL 14+ (multirange types). The polymorphic
 * built-ins @c F_MULTIRANGE_UNION / @c F_MULTIRANGE_INTERSECT /
 * @c F_MULTIRANGE_MINUS work for every multirange carrier and access
 * @c fcinfo->flinfo->fn_extra for type-cache lookups, so they must be
 * invoked through @c OidFunctionCall* (which builds a proper
 * @c FmgrInfo) rather than @c DirectFunctionCall* (which leaves
 * @c flinfo NULL and would crash).
 *
 * @see https://provsql.org/lean-docs/Provenance/Semirings/IntervalUnion.html
 *      Lean 4 verified instance: @c instSemiringWithMonusIntervalUnion,
 *      with proofs of @c IntervalUnion.absorptive and
 *      @c IntervalUnion.mul_sub_left_distributive.
 */
#ifndef INTERVAL_UNION_H
#define INTERVAL_UNION_H

#if PG_VERSION_NUM >= 140000 || defined(DOXYGEN)

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
 * @brief Interval-union m-semiring with @c Datum carrier, parameterised
 * by a multirange type OID.
 *
 * Each gate evaluates to a multirange Datum allocated in the current
 * memory context.  The class caches function OIDs and the zero/one
 * Datum values in its constructor so that operations dispatch cheaply
 * during circuit traversal.
 */
class IntervalUnion : public semiring::Semiring<Datum>
{
Oid in_func;
Oid typioparam;
Datum cached_zero;
Datum cached_one;

public:
explicit IntervalUnion(Oid multirange_oid) {
  getTypeInputInfo(multirange_oid, &in_func, &typioparam);
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
  return x;
}
virtual bool absorptive() const override {
  return true;
}

/**
 * @brief Parse a multirange text literal to a Datum.
 *
 * Used by @c pec_multirange() to build the input mapping; exposed here
 * so the parser can share the cached @c in_func / @c typioparam.
 */
Datum parse_leaf(const char *str) const {
  return OidInputFunctionCall(in_func, const_cast<char *>(str), typioparam, -1);
}

};
}

#endif /* PG_VERSION_NUM >= 140000 || defined(DOXYGEN) */

#endif /* INTERVAL_UNION_H */
