/**
 * @file semiring/MinMax.h
 * @brief Min-max and max-min m-semirings over PostgreSQL @c enum types.
 *
 * The min-max m-semiring associates each gate with an enum value drawn
 * from a bounded linear order (the @c pg_enum.enumsortorder of the
 * carrier enum type).  Two instantiations are provided through a single
 * class:
 *
 * - @b MinMax (@c reverse @c = @c false): @f$\oplus = \min@f$,
 *   @f$\otimes = \max@f$, @f$\mathbb{0} = \top@f$, @f$\mathbb{1} =
 *   \bot@f$.  This is the security / access-control shape: alternative
 *   derivations combine to the *least* sensitive label, and joins
 *   combine to the *most* sensitive label.
 *
 * - @b MaxMin (@c reverse @c = @c true): @f$\oplus = \max@f$,
 *   @f$\otimes = \min@f$, @f$\mathbb{0} = \bot@f$, @f$\mathbb{1} =
 *   \top@f$.  This is the fuzzy / availability / trust-level shape:
 *   alternatives combine to the *most* permissive label, and joins
 *   combine to the *strictest* label.
 *
 * Operations (described for @c MinMax; @c MaxMin is symmetric):
 * - @c zero()   → @f$\top@f$ (largest enum value by sortorder)
 * - @c one()    → @f$\bot@f$ (smallest enum value by sortorder)
 * - @c plus()   → enum-min over operands (empty list → @c zero())
 * - @c times()  → enum-max over operands (empty list → @c one())
 * - @c monus()  → @f$\mathbb{0}@f$ if @f$x \le_S y@f$, else @f$x@f$,
 *                 where @f$\le_S@f$ is the semiring order
 * - @c delta()  → @c zero() if @c x equals @c zero(), @c one() otherwise
 *
 * Absorptivity: `absorptive()` returns `true`. With inputs in the
 * bounded order, @f$\mathbb{1} \oplus a = a \oplus \mathbb{1} =
 * \mathbb{1}@f$ in both instantiations.
 *
 * @note Bottom and top are looked up once in the constructor from
 * @c pg_enum.enumsortorder via the @c ENUMTYPOIDNAME syscache.  Element
 * comparison goes through @c F_ENUM_CMP via @c OidFunctionCall2, which
 * handles the typecache lookup internally.
 *
 * @see https://provsql.org/lean-docs/Provenance/Semirings/MinMax.html
 *      Lean 4 verified instance: @c instSemiringWithMonusMinMax
 *      (and @c MaxMin = @c MinMax(OrderDual)).
 */
#ifndef MIN_MAX_H
#define MIN_MAX_H

extern "C" {
#include "fmgr.h"
#include "catalog/pg_enum.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "access/htup_details.h"
}

#include <vector>
#include <stdexcept>

#include "Semiring.h"

namespace semiring {
/**
 * @brief Min-max / max-min m-semiring with @c Datum carrier over a
 * PostgreSQL enum type.
 *
 * A single class covers both the @b MinMax (@c reverse @c = @c false,
 * the security shape) and the @b MaxMin (@c reverse @c = @c true, the
 * fuzzy / trust shape) instantiations: the two are related by order
 * reversal on the same carrier.  Bottom and top of the carrier enum are
 * looked up once in the constructor and cached as @c bottom_datum /
 * @c top_datum.
 */
class MinMax : public semiring::Semiring<Datum>
{
Oid enum_oid;
bool reverse;
Datum bottom_datum;
Datum top_datum;
Oid in_func;
Oid typioparam;

static int enum_cmp_datum(Datum a, Datum b) {
  return DatumGetInt32(OidFunctionCall2(F_ENUM_CMP, a, b));
}

public:
explicit MinMax(Oid enum_type_oid, bool reverse_)
  : enum_oid(enum_type_oid), reverse(reverse_)
{
  CatCList *list = SearchSysCacheList1(ENUMTYPOIDNAME, ObjectIdGetDatum(enum_oid));
  if(list->n_members == 0) {
    ReleaseCatCacheList(list);
    throw std::runtime_error("MinMax: enum type has no members");
  }
  Oid b_oid = InvalidOid, t_oid = InvalidOid;
  float4 b_order = 0, t_order = 0;
  for(int i = 0; i < list->n_members; ++i) {
    HeapTuple tup = &list->members[i]->tuple;
    Form_pg_enum en = (Form_pg_enum) GETSTRUCT(tup);
#if PG_VERSION_NUM >= 120000
    Oid label_oid = en->oid;
#else
    Oid label_oid = HeapTupleGetOid(tup);
#endif
    if(i == 0 || en->enumsortorder < b_order) {
      b_order = en->enumsortorder;
      b_oid = label_oid;
    }
    if(i == 0 || en->enumsortorder > t_order) {
      t_order = en->enumsortorder;
      t_oid = label_oid;
    }
  }
  ReleaseCatCacheList(list);

  bottom_datum = ObjectIdGetDatum(b_oid);
  top_datum = ObjectIdGetDatum(t_oid);

  getTypeInputInfo(enum_oid, &in_func, &typioparam);
}

virtual value_type zero() const override {
  return reverse ? bottom_datum : top_datum;
}
virtual value_type one() const override {
  return reverse ? top_datum : bottom_datum;
}
virtual value_type plus(const std::vector<value_type> &v) const override {
  if(v.empty()) return zero();
  Datum r = v[0];
  for(size_t i = 1; i < v.size(); ++i) {
    int c = enum_cmp_datum(v[i], r);
    if((!reverse && c < 0) || (reverse && c > 0))
      r = v[i];
  }
  return r;
}
virtual value_type times(const std::vector<value_type> &v) const override {
  if(v.empty()) return one();
  Datum r = v[0];
  for(size_t i = 1; i < v.size(); ++i) {
    int c = enum_cmp_datum(v[i], r);
    if((!reverse && c > 0) || (reverse && c < 0))
      r = v[i];
  }
  return r;
}
virtual value_type monus(value_type x, value_type y) const override
{
  // x ⊖ y = 0 if x ≤_S y, else x. The semiring order ≤_S satisfies
  //   x ≤_S y iff x ⊕ y = y. For MinMax (⊕ = enum-min), this is
  //   x ≥ y in the enum order; for MaxMin (⊕ = enum-max), it is
  //   x ≤ y in the enum order.
  int c = enum_cmp_datum(x, y);
  bool x_le_S_y = reverse ? (c <= 0) : (c >= 0);
  return x_le_S_y ? zero() : x;
}
virtual value_type delta(value_type x) const override
{
  return x == zero() ? zero() : one();
}
virtual bool absorptive() const override {
  return true;
}

/**
 * @brief Parse an enum text literal to a Datum.
 *
 * Used by @c pec_anyenum() to build the input mapping; exposed here so
 * the parser can share the cached @c in_func / @c typioparam.
 */
Datum parse(const char *str) const {
  return OidInputFunctionCall(in_func, const_cast<char *>(str), typioparam, -1);
}

};
}

#endif /* MIN_MAX_H */
