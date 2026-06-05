/**
 * @file subset.cpp
 * @brief Valid-world enumeration for aggregate HAVING predicates.
 *
 * Implements @c enumerate_valid_worlds() declared in @c subset.hpp.
 *
 * For a list of @f$n@f$ tuples with individual values, the function
 * iterates over all @f$2^n@f$ possible worlds (bitmasks), computes the
 * aggregate of the present tuples' values using the requested
 * @c AggregationOperator, and tests the comparison predicate.  All
 * valid worlds are collected and returned.
 *
 * The @c upset output flag is set to @c true when the set of valid
 * worlds is upward-closed (every superset of a valid world is also
 * valid), which is the case for monotone aggregation predicates (e.g.
 * SUM ≥ k).  This information is used to optimise the evaluation of
 * monotone HAVING clauses.
 *
 * Internal helpers in an anonymous namespace:
 * - @c increment(): advance a bitmask to the next possible world.
 * - @c compute_agg(): compute the aggregate value for one bitmask.
 */
#include "subset.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>
#include <stdexcept>
#include <cassert>

namespace {
static bool increment(mask_t &v)
{
  for(size_t i=0; i<v.size(); ++i)
  {
    v[i]=!v[i];
    if(v[i])
      return true;
  }
  return false;
}

static std::vector<mask_t> all_worlds(const std::vector<long> &values)
{
  std::vector<mask_t> worlds;
  mask_t mask(values.size());
  // Skip empty world
  while(increment(mask))
    worlds.push_back(mask);
  return worlds;
}

static void append_range(std::vector<mask_t> &out,
                         const std::vector<std::vector<mask_t> > &dp,
                         long long lo,
                         long long hi)
{
  if (dp.empty()) return;
  const long long J = static_cast<long long>(dp.size())-1;
  lo = std::max(lo, 0LL);
  hi = std::min(hi, J);
  if (lo>hi) return;

  for (long long j = lo; j <= hi; ++j) {
    out.insert(out.end(), dp[j].begin(), dp[j].end());
  }
}

class DPException : public std::exception {};

/** @brief Return the minimum of two values. */
#define MIN(x,y) ((x)<(y)?(x):(y))

static std::vector<mask_t> sum_dp(const std::vector<long> &values, long C, ComparisonOperator op, bool absorptive, bool &upset, bool keep_empty=false)
{
  const std::size_t n = values.size();

  std::vector<mask_t> R;

  // We first deal with NEQ by combining LT and GT
  if(op == ComparisonOperator::NE) {
    std::vector<mask_t> lt= sum_dp(values, C, ComparisonOperator::LT, absorptive, upset, keep_empty);
    std::vector<mask_t> gt= sum_dp(values, C, ComparisonOperator::GT, absorptive, upset, keep_empty);
    R.reserve(lt.size()+gt.size());
    R.insert(R.end(),lt.begin(),lt.end());
    R.insert(R.end(),gt.begin(),gt.end());
    return R;
  }

  long long T=0;
  for (long w: values) {
    if (w < 0)
      throw DPException();
    T+=w;
  }

  //no valid worlds case
  if (op == ComparisonOperator::GT && C>=T) return {};
  if (op == ComparisonOperator::GE && C>T) return {};
  if (op == ComparisonOperator::LT && C<=0) return {};
  if (op == ComparisonOperator::LE && C<0) return {};
  if (op == ComparisonOperator::EQ && (C>T || C<0)) return {};

  //tautology cases
  if (op == ComparisonOperator::GT && C<0) return all_worlds(values);
  if (op == ComparisonOperator::GE && C<=0) return all_worlds(values);
  if (op == ComparisonOperator::LT && C>T) return all_worlds(values);
  if (op == ComparisonOperator::LE && C>=T) return all_worlds(values);

  long long J=0;
  if (op == ComparisonOperator::GT || op == ComparisonOperator::GE)
    J=T;
  else if (op==ComparisonOperator::LT)
    J=MIN(C-1,T);
  else
    J=MIN(C,T);

  assert(J>=0);

  // The DP is pseudo-polynomial: it allocates one bucket per integer in
  // [0, J].  A large J (huge aggregate values, or a high-scale decimal grid)
  // would make that array impractical -- bail out so the caller falls back to
  // exact subset enumeration, which is magnitude-independent.
  static const long long SUM_DP_MAX_J = 10000000LL;
  if (J > SUM_DP_MAX_J)
    throw DPException();

  std::vector<std::vector<mask_t> > dp(static_cast<std::size_t>(J) + 1);
  dp[0].push_back(mask_t(n)); // dp[0] <- {emptyset}

  long long pref_sum=0;

  for (std::size_t i=0; i<n; ++i)
  {
    const long w=values[i];
    pref_sum+=w;
    const long long j_max=MIN(J,pref_sum);

    for (long long j = j_max; j >= w; --j) {
      const long long p = j - w;
      if(absorptive && ((op==ComparisonOperator::GT && p>C) ||
                        (op==ComparisonOperator::GE && p>=C))) {
        upset=true;
        continue;
      }
      size_t s=dp[p].size();
      for(size_t k=0; k<s; ++k) {
        mask_t m = dp[p][k];
        m[i] = true;
        dp[j].push_back(m);
      }
    }
  }

  switch(op){
  case ComparisonOperator::EQ:
    append_range(R,dp,C,C);
    break;

  case ComparisonOperator::GT:
    append_range(R,dp,C+1,J);
    break;


  case ComparisonOperator::LT:
    append_range(R,dp,0,C-1);
    break;

  case ComparisonOperator::GE:
    append_range(R,dp,C,J);
    break;

  case ComparisonOperator::LE:
    append_range(R,dp,0,C);
    break;

  case ComparisonOperator::NE: // case already processed
    assert(false);
  }

  // dp[0] holds the empty world together with every non-empty world whose
  // present tuples all sum to 0 (value 0 being SUM's additive identity).  A
  // HAVING predicate is never satisfied by the empty group, but those value-0
  // worlds are legitimate non-empty worlds and must be kept -- cf. the ValidSum
  // proof, which removes only the single world {emptyset} from the selected
  // range.  So the <, <= (and =, when C=0) ranges above include dp[0], and we
  // drop only the all-absent mask here.  Skipping all of dp[0] instead (the old
  // `lo=1`) silently dropped, e.g., a BID-block choice of a value-0 tuple under
  // `sum < k`.
  R.erase(std::remove_if(R.begin(), R.end(),
            [](const mask_t &m) {
              for(size_t i=0; i<m.size(); ++i) if(m[i]) return false;
              return true;
            }),
          R.end());

  // keep_empty marks a SCALAR COUNT enumerated through this value-aware DP
  // (count(col) with NULL contributors, whose 0/1 values make COUNT a SUM of
  // indicators).  Unlike a genuine SUM -- empty group SQL NULL, so the empty
  // world never satisfies -- a COUNT's empty group has the real value 0, so the
  // all-absent world is a legitimate possible world: re-add it exactly when 0
  // satisfies the predicate (a true-on-empty bound: = 0, < k, <= k, <> k!=0).
  if (keep_empty) {
    bool zero_sat = false;
    switch(op) {
    case ComparisonOperator::EQ: zero_sat = (C == 0); break;
    case ComparisonOperator::NE: zero_sat = (C != 0); break;
    case ComparisonOperator::LT: zero_sat = (0 <  C); break;
    case ComparisonOperator::LE: zero_sat = (0 <= C); break;
    case ComparisonOperator::GT: zero_sat = (0 >  C); break;
    case ComparisonOperator::GE: zero_sat = (0 >= C); break;
    }
    if (zero_sat)
      R.push_back(mask_t(n));
  }

  return R;
}

//generate k-subsets form an n-set
static void combinations(std::size_t start,
                         int k_left,
                         mask_t mask,
                         std::vector<mask_t> &out)
{
  const size_t n = mask.size();

  if (k_left == 0) {
    out.push_back(mask);
    return;
  }

  if (start >= n) return;

  const std::size_t remaining = n - start;
  if (remaining < static_cast<std::size_t>(k_left)) return;

  combinations(start + 1, k_left, mask, out);

  mask[start]=true;
  combinations(start + 1, k_left - 1, mask, out);
}

static std::vector<mask_t> count_enum(const std::vector<long> &values, long m, ComparisonOperator op, bool absorptive, bool &upset, bool is_scalar)
{
  const int n = static_cast<int>(values.size());
  std::vector<mask_t> out;

  auto add_exact_k = [&](long k) {
                       if (k < 0 || k > n) return;
                       combinations(0, static_cast<int>(k), mask_t(n), out);
                     };

  /* The lowest count a group can have: 1 for a grouped aggregate (the empty
   * group is no row, so a HAVING predicate is never evaluated on it -- the
   * count >= 0 / count > -K family collapses to "non-empty" rather than to a
   * tautology), but 0 for a SCALAR aggregate (no GROUP BY), whose single result
   * row always exists with the empty input contributing count 0.  Folding the
   * empty world into each branch's bound (rather than appending it afterwards)
   * keeps it consistent with the upset / minimal-witness structure: for the GE
   * upset, count >= 0 then has minimal witness {} and is correctly a tautology;
   * the empty subset is annotated by the caller as one ⊗ (𝟙 ⊖ ⊕(tuples)). */
  const long lo = is_scalar ? 0 : 1;

  switch (op)
  {
  case ComparisonOperator::EQ:
    if (m >= lo) add_exact_k(m);
    break;

  case ComparisonOperator::GT:
    ++m;
    [[fallthrough]];
  case ComparisonOperator::GE: {
    /* count >= m : minimal present count is max(m, lo). */
    const long mink = std::max(m, lo);
    if (absorptive) {
      upset = true;
      add_exact_k(mink);
    } else
      for (long k = mink; k <= n; ++k) add_exact_k(k);
    break;
  }

  case ComparisonOperator::LT:
    --m;
    [[fallthrough]];
  case ComparisonOperator::LE:
    /* count <= m : worlds k in [lo, m] (empty world k=0 included for a scalar
     * aggregate iff m >= 0). */
    for (long k = lo; k <= m; ++k) add_exact_k(k);
    break;

  case ComparisonOperator::NE:
    /* count != m : every world in [lo, n] except k = m (the scalar empty world
     * k=0 is included iff m != 0). */
    for (long k = lo; k <= n; ++k)
      if (k != m) add_exact_k(k);
    break;
  }

  return out;
}

}

