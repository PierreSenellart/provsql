/**
 * @file RandomVariable.h
 * @brief Continuous random-variable helpers (distribution parsing, moments).
 *
 * Helpers for the @c gate_rv leaf introduced for continuous probabilistic
 * c-tables.  A @c gate_rv stores its distribution name and parameters in
 * the gate's @c extra byte string using a small text encoding (e.g.
 * <tt>"normal:2.5,0.5"</tt>); these helpers parse and format that encoding,
 * and provide closed-form moments where they exist.  Arithmetic over RV
 * expressions is built on the generic @c gate_arith gate (see
 * @c provsql_utils.h), which is shared with non-RV scalar arithmetic.
 *
 * Sampling itself lives in @c BooleanCircuit::monteCarlo and
 * @c MonteCarloSampler; this header only exposes what is needed for
 * parsing and analytical moment computation.
 */
#ifndef PROVSQL_RANDOM_VARIABLE_H
#define PROVSQL_RANDOM_VARIABLE_H

#include <optional>
#include <string>

namespace provsql {

/**
 * @brief Continuous distribution kinds supported by @c gate_rv.
 */
enum class DistKind {
  Normal,      ///< Normal (Gaussian): p1=μ, p2=σ
  Uniform,     ///< Uniform on [a,b]: p1=a, p2=b
  Exponential  ///< Exponential: p1=λ, p2 unused
};

/**
 * @brief Parsed distribution spec (kind + up to two parameters).
 *
 * Stored in the @c extra byte string of a @c gate_rv as
 * <tt>"normal:μ,σ"</tt>, <tt>"uniform:a,b"</tt>, or <tt>"exponential:λ"</tt>.
 */
struct DistributionSpec {
  DistKind kind;
  double p1;  ///< First parameter (μ, a, or λ)
  double p2;  ///< Second parameter (σ or b; unused for Exponential)
};

/**
 * @brief Parse the on-disk text encoding of a @c gate_rv distribution.
 *
 * Accepts <tt>"normal:μ,σ"</tt>, <tt>"uniform:a,b"</tt>, and
 * <tt>"exponential:λ"</tt>, with parameters parseable as @c double.
 * Whitespace around the kind name and parameters is tolerated.
 *
 * @param s The byte string read from @c MMappedCircuit::getExtra.
 * @return The parsed spec, or @c std::nullopt on malformed input.
 */
std::optional<DistributionSpec> parse_distribution_spec(const std::string &s);

/**
 * @brief Format a spec back into its on-disk text encoding.
 *
 * Inverse of @c parse_distribution_spec: round-trip safe up to the
 * precision of @c std::to_string for @c double.
 */
std::string format_distribution_spec(const DistributionSpec &d);

/**
 * @brief Strictly parse @p s as a @c double.
 *
 * Used by every consumer that has to interpret the @c extra byte
 * string of a @c gate_value: the sampler when sampling a constant
 * leaf, the interval-arith pass when bounding a constant leaf, and
 * any future scalar-evaluation pass.  Lives here (rather than next
 * to one specific consumer) so the parsing convention is shared.
 *
 * @throws CircuitException on empty input, non-numeric input, or
 *         trailing characters past the parsed double.
 */
double parseDoubleStrict(const std::string &s);

/**
 * @brief Closed-form expectation E[X] for a basic distribution.
 *
 * - Normal(μ, σ):     μ
 * - Uniform(a, b):    (a + b) / 2
 * - Exponential(λ):   1 / λ
 */
double analytical_mean(const DistributionSpec &d);

/**
 * @brief Closed-form variance Var(X) for a basic distribution.
 *
 * - Normal(μ, σ):     σ²
 * - Uniform(a, b):    (b − a)² / 12
 * - Exponential(λ):   1 / λ²
 */
double analytical_variance(const DistributionSpec &d);

/**
 * @brief Closed-form raw moment @f$E[X^k]@f$ for a basic distribution.
 *
 * - Normal(μ, σ):
 *   @f$\sum_{j=0,2,\ldots}^{k} \binom{k}{j} \mu^{k-j} \sigma^j (j-1)!!@f$
 *   (odd-@f$j@f$ terms vanish since central moments of @f$N(0, \sigma)@f$
 *   are zero for odd @f$j@f$).
 * - Uniform(a, b):    @f$(b^{k+1} - a^{k+1}) / ((k+1)(b-a))@f$.
 * - Exponential(λ):   @f$k! / \lambda^k@f$.
 *
 * Returns 1 for @f$k = 0@f$ and @c analytical_mean for @f$k = 1@f$.
 */
double analytical_raw_moment(const DistributionSpec &d, unsigned k);

}  // namespace provsql

#endif  // PROVSQL_RANDOM_VARIABLE_H
