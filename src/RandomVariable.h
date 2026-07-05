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

struct DistributionFamily;  // src/distributions/Distribution.h

/**
 * @brief Parsed distribution spec (family + up to two parameters).
 *
 * Stored in the @c extra byte string of a @c gate_rv as
 * <tt>"normal:μ,σ"</tt>, <tt>"uniform:a,b"</tt>, <tt>"exponential:λ"</tt>,
 * <tt>"erlang:k,λ"</tt>, <tt>"gamma:k,λ"</tt>, ...  @c family points at
 * the interned registry descriptor of the parsed name token (one
 * instance per family, so plain pointer comparison is family
 * identity); there is deliberately no family enum -- adding a family
 * registers a new descriptor without touching any shared header.
 */
struct DistributionSpec {
  const DistributionFamily *family;
  double p1;  ///< First parameter (μ, a, k, or λ)
  double p2;  ///< Second parameter (σ, b, or λ; unused for 1-parameter families)
};

/**
 * @brief One parameter slot of a @c gate_rv, either a literal or a wire.
 *
 * A latent-variable @c gate_rv (see the "Latent variables" surface)
 * lets a distribution parameter be a scalar provenance token rather
 * than a concrete double: the parameter is then a random variable
 * itself, making the leaf a compound (hierarchical) distribution.  In
 * the on-disk @c extra text a wired slot is written @c "$i" (0-based
 * index into the gate's wire vector); a literal slot keeps its decimal
 * text as before.  When @c wire_slot @c < @c 0 the slot is the literal
 * @c literal; otherwise it is resolved from @c wires[wire_slot] at
 * evaluation time.
 */
struct DistributionParam {
  double literal;    ///< Value when @c wire_slot < 0.
  int    wire_slot;  ///< >= 0: resolve from the gate's @c wires[wire_slot].
};

/**
 * @brief A @c gate_rv distribution spec that may carry wired (token)
 *        parameters -- the parse-time counterpart of @c DistributionSpec.
 *
 * @c DistributionSpec stays the fully-resolved @c {family, double, double}
 * form every analytic call site consumes; this parallel template keeps the
 * per-slot literal-or-wire distinction so the Monte Carlo sampler can
 * resolve wired parameters per iteration.  @c parametric() is true when any
 * slot is wired -- the signal every analytic path uses to fall through to
 * Monte Carlo (a compound leaf has no constant-parameter closed form).
 */
struct DistributionTemplate {
  const DistributionFamily *family;
  DistributionParam p1, p2;
  /// True iff any parameter is a wire reference (a compound / latent leaf).
  bool parametric() const { return p1.wire_slot >= 0 || p2.wire_slot >= 0; }
};

/**
 * @brief Parse the on-disk text encoding of a @c gate_rv distribution,
 *        keeping wired (token) parameters as wire references.
 *
 * Accepts the same family tokens as @c parse_distribution_spec, but a
 * parameter may be either a decimal literal or a wire reference @c "$i"
 * (0-based index into the gate's wire vector).  The literal form is
 * byte-identical to the pre-latent encoding, so an all-literal spec
 * round-trips unchanged.
 *
 * @param s The byte string read from @c MMappedCircuit::getExtra.
 * @return The parsed template, or @c std::nullopt on malformed input.
 */
std::optional<DistributionTemplate>
parse_distribution_template(const std::string &s);

/**
 * @brief Parse the on-disk text encoding of a @c gate_rv distribution.
 *
 * Accepts <tt>"normal:μ,σ"</tt>, <tt>"uniform:a,b"</tt>,
 * <tt>"exponential:λ"</tt>, <tt>"erlang:k,λ"</tt>, and
 * <tt>"gamma:k,λ"</tt>, with parameters parseable as @c double.
 * Whitespace around the kind name and parameters is tolerated.
 *
 * @param s The byte string read from @c MMappedCircuit::getExtra.
 * @return The parsed spec, or @c std::nullopt on malformed input @b or
 *         when any parameter is a wire reference (a compound / latent
 *         leaf, which has no constant-parameter closed form -- use
 *         @c parse_distribution_template for that case).
 */
std::optional<DistributionSpec> parse_distribution_spec(const std::string &s);

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
 * @brief Format a double back into the canonical text form used by
 *        @c gate_value extras and @c gate_rv distribution parameters
 *        (the serialisation counterpart of @c parseDoubleStrict).
 *
 * @c std::to_chars produces the shortest decimal representation that
 * round-trips through @c std::from_chars / @c std::stod, so round
 * cases like @c 0.2 = 0.4/2 print as @c "0.2" rather than
 * @c "0.20000000000000001" while irrational values fall back to
 * whatever length is needed for exact recovery.  The legacy
 * @c std::ostringstream @c << @c setprecision(17) path is kept as a
 * defensive fallback in case @c to_chars fails (range / buffer).
 */
std::string double_to_text(double v);

/**
 * @brief Closed-form expectation E[X] for a basic distribution.
 *
 * - Normal(μ, σ):     μ
 * - Uniform(a, b):    (a + b) / 2
 * - Exponential(λ):   1 / λ
 * - Erlang / Gamma(k, λ): k / λ
 */
double analytical_mean(const DistributionSpec &d);

/**
 * @brief Closed-form variance Var(X) for a basic distribution.
 *
 * - Normal(μ, σ):     σ²
 * - Uniform(a, b):    (b − a)² / 12
 * - Exponential(λ):   1 / λ²
 * - Erlang / Gamma(k, λ): k / λ²
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
 * - Erlang / Gamma(s, λ):
 *   @f$\Gamma(s+k) / (\Gamma(s) \lambda^k) = s(s+1)\cdots(s+k-1) / \lambda^k@f$
 *   (the rising factorial is valid for any real shape @f$s > 0@f$).
 *
 * Returns 1 for @f$k = 0@f$ and @c analytical_mean for @f$k = 1@f$.
 */
double analytical_raw_moment(const DistributionSpec &d, unsigned k);

}  // namespace provsql

#endif  // PROVSQL_RANDOM_VARIABLE_H
