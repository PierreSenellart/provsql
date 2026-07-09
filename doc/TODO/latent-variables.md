# Latent variables: remaining inference scale-up

The latent-variable surface -- RV-valued distribution parameters
(compound / hierarchical distributions) and the likelihood-weighting /
self-normalised importance-sampling inference engine over `observe`
evidence, including Shapley over evidence -- is shipped; see
`doc/source/user/continuous-distributions.rst` (§Latent variables and
posterior inference), `doc/source/dev/continuous-distributions.rst`, and
the `continuous_latent*` / `continuous_posterior` / `continuous_logistic`
/ `continuous_collapsed_posterior` regression tests. The SQL surface is
`provsql.observe` / `and_agg` / `evidence` / `shapley_observe`, the
conditioning operator `|`, the token-accepting constructor overloads, and
the `provsql.ess_warn_fraction` GUC; the one gate type is `gate_observe`.

This file keeps the open work: recognising exact-inference structure
more broadly than the shipped recognisers reach, and the deferred
scale-up ladder (SMC, then MCMC) for the truly intractable residue.

Anchored on:

- The base-independence backbone in
  [`continuous_distributions.md`](continuous_distributions.md)
  (§Theoretical backbone, §D.4).
- [`conditioning.md`](conditioning.md) -- likelihood weighting is the
  continuous realisation of the soft/weighted-conditioning direction
  deferred there.
- [`conjugate-posteriors.md`](conjugate-posteriors.md) -- the conjugacy
  branch of the recognition work, with its own follow-up list.

## Out of scope

- **Observing a derived quantity.** `observe(X + Y, d)` needs a
  change-of-variables (Jacobian) density or an ABC kernel; `observe`
  binds a datum to a **bare `gate_rv` leaf** only. Approximate (kernel)
  observation of composites is a later, separate feature.
- **Gradient methods (HMC/NUTS).** Structurally out: the circuit is
  non-differentiable (`gate_cmp`, mixtures, Boolean provenance) and has
  no autodiff.
- **Multivariate Normal / copulas** as a *primitive*: a shared latent
  reproduces the correlation informally, but the ergonomic joint
  primitive is [`continuous_distributions.md`](continuous_distributions.md)
  §A.2 / §D.1, not this plan.

## Plan

### Broader recognition of exact-inference structure

The collapsed (Rao-Blackwellised) posterior
(`collapsedConditionalMoment`, `src/CollapsedAggMoment.{h,cpp}`) fires
for its recognised shape only: one shared latent, one comparison per
row, a discrete rv over the conditioned latent equalling the count. The
conjugate recogniser (`src/ConjugatePosterior.{h,cpp}`) likewise covers
bare-leaf targets with flat `gate_observe` evidence. Open
generalisations, each workload-gated:

- **Several shared latents** coupled by one evidence set (the joint
  posterior leaves the one-`DistributionSpec` carrier; needs a
  numeric-joint or multivariate representation first).
- **A SUM rather than a COUNT** in the collapsed posterior's conditioned
  aggregate.
- **Hierarchies of latents** (latents feeding latents): today the
  recognisers decline and importance sampling takes over.

### Hypergeometric as a parametric family

The rv-parametrized discrete families (`poisson`, `binomial`,
`geometric`, `negative_binomial`) are parametric `gate_rv` leaves;
hypergeometric stays literal-only because its three parameters (N, K, n)
do not fit the two-parameter `Distribution` ABI. Widening the ABI to a
parameter vector is the cost of that one remaining family -- do it only
if a workload asks, and mind that the fixed-two-double
`DistributionSpec` is load-bearing across every analytic call site.

### Part C -- deferred scale-up: SMC, then MCMC

Likelihood weighting draws latents from the prior, so with many
observations per latent (the relational regime: one latent, N rows) the
weight is a product of N densities and ESS collapses. The collapsed and
conjugate exact paths already remove the common cases; for the residue,
the mitigation ladder, in order, deferred until a real workload shows
collapse:

1. **Sequential Monte Carlo / particle resampling** *(preferred second
   step)*. Resample particles between observation batches to hold ESS
   up. Still forward-only, no point-density evaluation mode needed;
   natural fit for sequentially-arriving observations and the stochastic
   processes of
   [`continuous_distributions.md`](continuous_distributions.md) §D.2.
2. **MCMC (Metropolis-Hastings / Gibbs)** *(last resort)*. Does not
   degenerate with many observations, but fights the architecture: it is
   **stateful** (the whole sampler is built on `resetIteration()` wiping
   state each draw), and its acceptance ratio needs the **joint density
   evaluated at a proposed assignment** -- precisely the "third
   evaluation mode" that
   [`continuous_distributions.md`](continuous_distributions.md) §D.4
   notes the RV layer lacks ("never evaluates a joint density at an
   assignment"). It also needs burn-in / R-hat / ESS convergence UX,
   which breaks the invisible-fallback principle. Scope it, if ever, as
   that new point-density evaluation mode, not a bolt-on.

## Priorities

1. **Broader exact-structure recognition** -- extend the collapsed /
   conjugate recognisers when a real workload declines to importance
   sampling and degenerates.
2. **Part C (SMC, then MCMC)** -- workload-gated; do not build
   speculatively.
3. **Hypergeometric ABI widening** -- only on demand.

## Implementation observations

- **`DistributionSpec` stays fixed-two-double** unless the
  hypergeometric item forces the widening; the family has two scalar
  parameters and every analytic call site consumes the resolved POD.
  A parameter *vector* is the §D.4 probabilistic-circuit subsystem's
  concern, not this track's.
- **Couple via the joint circuit, always.** Any readout where the latent
  is shared between `root` and evidence must go through `getJointCircuit`
  so the shared leaf is one `gate_t`; otherwise `scalar_cache_` cannot
  couple the weight and the value.