/**
 * @brief Apply a comparison operator to two values.
 * @tparam I  Type of the left operand.
 * @tparam J  Type of the right operand.
 * @param a   Left operand.
 * @param op  Comparison operator.
 * @param b   Right operand.
 * @return    Result of the comparison.
 */
template<typename I, typename J>
static bool compare(I a, ComparisonOperator op, J b) {
  switch (op) {
  case ComparisonOperator::EQ:  return a == b;
  case ComparisonOperator::NE:  return a != b;
  case ComparisonOperator::GT:  return a >  b;
  case ComparisonOperator::LT:  return a <  b;
  case ComparisonOperator::GE:  return a >= b;
  case ComparisonOperator::LE:  return a <= b;
  }
  return false;
}

/**
 * @brief Evaluate whether the aggregation of @p values masked by @p mask satisfies @p op @p constant.
 * @param values      Input values to aggregate.
 * @param mask        Boolean mask selecting which values to include.
 * @param constant    Right-hand side constant of the comparison.
 * @param op          Comparison operator.
 * @param aggregator  Aggregator to apply to the selected values.
 * @return            @c true if the aggregate result satisfies the comparison.
 */
bool evaluate(const std::vector<long>& values,
              const std::vector<bool>& mask,
              long constant, ComparisonOperator op,
              std::unique_ptr<Aggregator> aggregator)
{
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (mask[i]) aggregator->add(AggValue {values[i]});
  }
  auto res = aggregator->finalize();
  switch(aggregator->resultType()) {
  case ValueType::INT:
    return compare(std::get<long>(res.v), op, constant);
  case ValueType::BOOLEAN:
    return compare(std::get<bool>(res.v), op, constant);
  case ValueType::FLOAT:
    return compare(std::get<double>(res.v), op, constant);
  default:
    throw std::runtime_error("Cannot compare this kind of value");
  }
}

