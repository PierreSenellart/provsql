#include "subset.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>
#include <stdexcept>

namespace {

// static bool all_ones(const std::vector<int> &values)
// {
//   for (int v: values)
//   {
//     if (v!=1) return false;
//   }
//   return true;
// }

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

  int J = 0;

  if (op == ComparisonOp::GT || op == ComparisonOp ::GE)
  {
    long long T=0;
    for (int w : values ) T+=w;
    if (T>std::numeric_limits<int>::max())
    {
      throw std::runtime_error("DP bound overflowww");
     }

    J= static_cast<int>(T);
  }

  else if (op == ComparisonOp::LT){
    J=C-1;

  }
  else{
    J=C;
  }
 if (J<0){return {};}

  std::vector<std::vector<uint64_t>> dp(static_cast<std::size_t>(J) + 1);
  dp[0].push_back(0ULL); // dp[0] <- {emptyset}

  for (std::size_t i = 0; i < n; ++i) {
    const int w = values[i];
    const uint64_t bit = (1ULL << i);

    if (w == 0) {
      for (int j = J; j >= 0; --j) {
        const std::size_t sz = dp[j].size();
        for (std::size_t k = 0; k < sz; ++k) {
          dp[j].push_back(dp[j][k] | bit);
        }
      }
      continue;
    }

    for (int j = J; j >= w; --j) 
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


}//end namesoace



std::vector<uint64_t> enumerate_valid_worlds(
  const std::vector<int> &values,
  int constant,
  ComparisonOp op
) 
{
  const std::size_t n = values.size();
  if (n >= 63) {
    throw std::runtime_error("n>63 not supported");
  }
  return sum_dp(values,constant,op);
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
