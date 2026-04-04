/**
 * @file subset.hpp
 * @brief Enumerate tuple subsets satisfying an aggregate HAVING predicate.
 *
 * For aggregate provenance evaluation, ProvSQL needs to determine which
 * subsets of base tuples produce a group-aggregate value that satisfies
 * the HAVING condition.  This header declares the @c enumerate_valid_worlds()
 * function that enumerates all such subsets ("valid worlds") as bitmasks.
 *
 * A *world* is represented as a @c mask_t = @c std::vector<bool> of length
 * @f$n@f$, where bit @f$i@f$ is @c true iff tuple @f$i@f$ is present in
 * that world.  The function returns all worlds where the aggregate of the
 * present tuples' values satisfies the given comparison.
 */
#ifndef SUBSET_HPP
#define SUBSET_HPP

#include <cstdint>
#include <vector>

#include "Aggregation.h"

/**
 * @brief A bitmask over @f$n@f$ tuples representing one possible world.
 *
 * @c mask_t[i] is @c true iff tuple @f$i@f$ is present in this world.
 */
using mask_t=std::vector<bool>;

/**
 * @brief Enumerate all subsets of @p n tuples satisfying an aggregate predicate.
 *
 * For each subset @f$W \subseteq \{0, \ldots, n-1\}@f$ of the tuples,
 * computes the aggregate of their @p values, tests the predicate
 * @f$\text{agg}(W) \;\mathit{op}\; \text{constant}@f$, and includes @f$W@f$
 * in the result if the predicate holds.
 *
 * @param values    Aggregate contribution of each tuple (one value per tuple).
 * @param constant  The constant on the right-hand side of the comparison.
 * @param op        Comparison operator (=, ≠, <, ≤, >, ≥).
 * @param agg_kind  Aggregation function (SUM, COUNT, MIN, MAX, …).
 * @param enumerate If @c false, only determine whether the set of valid worlds
 *                  is an upset; the returned vector may be empty.
 * @param upset     Output: set to @c true if the set of valid worlds forms an
 *                  upset (upward-closed set), @c false otherwise.
 * @return          Vector of bitmasks, one per valid world.
 */
std::vector<mask_t> enumerate_valid_worlds(
  const std::vector<long> &values,
  int constant,
  ComparisonOperator op,
  AggregationOperator agg_kind,
  bool enumerate,
  bool &upset
  );

#endif /* SUBSET_HPP */