bool agg_cmp_holds_in_world(
  const std::vector<long> &values,
  const mask_t &present,
  long constant,
  ComparisonOperator op,
  AggregationOperator agg_kind)
{
  // Empty-group exclusion: an all-absent group does not exist, so HAVING is
  // never satisfied on it (the one edge convention a sampler must reproduce to
  // match the expansion).
  bool any = false;
  for (bool b : present)
    if (b) { any = true; break; }
  if (!any)
    return false;

  return evaluate(values, present, constant, op,
                  makeAggregator(agg_kind, ValueType::INT));
}

/**
 * @brief Enumerate all subsets satisfying a HAVING predicate by exhaustive search.
 * @param values      Input values.
 * @param constant    Constant for the comparison.
 * @param op          Comparison operator.
 * @param agg_kind    Aggregation function to apply.
 * @param absorptive  Whether the semiring is absorptive.
 * @param upset       Set to @c true if the result set forms an upset (monotone).
 * @return            Vector of satisfying subset masks.
 */
std::vector<mask_t> enumerate_exhaustive(
  const std::vector<long> &values,
  long constant,
  ComparisonOperator op,
  AggregationOperator agg_kind,
  bool absorptive,
  bool &upset)
{
  const size_t n = values.size();

  std::vector<mask_t> worlds;
  mask_t mask(n);

  bool all_worlds = true;

  while(increment(mask)) { // Skipping empty world (handled by agg_cmp_holds_in_world too)
    if(agg_cmp_holds_in_world(values, mask, constant, op, agg_kind))
      worlds.push_back(mask);
    else
      all_worlds=false;
  }

  if(all_worlds && absorptive)
  {
    worlds.clear();

    // In that case, the result is equivalent to the upset generated by
    // the single-tuple possible worlds
    combinations(0, 1, mask_t(n), worlds);
    upset=true;
  }

  return worlds;
}

