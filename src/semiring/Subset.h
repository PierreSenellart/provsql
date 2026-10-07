/**
 * @file semiring/Subset.h
 * @brief The Boolean algebra of the subsets of a PostgreSQL @c enum type.
 *
 * An annotation is a set of labels of a user enum (at most 63 of them),
 * stored as a bitmask: bit @f$i@f$ stands for the label of rank @f$i@f$ in
 * @c pg_enum.enumsortorder.  The operations are those of the powerset
 * Boolean algebra:
 * - @c zero()  → the empty set
 * - @c one()   → every label (and, for consent, the unrestricted element)
 * - @c plus()  → union
 * - @c times() → intersection
 * - @c monus() → set difference
 * - @c delta() → identity
 *
 * Evaluating a circuit here is evaluating it in every one of the worlds the
 * labels stand for at once: the annotation of a result is the set of labels
 * whose world contains it.  Three front-ends read the labels differently,
 * selected by @c Mode:
 * - @c SUBSET: a mapping value is a set of labels (an enum array), or a
 *   single label standing for itself;
 * - @c CLEARANCE: the labels are clearance levels in increasing order, and a
 *   single label stands for itself and every level above it (the levels at
 *   which a tuple of that clearance may be seen); an array is any set
 *   (compartments);
 * - @c CONSENT: the labels are purposes, a mapping value is the set of
 *   purposes a tuple is consented for, and every tuple also belongs to the
 *   unrestricted world, an extra element (bit 63) standing for the whole
 *   database.
 *
 * Being a Boolean algebra, the semiring is absorptive, idempotent for both
 * operations, exclusive, and its product distributes over its monus: every
 * property the evaluators look for holds, so negation (@c EXCEPT,
 * @c NOT @c EXISTS) gets its exact reading, which a single level
 * (@c MinMax) cannot represent.
 */
#ifndef SUBSET_H
#define SUBSET_H

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/catcache.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "access/htup_details.h"
}

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Semiring.h"

namespace semiring {

/** @brief The labels of an enum type, in @c enumsortorder: their OIDs, and
 *         the rank of each OID. */
struct EnumLabels {
  std::vector<Oid> oids;                  ///< label OIDs, by rank
  std::unordered_map<Oid, unsigned> rank; ///< rank of each label OID

  explicit EnumLabels(Oid enum_oid) {
    CatCList *list = SearchSysCacheList1(ENUMTYPOIDNAME, ObjectIdGetDatum(enum_oid));
    std::vector<std::pair<float4, Oid> > labels;
    for(int i = 0; i < list->n_members; ++i) {
      HeapTuple tup = &list->members[i]->tuple;
      Form_pg_enum en = (Form_pg_enum) GETSTRUCT(tup);
#if PG_VERSION_NUM >= 120000
      Oid label_oid = en->oid;
#else
      Oid label_oid = HeapTupleGetOid(tup);
#endif
      labels.emplace_back(en->enumsortorder, label_oid);
    }
    ReleaseCatCacheList(list);
    std::sort(labels.begin(), labels.end());
    for(const auto &l : labels) {
      rank.emplace(l.second, oids.size());
      oids.push_back(l.second);
    }
  }
};

/**
 * @brief Subsets of the labels of an enum type, as a 64-bit mask.
 */
class Subset : public semiring::Semiring<uint64_t>
{
public:
/** @brief How a mapping value is read (see the file comment). */
enum class Mode { SUBSET, CLEARANCE, CONSENT };

/** @brief The bit of the unrestricted world, in @c CONSENT mode. */
static constexpr uint64_t UNRESTRICTED = uint64_t(1) << 63;

private:
Oid enum_oid;
Mode mode;
EnumLabels labels;
uint64_t universe;
Oid in_func, array_in_func, typioparam, array_typioparam;

uint64_t bit_of(Oid label) const {
  auto it = labels.rank.find(label);
  if(it == labels.rank.end())
    throw std::runtime_error("Subset: value is not a label of the enum type");
  return uint64_t(1) << it->second;
}

public:
Subset(Oid enum_type_oid, Mode mode_)
  : enum_oid(enum_type_oid), mode(mode_), labels(enum_type_oid)
{
  const std::size_t n = labels.oids.size();
  if(n == 0)
    throw std::runtime_error("Subset: enum type has no labels");
  if(n > 63)
    throw std::runtime_error("Subset: an enum type of at most 63 labels is "
                             "supported, one bit per label");
  universe = n == 64 ? ~uint64_t(0) : (uint64_t(1) << n) - 1;
  if(mode == Mode::CONSENT)
    universe |= UNRESTRICTED;

  getTypeInputInfo(enum_oid, &in_func, &typioparam);
  Oid array_type = get_array_type(enum_oid);
  if(!OidIsValid(array_type))
    throw std::runtime_error("Subset: enum type has no array type");
  getTypeInputInfo(array_type, &array_in_func, &array_typioparam);
}

/** @brief The labels, in @c enumsortorder. */
const std::vector<Oid> &label_oids() const {
  return labels.oids;
}

virtual value_type zero() const override {
  return 0;
}
virtual value_type one() const override {
  return universe;
}
virtual value_type plus(const std::vector<value_type> &v) const override {
  value_type r = 0;
  for(auto x : v) r |= x;
  return r;
}
virtual value_type times(const std::vector<value_type> &v) const override {
  value_type r = universe;
  for(auto x : v) r &= x;
  return r;
}
virtual value_type monus(value_type x, value_type y) const override {
  return x & ~y;
}
virtual value_type delta(value_type x) const override {
  return x;
}
virtual bool absorptive() const override {
  return true;
}
virtual bool exclusive() const override {
  return true;
}
virtual bool mul_idempotent() const override {
  return true;
}
virtual bool mul_sub_left_distributive() const override {
  return true;
}
/** @brief A Boolean algebra receives a homomorphism from the Boolean
 *  functions, so the safe-query rewriting is sound here. */
virtual bool compatibleWithBooleanRewrite() const override {
  return true;
}

/**
 * @brief Parse a mapping value: a single label, or an array of labels.
 *
 * In @c CLEARANCE mode a single label stands for itself and every label
 * above it; in @c CONSENT mode the unrestricted element is added to every
 * value.
 */
value_type parse_leaf(const char *str) const {
  uint64_t r = 0;
  if(str[0] == '{') {
    Datum arr = OidInputFunctionCall(array_in_func, const_cast<char *>(str),
                                     array_typioparam, -1);
    ArrayType *a = DatumGetArrayTypeP(arr);
    Datum *elems;
    bool *nulls;
    int nelems;
    int16 typlen;
    bool typbyval;
    char typalign;
    get_typlenbyvalalign(enum_oid, &typlen, &typbyval, &typalign);
    deconstruct_array(a, enum_oid, typlen, typbyval, typalign,
                      &elems, &nulls, &nelems);
    for(int i = 0; i < nelems; ++i)
      if(!nulls[i])
        r |= bit_of(DatumGetObjectId(elems[i]));
  } else {
    Oid label = DatumGetObjectId(
      OidInputFunctionCall(in_func, const_cast<char *>(str), typioparam, -1));
    r = bit_of(label);
    if(mode == Mode::CLEARANCE)
      r = (universe & ~(r - 1));   // this level and every one above
  }
  if(mode == Mode::CONSENT)
    r |= UNRESTRICTED;
  return r;
}

};

}

#endif /* SUBSET_H */
