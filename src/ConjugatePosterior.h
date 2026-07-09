/**
 * @file ConjugatePosterior.h
 * @brief Exact conjugate-prior posteriors for observe-evidence circuits.
 *
 * Recognises conjugate prior/likelihood structure in an @c and_agg
 * evidence conjunction of @c gate_observe atoms and computes the latent's
 * posterior in closed form: the posterior of a bare all-literal @c gate_rv
 * prior, observed through leaves that wire it into one parameter slot,
 * stays in the prior's family with parameters folded one observation at a
 * time through the conjugate-update registry
 * (@c registerConjugateRule in @c distributions/Distribution.h).
 *
 * The recogniser computes exactly the estimand likelihood-weighting
 * importance sampling targets (the IS weight is @f$\prod_i f(d_i \mid
 * \theta)@f$ and the conjugate posterior is the prior times that same
 * product, renormalised), so where it fires only the method changes,
 * never the semantics -- exact, deterministic, and available at
 * @c provsql.rv_mc_samples @c = @c 0.  Any shape mismatch (a Boolean or
 * @c gate_cmp evidence factor, a latent reaching the leaf through
 * @c gate_arith, a registry miss, an out-of-support datum...) declines
 * with @c std::nullopt and the caller falls back to importance sampling
 * unchanged.
 */
#ifndef PROVSQL_CONJUGATE_POSTERIOR_H
#define PROVSQL_CONJUGATE_POSTERIOR_H

#include <optional>

#include "GenericCircuit.h"
#include "RandomVariable.h"

namespace provsql {

/**
 * @brief The exact posterior of @p target given @p evidence, as a
 *        resolved distribution spec, when the circuit matches the
 *        conjugate shape; @c std::nullopt on any mismatch (the caller
 *        falls back to importance sampling).
 *
 * Recognised shape: @p target is a bare all-literal @c gate_rv (the
 * prior); @p evidence flattens through the @c gate_times conjunction
 * spine (the shape @c and_agg builds) into @c gate_observe atoms only
 * (@c gate_one factors are transparently skipped); each observed leaf
 * parses to a @c DistributionTemplate with exactly one wired slot whose
 * wire is @p target itself; and the registry has an update rule for
 * every (likelihood family, wired slot, running posterior family)
 * triple.  Every registered update is exchangeable, so the left-nested
 * @c and_agg fold order is irrelevant, and mixed likelihoods sharing
 * one conjugate prior compose (a Gamma-prior rate observed through
 * interleaved Poisson counts and Exponential gaps stays Gamma).
 */
std::optional<DistributionSpec>
conjugatePosterior(const GenericCircuit &gc, gate_t target, gate_t evidence);

/**
 * @brief The exact log marginal likelihood @f$\ln P(\mathrm{data})@f$ of
 *        a conjugate-shaped @p evidence circuit; @c std::nullopt on any
 *        shape mismatch or when a rule in the fold has no predictive
 *        density (the caller keeps the Monte Carlo mean-weight path).
 *
 * The latent is inferred from the evidence itself: every observed leaf
 * must wire the same bare all-literal @c gate_rv prior.  The result is
 * the sum of each rule's log predictive density of its datum under the
 * running posterior (the sequential chain-rule factorisation of the
 * joint marginal), accumulated in log space.
 */
std::optional<double>
conjugateLogEvidence(const GenericCircuit &gc, gate_t evidence);

}  // namespace provsql

#endif  // PROVSQL_CONJUGATE_POSTERIOR_H
