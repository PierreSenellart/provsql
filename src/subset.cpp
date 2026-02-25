#include "subset.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>
#include <stdexcept>

namespace {

static std::vector<uint64_t> all_worlds(const std::vector<int> &values)
{
  const size_t n=values.size();
  const uint64_t total =(1ULL << n);
  std::vector<uint64_t> worlds;

 for (uint64_t mask = 0; mask < total; ++mask) { worlds.push_back(mask); }
  return worlds;
}

static void append_range(std::vector<uint64_t> &out,
                         const std::vector<std::vector<uint64_t>> &dp,
                         int lo,
                         int hi)
{
  if (dp.empty()) return;
  const int J = static_cast<int>(dp.size())-1;
  lo = std::max(lo, 0);
  hi = std::min(hi, J);
  if (lo>hi ) return;

  for (int j = lo; j <= hi; ++j) {
    out.insert(out.end(), dp[j].begin(), dp[j].end());
  }
}

static std::vector<uint64_t> sum_dp(const std::vector<int> &values, int C, ComparisonOp op)
{
  const std::size_t n = values.size();

  for (int w : values) {
    if (w < 0) {
      throw std::runtime_error("sum_dp world enumeration requires non-negative weights");
    }
  }


   long long T_ll=0;
    for (int w : values ) T_ll+=w;
    if (T_ll>std::numeric_limits<int>::max())
    {
      throw std::runtime_error("DP bound overflowww");
     }

    const int T= static_cast<int>(T_ll);
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


   int J=0;
   if (op == ComparisonOp::GT || op == ComparisonOp ::GE)
  {
    J=T;
  }

  else if (op == ComparisonOp::LT){
    J=std::min(C-1,T);

  }
  else{
    J=std::min(C,T);
  }
 if (J<0){return {};}

  std::vector<std::vector<uint64_t>> dp(static_cast<std::size_t>(J) + 1);
  dp[0].push_back(0ULL); // dp[0] <- {emptyset}

  
    int pref_sum =0;

    for (std::size_t i=0;i<n;++i)
    {
     const int w=values[i];
     const uint64_t bit = (1ULL <<i);
     pref_sum+=w;
     const int j_max= std::min(J,pref_sum);

     if (w == 0) {
      for (int j = j_max; j >= 0; --j) {
        const std::size_t sz = dp[j].size();
        for (std::size_t k = 0; k < sz; ++k) {
          dp[j].push_back(dp[j][k] | bit);
        }
      }
      continue;
     }
         
     for (int j = j_max; j >= w; --j) 
     {
      const int p = j - w;
      if (dp[p].empty()) continue;
      const std::size_t sz = dp[p].size();
      for (std::size_t k = 0; k < sz; ++k) {
        dp[j].push_back(dp[p][k] | bit);
      }
     }
    }
 

  std::vector<uint64_t> R;
  switch(op){
    case ComparisonOp::EQ:
        append_range(R,dp,C,C);
        break;

    case ComparisonOp::GT:
        append_range(R,dp,C+1,J);
        break;


    case ComparisonOp::LT:
        append_range(R,dp,0,C-1);
        break;


    case ComparisonOp::GE:
        append_range(R,dp,C,J);
        break;


    case ComparisonOp::LE:
        append_range(R,dp,0,C);
         break;
    
    case ComparisonOp::NEQ:
         {
           std::vector<uint64_t> lt= sum_dp(values, C, ComparisonOp::LT);
          std::vector<uint64_t> gt= sum_dp(values, C, ComparisonOp::GT);
          R.reserve(lt.size()+gt.size());
          R.insert(R.end(),lt.begin(),lt.end());
          R.insert(R.end(),gt.begin(),gt.end());
          return R;

         }

  }
 return R;
}

//generate k-subsets form an n-set
static void combinations(std::size_t start,
                       std::size_t n,
                       int k_left,
                       uint64_t mask,
                       std::vector<uint64_t> &out)
{
  if (k_left == 0) {
    out.push_back(mask);
    return;
  }
  if (start >= n) return;

  const std::size_t remaining = n - start;
  if (remaining < static_cast<std::size_t>(k_left)) return;

  // include current index
  combinations(start + 1, n, k_left - 1, mask | (1ULL << start), out);
  // exclude current index
  combinations(start + 1, n, k_left, mask, out);
}

static std::vector<uint64_t> count_enum(const std::vector<int> &values, int m, ComparisonOp op)
{
  const int n = static_cast<int>(values.size());
  std::vector<uint64_t> out;

  auto add_exact_k = [&](int k) {
    if (k < 0 || k > n) return;
    combinations(0, static_cast<std::size_t>(n), k, 0ULL, out);
  };

  switch (op)
  {
    case ComparisonOp::EQ:
      add_exact_k(m);
      break;

    case ComparisonOp::GT:
      for (int k = m + 1; k <= n; ++k) add_exact_k(k);
      break;

    case ComparisonOp::LT:
      for (int k = 0; k <= m - 1; ++k) add_exact_k(k);
      break;

    case ComparisonOp::GE:
      for (int k = m; k <= n; ++k) add_exact_k(k);
      break;


    case ComparisonOp::LE:
      for (int k = 0; k <= m; ++k) add_exact_k(k);
      break;

    case ComparisonOp::NEQ:
      for (int k = 0; k <= n; ++k)
      {
        if (k != m) add_exact_k(k);
      }
      break;
  }

  return out;
}

}//end namesoace



std::vector<uint64_t> enumerate_valid_worlds(
  const std::vector<int> &values,
  int constant,
  ComparisonOp op,
  AggKind agg_kind
) 
{
  const std::size_t n = values.size();
  if (n >= 63) {
    throw std::runtime_error("n>63 not supported");
  }
  if (agg_kind == AggKind::COUNT){return count_enum(values,constant,op);}
  return sum_dp(values,constant,op);
  //could implement a similar if elsse if ladder for different aggregates when required
}










































// #include "subset.hpp"
// #include <stdexcept>
//
// static bool compare_int(int a, ComparisonOp op, int b) {
//   switch (op) {
//   case ComparisonOp::EQ:  return a == b;
//   case ComparisonOp::NEQ: return a != b;
//   case ComparisonOp::GT:  return a >  b;
//   case ComparisonOp::LT:  return a <  b;
//   case ComparisonOp::GE:  return a >= b;
//   case ComparisonOp::LE:  return a <= b;
//   }
//   return false;
// }
//
// std::vector<uint64_t> enumerate_valid_worlds(
//   const std::vector<int> &values,
//   int constant,
//   ComparisonOp op
//   ) {
//   const size_t n = values.size();
//
//   // bitmask enumeration: support up to 62 tuples
//   if (n >= 63) {
//     throw std::runtime_error("enumerate_valid_worlds: n >= 63 not supported (bitmask enumeration)");
//   }
//
//   const uint64_t total = (1ULL << n);
//   std::vector<uint64_t> worlds;
//
//   for (uint64_t mask = 0; mask < total; ++mask) {
//     int sum = 0;
//     for (size_t i = 0; i < n; ++i) {
//       if (mask & (1ULL << i)) sum += values[i];
//     }
//     if (compare_int(sum, op, constant)) {
//       worlds.push_back(mask);
//     }
//   }
//
//   return worlds;
// }
