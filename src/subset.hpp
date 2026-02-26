#ifndef SUBSET_HPP
#define SUBSET_HPP

#include <cstdint>
#include <vector>

#include "Aggregation.h"

// Return valid worlds as bitmasks over [0..n-1].
// Bit i = 1 means tuple i is present in world W.
using mask_t=std::vector<bool>;

std::vector<mask_t> enumerate_valid_worlds(
  const std::vector<long> &values,
  int constant,
  ComparisonOperator op,
  AggregationOperator agg_kind,
  bool enumerate,
  bool &upset
  );

#endif /* SUBSET_HPP */
