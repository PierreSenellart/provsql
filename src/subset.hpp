#ifndef SUBSET_HPP
#define SUBSET_HPP

#include <cstdint>
#include <vector>

enum class ComparisonOp {
  EQ, NEQ, GT, LT, GE, LE
};

// Return valid worlds as bitmasks over [0..n-1].
// Bit i = 1 means tuple i is present in world W.
std::vector<uint64_t> enumerate_valid_worlds(
  const std::vector<int> &values,
  int constant,
  ComparisonOp op
);

#endif 
