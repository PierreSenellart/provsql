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

static std::vector<mask_t> all_worlds(const std::vector<int> &values)
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
                         int lo,
                         int hi)
{
  if (dp.empty()) return;
  const int J = static_cast<int>(dp.size())-1;
  lo = std::max(lo, 0);
  hi = std::min(hi, J);
  if (lo>hi) return;

  for (int j = lo; j <= hi; ++j) {
    out.insert(out.end(), dp[j].begin(), dp[j].end());
  }
}

class DPException : public std::exception {};

#define MIN(x,y) ((x)<(y)?(x):(y))

static std::vector<mask_t> sum_dp(const std::vector<int> &values, int C, ComparisonOp op, bool absorptive)
{
  const std::size_t n = values.size();

  std::vector<mask_t> R;

  // We first deal with NEQ by combining LT and GT
  if(op == ComparisonOp::NEQ) {
    std::vector<mask_t> lt= sum_dp(values, C, ComparisonOp::LT, absorptive);
    std::vector<mask_t> gt= sum_dp(values, C, ComparisonOp::GT, absorptive);
    R.reserve(lt.size()+gt.size());
    R.insert(R.end(),lt.begin(),lt.end());
    R.insert(R.end(),gt.begin(),gt.end());
    return R;
  }

  long long T=0;
  for (int w: values) {
    if (w < 0)
      throw DPException();
    T+=w;
  }

  //no valid worlds case
  if (op == ComparisonOp::GT && C>=T) return {};
  if (op == ComparisonOp::GE && C>T) return {};
  if (op == ComparisonOp::LT && C<=0) return {};
  if (op == ComparisonOp::LE && C<0) return {};
  if (op == ComparisonOp::EQ && (C>T || C<0)) return {};

  //tautology cases
  if (op == ComparisonOp::GT && C<0) return all_worlds(values);
  if (op == ComparisonOp::GE && C<=0) return all_worlds(values);
  if (op == ComparisonOp::LT && C>T) return all_worlds(values);
  if (op == ComparisonOp::LE && C>=T) return all_worlds(values);

  long long J=0;
  if (op == ComparisonOp::GT || op == ComparisonOp::GE)
    J=T;
  else if (op==ComparisonOp::LT)
    J=MIN(C-1,T);
  else
    J=MIN(C,T);

  assert(J>=0);

  std::vector<std::vector<mask_t> > dp(static_cast<std::size_t>(J) + 1);
  dp[0].push_back(mask_t(n)); // dp[0] <- {emptyset}

  int pref_sum=0;

  for (std::size_t i=0; i<n; ++i)
  {
    const int w=values[i];
    pref_sum+=w;
    const int j_max=MIN(J,pref_sum);

    for (int j = j_max; j >= w; --j) {
      const int p = j - w;
      if(absorptive && ((op==ComparisonOp::GT && p>C) ||
                        (op==ComparisonOp::GE && p>=C)))
        continue;
      size_t s=dp[p].size();
      for(size_t k=0; k<s; ++k) {
        mask_t m = dp[p][k];
        m[i] = true;
        dp[j].push_back(m);
      }
    }
  }

  switch(op){
  case ComparisonOp::EQ:
    append_range(R,dp,C,C);
    break;

  case ComparisonOp::GT:
    append_range(R,dp,C+1,J);
    break;


  case ComparisonOp::LT:
    append_range(R,dp,1,C-1);
    break;

  case ComparisonOp::GE:
    append_range(R,dp,C,J);
    break;

  case ComparisonOp::LE:
    append_range(R,dp,1,C);
    break;

  case ComparisonOp::NEQ: // case already processed
    assert(false);
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

static std::vector<mask_t> count_enum(const std::vector<int> &values, int m, ComparisonOp op, bool absorptive)
{
  const int n = static_cast<int>(values.size());
  std::vector<mask_t> out;

  auto add_exact_k = [&](int k) {
                       if (k < 0 || k > n) return;
                       combinations(0, k, mask_t(n), out);
                     };

  switch (op)
  {
  case ComparisonOp::EQ:
    if(m!=0) add_exact_k(m);
    break;

  case ComparisonOp::GT:
    ++m;
    [[fallthrough]];
  case ComparisonOp::GE:
    if(absorptive)
      add_exact_k(m);
    else
      for (int k = m; k <= n; ++k) add_exact_k(k);
    break;

  case ComparisonOp::LT:
    --m;
    [[fallthrough]];
  case ComparisonOp::LE:
    for (int k = 1; k <= m; ++k) add_exact_k(k);
    break;

  case ComparisonOp::NEQ:
    for (int k = 1; k <= n; ++k)
    {
      if (k != m) add_exact_k(k);
    }
    break;
  }

  return out;
}

}

static bool compare_int(int a, ComparisonOp op, int b) {
  switch (op) {
  case ComparisonOp::EQ:  return a == b;
  case ComparisonOp::NEQ: return a != b;
  case ComparisonOp::GT:  return a >  b;
  case ComparisonOp::LT:  return a <  b;
  case ComparisonOp::GE:  return a >= b;
  case ComparisonOp::LE:  return a <= b;
  }
  return false;
}

static bool evaluate(
  const std::vector<int> values,
  int constant,
  ComparisonOp op,
  mask_t mask,
  AggKind agg_kind)
{
  if(agg_kind != AggKind::SUM)
    throw std::runtime_error("Aggregation operator not supported.");

  int sum = 0;
  for (size_t i = 0; i < values.size(); ++i) {
    if (mask[i])
      sum += values[i];
  }

  return compare_int(sum, op, constant);
}

std::vector<mask_t> enumerate_exhaustive(
  const std::vector<int> &values,
  int constant,
  ComparisonOp op,
  AggKind agg_kind)
{
  const size_t n = values.size();

  if (n >= 63)
    throw std::runtime_error("enumerate_valid_worlds: n >= 63 not supported (bitmask enumeration)");

  std::vector<mask_t> worlds;
  mask_t mask(n);

  while(increment(mask)) { // Skipping possible world
    if(evaluate(values, constant, op, mask, agg_kind))
      worlds.push_back(mask);
  }

  return worlds;
}

std::vector<mask_t> enumerate_valid_worlds(
  const std::vector<int> &values,
  int constant,
  ComparisonOp op,
  AggKind agg_kind,
  bool absorptive
  )
{
  if (agg_kind == AggKind::COUNT)
    return count_enum(values,constant,op, absorptive);

  if(agg_kind == AggKind::SUM)
    try {
      return sum_dp(values, constant, op, absorptive);
    } catch(DPException &e) {
      // We will use the default implementation of the enumeration
    }

  return enumerate_exhaustive(values, constant, op, agg_kind);
}
