/**
 * @file Distribution.h
 * @brief Per-family polymorphic view over a continuous @c gate_rv
 *        distribution (§F.1 class hierarchy).
 *
 * Replaces the @c DistKind @c switch blocks scattered across the RV
 * evaluators with one virtual dispatch per family.  A @c Distribution is a
 * transient view constructed from a parsed @c DistributionSpec via
 * @ref makeDistribution -- there is no gate-ABI or on-disk change; the
 * @c extra text encoding and @c DistributionSpec POD are unchanged.
 *
 * Adding a family becomes a new subclass + a factory arm (the migration is
 * ongoing; see @c doc/TODO/distribution-refactor.md).  Methods that a
 * family cannot answer follow the existing NaN-as-undecided contract so the
 * analytic paths fall back to Monte Carlo unchanged.
 */
#ifndef PROVSQL_DISTRIBUTION_H
#define PROVSQL_DISTRIBUTION_H

#include <cstddef>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "../RandomVariable.h"  // DistKind, DistributionSpec

namespace provsql {

/** @brief A closed support interval [lo, hi] (±infinity for unbounded). */
struct DistSupport {
  double lo;
  double hi;
};

/**
 * @brief Abstract per-family continuous distribution.
 *
 * Concrete subclasses (Normal / Uniform / Exponential / Erlang) hold the
 * two parameters and implement the family-specific closed forms.  The
 * interface grows as consumers migrate off their @c DistKind switches; the
 * methods below are the family-local ones (pairwise closure / comparison
 * rules live in separate registries, not here).
 */
class Distribution {
public:
  virtual ~Distribution() = default;

  /** @name Identity (parameters, for the i.i.d. equality test) */
  ///@{
  virtual DistKind kind() const = 0;
  virtual double p1() const = 0;
  virtual double p2() const = 0;
  ///@}

  /** @name Closed-form moments */
  ///@{
  virtual double mean() const = 0;                 ///< E[X]
  virtual double variance() const = 0;             ///< Var(X)
  virtual double rawMoment(unsigned k) const = 0;  ///< E[X^k]
  ///@}

  /** @name Density / distribution */
  ///@{
  virtual double pdf(double x) const = 0;  ///< f(x); NaN if the family declines
  virtual double cdf(double x) const = 0;  ///< F(x); NaN if the family declines
  ///@}

  /** @brief Natural support interval of X. */
  virtual DistSupport support() const = 0;

  /**
   * @brief Finite window [lo, hi] covering essentially all of X's mass, for
   *        numerical quadrature.  Returns false (leaving @p lo / @p hi
   *        untouched) when the parameters are degenerate.
   */
  virtual bool integrationRange(double &lo, double &hi) const = 0;

  /**
   * @brief Plot x-window given optional truncation bounds (±infinity means
   *        "unbounded on that side"); used by the SVG curve renderer.
   */
  virtual std::pair<double, double> plotRange(double trunc_lo,
                                              double trunc_hi) const = 0;

  /** @brief Draw one sample using the shared MC generator. */
  virtual double sample(std::mt19937_64 &rng) const = 0;

  /**
   * @brief Inverse CDF @f$Q(p) = F^{-1}(p)@f$ for @f$p \in (0, 1)@f$.
   *
   * @c std::nullopt when the family has no elementary inverse CDF
   * (Erlang), so "unsupported" cannot be silently consumed -- callers
   * branch on availability explicitly.  Normal uses the
   * Beasley-Springer-Moro rational approximation (~1e-7 accuracy, ample
   * for sampling); Uniform / Exponential invert exactly.  Behaviour at
   * the endpoints is the family's (BSM diverges; clamp @p p strictly
   * inside @c (0, 1) before calling).
   */
  virtual std::optional<double> quantile(double p) const {
    return std::nullopt;
  }

  /**
   * @brief Closed-form raw moment @f$E[X^k \mid lo < X < hi]@f$ of the
   *        distribution truncated to @c [lo, hi] (±infinity for a
   *        semi-infinite side).
   *
   * @c std::nullopt when the family has no closed form (Erlang: needs
   * the regularised lower incomplete gamma) or the truncation is
   * degenerate (mass below a numerical floor), so the caller falls
   * through to MC rejection.  @p k is at least 1 (the caller
   * short-circuits @c k @c = @c 0).
   */
  virtual std::optional<double> truncatedRawMoment(double lo, double hi,
                                                   unsigned k) const {
    (void) lo; (void) hi; (void) k;
    return std::nullopt;
  }

