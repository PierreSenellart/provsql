/**
 * @file ConjugatePosterior.cpp
 * @brief Implementation of the conjugate-posterior recogniser.
 */
#include "ConjugatePosterior.h"

#include "Circuit.h"                     // CircuitException
#include "distributions/Distribution.h"  // lookupConjugateRule

#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include "provsql_utils.h"
}

namespace provsql {

namespace {

/**
 * @brief Flatten the @c gate_times conjunction spine of @p g into its
 *        @c gate_observe atoms.
 *
 * Multiplicity-preserving (no seen-set): the importance-sampling weight
 * walk multiplies one density factor per wire occurrence, so a repeated
 * atom must fold twice here too.  @c gate_one factors are the times
 * neutral and are skipped.  Returns @c false on any other factor -- a
 * Boolean event, a @c gate_cmp, anything -- declining the recognition.
 */
bool collectObserveAtoms(const GenericCircuit &gc, gate_t g,
                         std::vector<gate_t> &atoms)
{
  const auto type = gc.getGateType(g);
  if (type == gate_observe) {
    atoms.push_back(g);
    return true;
  }
  if (type == gate_one)
    return true;
  if (type == gate_times) {
    for (gate_t c : gc.getWires(g))
      if (!collectObserveAtoms(gc, c, atoms))
        return false;
    return true;
  }
  return false;
}

/** @brief One parsed @c gate_observe atom of the recognised shape. */
struct ParsedObservation {
  DistributionTemplate lik;  ///< The observed leaf's template.
  int wired_param;           ///< Which parameter is the latent (0 = p1, 1 = p2).
  gate_t latent;             ///< The gate the wired slot resolves to.
  double datum;              ///< The observed datum (the observe gate's extra).
};

/**
 * @brief Parse one @c gate_observe atom: a single bare @c gate_rv child
 *        whose template has exactly one wired slot, with the datum in
 *        the observe gate's @c extra.  @c std::nullopt on any mismatch.
 */
std::optional<ParsedObservation>
parseObservation(const GenericCircuit &gc, gate_t obs)
{
  const auto &wires = gc.getWires(obs);
  if (wires.size() != 1)
    return std::nullopt;
  const gate_t leaf = wires[0];
  if (gc.getGateType(leaf) != gate_rv)
    return std::nullopt;

  auto tmpl = parse_distribution_template(gc.getExtra(leaf));
  if (!tmpl || !tmpl->family)
    return std::nullopt;

  /* Exactly one wired slot, and that wire must be the latent gate
   * itself -- not an arith composition of it (the wire points wherever
   * the parameter token resolved to; a composed parameter wires the
   * arith gate, which the caller's target-identity check refuses). */
  const bool w1 = tmpl->p1.wire_slot >= 0;
  const bool w2 = tmpl->p2.wire_slot >= 0;
  if (w1 == w2)
    return std::nullopt;

  const auto &leaf_wires = gc.getWires(leaf);
  const int slot = w1 ? tmpl->p1.wire_slot : tmpl->p2.wire_slot;
  if (slot < 0 || static_cast<std::size_t>(slot) >= leaf_wires.size())
    return std::nullopt;

  ParsedObservation out;
  out.lik = *tmpl;
  out.wired_param = w1 ? 0 : 1;
  out.latent = leaf_wires[slot];

  try {
    out.datum = parseDoubleStrict(gc.getExtra(obs));
  } catch (const CircuitException &) {
    return std::nullopt;
  }
  if (!std::isfinite(out.datum))
    return std::nullopt;

  return out;
}

/**
 * @brief Canonicalise a prior spec into the conjugate registry's carrier
 *        family: the SQL @c gamma constructor stores an integer-shape
 *        prior as an @c erlang leaf, and @c Exponential(λ) is
 *        @c Gamma(1, λ), so rate priors always fold in the @c gamma
 *        carrier.  The substituted spec is the identical distribution;
 *        only the update-rule key (and the readout dispatch) changes.
 */
void canonicalisePrior(DistributionSpec &spec)
{
  const char *n = spec.family->name;
  const bool is_erlang = std::strcmp(n, "erlang") == 0;
  const bool is_exponential = std::strcmp(n, "exponential") == 0;
  if (!is_erlang && !is_exponential)
    return;
  if (const DistributionFamily *g = lookupDistributionFamily("gamma")) {
    if (is_exponential) {
      spec.p2 = spec.p1;
      spec.p1 = 1.0;
    }
    spec.family = g;
  }
}

/**
 * @brief The shared fold: parse every atom, check the common latent /
 *        prior shape, and run the registry updates over the running
 *        posterior spec.  When @p log_evidence is non-null, also
 *        accumulate each rule's log predictive density (declining if
 *        any rule lacks one).  @p target, when supplied, must be the
 *        latent every observation wires; otherwise the latent is
 *        inferred from the first atom.
 */
std::optional<DistributionSpec>
foldConjugate(const GenericCircuit &gc, std::optional<gate_t> target,
              gate_t evidence, double *log_evidence)
{
  std::vector<gate_t> atoms;
  if (!collectObserveAtoms(gc, evidence, atoms) || atoms.empty())
    return std::nullopt;

  std::optional<gate_t> latent = target;
  DistributionSpec post;
  bool have_prior = false;
  double log_m = 0.0;

  for (gate_t obs : atoms) {
    auto o = parseObservation(gc, obs);
    if (!o)
      return std::nullopt;
    if (!latent)
      latent = o->latent;
    else if (o->latent != *latent)
      return std::nullopt;   /* two distinct latents (or not the target) */

    if (!have_prior) {
      /* The prior: a bare, all-literal gate_rv.  A parametric
       * (hierarchical) prior has no constant-parameter spec and
       * declines here. */
      if (gc.getGateType(*latent) != gate_rv)
        return std::nullopt;
      auto prior = parse_distribution_spec(gc.getExtra(*latent));
      if (!prior || !prior->family)
        return std::nullopt;
      post = *prior;
      canonicalisePrior(post);
      have_prior = true;
    }

    const ConjugateRule *rule = lookupConjugateRule(
      o->lik.family->name, o->wired_param, post.family->name);
    if (!rule)
      return std::nullopt;
    if (log_evidence) {
      if (!rule->log_predictive)
        return std::nullopt;
      const double l = rule->log_predictive(post.p1, post.p2,
                                            o->lik, o->datum);
      if (std::isnan(l))
        return std::nullopt;
      log_m += l;
    }
    if (!rule->update(post.p1, post.p2, o->lik, o->datum))
      return std::nullopt;
  }

  if (log_evidence)
    *log_evidence = log_m;
  return post;
}

}  // namespace

std::optional<DistributionSpec>
conjugatePosterior(const GenericCircuit &gc, gate_t target, gate_t evidence)
{
  return foldConjugate(gc, target, evidence, nullptr);
}

std::optional<double>
conjugateLogEvidence(const GenericCircuit &gc, gate_t evidence)
{
  double log_m;
  if (!foldConjugate(gc, std::nullopt, evidence, &log_m))
    return std::nullopt;
  return log_m;
}

}  // namespace provsql