std::vector<mask_t> enumerate_valid_worlds(
  const std::vector<long> &values,
  long constant,
  ComparisonOperator op,
  AggregationOperator agg_kind,
  bool absorptive,
  bool &upset,
  bool is_scalar
  )
{
  if (agg_kind == AggregationOperator::COUNT) {
    /* count(*) (every contributor's value is the unit 1) is pure cardinality:
     * count_enum enumerates by subset size and folds the scalar empty world via
     * its lo bound.  count(col) keeps the COUNT identity but carries per-row 0/1
     * values (0 for a NULL-valued / null-padded row that still keeps the group
     * alive); cardinality would wrongly count the 0-valued rows, so route it to
     * the value-aware sum_dp -- with keep_empty=is_scalar, since a scalar
     * count(col)'s empty group is the real value 0 (not SQL NULL like a sum). */
    bool all_one = true;
    for (long v : values) if (v != 1) { all_one = false; break; }
    if (all_one)
      return count_enum(values,constant,op, absorptive, upset, is_scalar);
    try {
      return sum_dp(values, constant, op, absorptive, upset, /*keep_empty=*/is_scalar);
    } catch(DPException &e) {
      // 0/1 values are non-negative, so this never throws; fall back defensively.
      return enumerate_exhaustive(values, constant, op, agg_kind, absorptive, upset);
    }
  }

  if(agg_kind == AggregationOperator::SUM)
    try {
      return sum_dp(values, constant, op, absorptive, upset);
    } catch(DPException &e) {
      // We will use the default implementation of the enumeration
    }

  return enumerate_exhaustive(values, constant, op, agg_kind, absorptive, upset);
}