  /**
   * @brief Draw @p n rejection-free samples of X conditioned on
   *        @c lo < X < hi (±infinity for a semi-infinite side).
   *
   * Each family uses its own exact scheme (interval intersection for
   * Uniform, memorylessness / stable inverse-CDF for Exponential,
   * inverse-CDF transform for Normal).  @c std::nullopt when the family
   * has no scheme (Erlang: needs the inverse regularised incomplete
   * gamma) or the truncation is degenerate / parameters invalid, so the
   * caller's MC-rejection fallback can emit its usual diagnostics.
   */
  virtual std::optional<std::vector<double>> sampleTruncated(
    std::mt19937_64 &rng, double lo, double hi, unsigned n) const {
    (void) rng; (void) lo; (void) hi; (void) n;
    return std::nullopt;
  }

  /**
   * @brief Closed-form mean of the max / min of @p n i.i.d. copies of X.
   *
   * - Uniform(a, b): @c E[max] @c = @c a + (b-a)·n/(n+1),
   *   @c E[min] @c = @c a + (b-a)/(n+1).
   * - Exponential(λ): @c E[min] @c = @c 1/(nλ), @c E[max] @c = @c H_n/λ
   *   (harmonic number), for @c λ > 0.
   *
   * @c std::nullopt when the family has no elementary order-statistic
   * mean (Normal, Erlang -- those need the 1-D layer-cake quadrature) or
   * the parameters are degenerate, so the caller falls back to
   * quadrature / Monte Carlo.
   */
  virtual std::optional<double> iidOrderStatMean(std::size_t n,
                                                 bool isMax) const {
    (void) n; (void) isMax;
    return std::nullopt;
  }

  /**
   * @brief Closed-form location-scale transform @c a·X @c + @c b within
   *        the family.
   *
   * Returns the transformed distribution when the family is closed under
   * these coefficients, @c nullptr when it is not: Exponential / Erlang
   * decline @c a @c <= @c 0 (the support flips) and any non-zero offset
   * (a shifted Erlang leaves the family); every family declines
   * @c a @c == @c 0 (a Dirac is not in-family -- the constant-fold path
   * is responsible for pure constants).
   */
  virtual std::unique_ptr<Distribution> affine(double a, double b) const = 0;

  /** @brief Closed-form @c c·X (affine with no offset). */
  std::unique_ptr<Distribution> scale(double c) const {
    return affine(c, 0.0);
  }

  /** @brief Closed-form @c -X (affine with coefficient -1). */
  std::unique_ptr<Distribution> negate() const {
    return affine(-1.0, 0.0);
  }

  /**
   * @brief The on-disk @c gate_rv @c extra text encoding
   *        (e.g. <tt>"normal:2.5,0.5"</tt>), inverse of
   *        @c parse_distribution_spec.
   */
  virtual std::string serialise() const = 0;

