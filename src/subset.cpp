#include "subset.hpp"
#include <stdexcept>

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

std::vector<uint64_t> enumerate_valid_worlds(
  const std::vector<int> &values,
  int constant,
  ComparisonOp op
) {
  const size_t n = values.size();

  // bitmask enumeration: support up to 62 tuples
  if (n >= 63) {
    throw std::runtime_error("enumerate_valid_worlds: n >= 63 not supported (bitmask enumeration)");
  }

  const uint64_t total = (1ULL << n);
  std::vector<uint64_t> worlds;
  worlds.reserve(total); // worst case if conparison predicate matches everything

  for (uint64_t mask = 0; mask < total; ++mask) {
    int sum = 0;
    for (size_t i = 0; i < n; ++i) {
      if (mask & (1ULL << i)) sum += values[i];
    }
    if (compare_int(sum, op, constant)) {
      worlds.push_back(mask);
    }
  }

  return worlds;
}
