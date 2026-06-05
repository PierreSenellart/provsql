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
 * @param is_scalar Scalar (no GROUP BY) aggregation: the output row exists
 *                  even in the empty world, so the empty subset is also
 *                  tested against the predicate.
 * @return          Vector of bitmasks, one per valid world.
 */
std::vector<mask_t> enumerate_valid_worlds(
  const std::vector<long> &values,
  long constant,
  ComparisonOperator op,
  AggregationOperator agg_kind,
  bool enumerate,
  bool &upset,
  bool is_scalar = false
  );

/**
 * @brief Canonical per-world decision: does @c agg(present) @c op @c constant
 *        hold in the world where exactly the @p present tuples are present?
 *
 * The single source of truth for "value of @c agg θ k in one world", shared by
 * the all-worlds expansion (the exhaustive enumerator builds its world set from
 * this predicate) and by any sampler that draws one world (so sample and expand
 * agree by construction).
 *
 * Enforces the **empty-group exclusion**: a world with no present tuple makes
 * the predicate @c false (SQL evaluates HAVING only on groups that exist, so an
 * all-absent group never satisfies it -- even for e.g. @c sum(x) >= -5 or
 * @c min(x) <= k).  Otherwise it aggregates the present tuples' @p values with
 * @p agg_kind and applies @p op against @p constant on the integer grid (the
 * caller having scaled numeric / decimal-float domains; see @c matchAggCmp).
 *
 * @param values    Per-tuple aggregate contribution (scaled to a common grid).
 * @param present   Which tuples are present in this world (same length).
 * @param constant  Right-hand side of the comparison, on the same grid.
 * @param op        Comparison operator.
 * @param agg_kind  Aggregation function.
 * @return          @c true iff a non-empty group's aggregate satisfies @p op.
 */
bool agg_cmp_holds_in_world(
  const std::vector<long> &values,
  const mask_t &present,
  long constant,
  ComparisonOperator op,
  AggregationOperator agg_kind
  );

#endif /* SUBSET_HPP */