  /**
   * @brief The point-mass value when the parameters make the
   *        distribution degenerate (a Dirac), @c std::nullopt for a
   *        proper distribution.
   *
   * Only Normal reports one (σ == 0, e.g. after an underflowing scale
   * fold); the simplifier collapses such a result to a plain constant.
   * Families whose degenerate forms are still sampled as-is
   * (Uniform(a, a)) do not report.
   */
  virtual std::optional<double> asDirac() const { return std::nullopt; }
};

/**
 * @brief Construct the per-family @c Distribution for a parsed spec.
 *
 * Returns @c nullptr only for an unknown @c DistKind (never for the four
 * built-in families).  Parameter-validity guards live in the family
 * methods (e.g. @c pdf returns NaN for a non-positive σ), matching the
 * pre-refactor behaviour.
 */
std::unique_ptr<Distribution> makeDistribution(const DistributionSpec &spec);

/**
 * @name DistributionRegistry — family descriptor table
 *
 * Family implementations self-register their identity at static
 * initialisation: the @c DistKind tag, the on-disk name token (the part
 * before the colon in a @c gate_rv @c extra), the parameter count, and
 * the constructor.  @c makeDistribution and @c parse_distribution_spec
 * dispatch through this table, so adding a family means one new
 * implementation file with one registrar -- no existing factory or
 * parser code is touched.
 */
///@{

/** @brief Construct a family instance from its (up to) two parameters. */
using DistributionFactory =
  std::unique_ptr<Distribution> (*)(double p1, double p2);

/** @brief A registered family's name / arity descriptor. */
struct DistributionFamily {
  DistKind kind;
  unsigned nparams;   ///< 1 or 2 (a 1-parameter family leaves p2 = 0)
};

/** @brief Register a family; called by the registrar at static init. */
void registerDistributionFamily(DistKind kind, const char *name,
                                unsigned nparams,
                                DistributionFactory factory);

/** @brief Static-initialisation helper: one per family implementation. */
struct DistributionFamilyRegistrar {
  DistributionFamilyRegistrar(DistKind kind, const char *name,
                              unsigned nparams,
                              DistributionFactory factory) {
    registerDistributionFamily(kind, name, nparams, factory);
  }
};

/**
 * @brief Look up a family by its on-disk name token.
 *
 * Used by @c parse_distribution_spec to resolve the kind and expected
 * parameter count; @c std::nullopt for an unknown name.
 */
std::optional<DistributionFamily> lookupDistributionFamily(
  const std::string &name);

///@}

/**
 * @name ComparatorRuleRegistry — pairwise §B.2 closed forms
 *
 * Closed-form @f$P(X < Y)@f$ for an ordered pair of independent RV
 * families.  On continuous distributions every ordered comparator reduces
 * to @f$P(X < Y)@f$ or its complement, so rules are keyed on the family
 * pair alone (no operator in the key).  Pairwise behaviour deliberately
 * stays out of the @c Distribution interface: family files self-register
 * their rules at static initialisation through
 * @ref ComparatorRuleRegistrar, so adding a family touches no existing
 * file, and a missing rule is not an error -- the driver falls back to a
 * family-agnostic quadrature.
 */
///@{

/**
 * @brief A pairwise closed form for @f$P(X < Y)@f$, X and Y independent.
 *
 * Returns NaN when its parameter guards fail (e.g. a non-positive rate);
 * the driver then tries the generic quadrature.
 */
using ComparatorRule = double (*)(const Distribution &X,
                                  const Distribution &Y);

/** @brief Register the @f$P(X < Y)@f$ closed form for a family pair. */
void registerComparatorRule(DistKind x, DistKind y, ComparatorRule rule);

/** @brief Static-initialisation helper: one per registered family pair. */
struct ComparatorRuleRegistrar {
  ComparatorRuleRegistrar(DistKind x, DistKind y, ComparatorRule rule) {
    registerComparatorRule(x, y, rule);
  }
};

/**
 * @brief @f$P(X < Y)@f$ for two independent RVs.
 *
 * Applies the registered closed form for the family pair when there is
 * one; on a registry miss (or a rule declining with NaN) falls back to the
 * 1-D composite-Simpson quadrature
 * @f$P(X<Y) = \int (1 - F_Y(t))\, f_X(t)\, dt@f$ over X's integration
 * range.  NaN when neither decides (a density / CDF is undefined, e.g. a
 * non-integer Erlang shape), so the caller falls back to Monte Carlo.
 */
double comparatorPairLess(const Distribution &X, const Distribution &Y);

///@}

/**
 * @name ClosureRuleRegistry — pairwise family-closure folds on PLUS
 *
 * Closed-form folds of a sum of independent scalar terms into a single
 * distribution (Normal + Normal, same-rate Exponential / Erlang chains).
 * Like the comparator rules, pairwise behaviour stays out of the
 * @c Distribution interface: family files self-register at static
 * initialisation via @ref ClosureRuleRegistrar, keyed on the (ordered)
 * pair of families that may meet in the sum, and a registry miss simply
 * means the sum stays unfolded (Monte Carlo handles it).
 *
 * A rule receives the whole term list rather than reducing pairwise so
 * its accumulation arithmetic (e.g. one variance sum with a single final
 * square root) is not perturbed by intermediate re-serialisation.
 */
///@{

/**
 * @brief One wire of a PLUS under closure: @c a·Z @c + @c b for a base
 *        RV @c Z (@c dist non-null), or a pure additive constant @c b
 *        (@c dist null, @c a @c == @c 0).
 *
 * Structural concerns (base-RV identity, pairwise independence of the
 * @c Z's) are the caller's responsibility; rules only see distributions
 * and coefficients.
 */
struct ClosureTerm {
  const Distribution *dist;
  double a;
  double b;
};

/**
 * @brief A family sum-closure fold.  Returns the closed-form
 *        distribution of the summed terms, or @c nullptr when the shape
 *        is outside the closure (mixed rates, scaled / shifted terms a
 *        family cannot absorb, degenerate variance...).
 */
using ClosureRule =
  std::unique_ptr<Distribution> (*)(const std::vector<ClosureTerm> &terms);

/** @brief Register the sum-closure rule for a family pair. */
void registerClosureRule(DistKind x, DistKind y, ClosureRule rule);

/** @brief Static-initialisation helper: one per registered family pair. */
struct ClosureRuleRegistrar {
  ClosureRuleRegistrar(DistKind x, DistKind y, ClosureRule rule) {
    registerClosureRule(x, y, rule);
  }
};

/**
 * @brief Fold @c PLUS(terms) into a single distribution when a
 *        registered closure covers every family in the sum.
 *
 * Dispatch: the first RV term's family is looked up against itself and
 * against every other RV term's family; all lookups must resolve to the
 * same rule (this is how the Exponential / Erlang pairs share one
 * Erlang-sum rule).  Returns @c nullptr on any miss, on an
 * inconsistent pair, when no RV term is present (the constant-fold
 * path's job), or when the rule itself declines.
 */
std::unique_ptr<Distribution> closePlusTerms(
  const std::vector<ClosureTerm> &terms);

///@}

}  // namespace provsql

#endif  // PROVSQL_DISTRIBUTION_H
