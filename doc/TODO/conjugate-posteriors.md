# Exact conjugate-prior posteriors: follow-ups

The core feature -- the `observe`-evidence shape recogniser
(`src/ConjugatePosterior.{h,cpp}`), the conjugate-update registry in
`src/distributions/Distribution.{h,cpp}`, the full rule table with exact
log predictives, and the seven hook sites across `Expectation.cpp` /
`RangeCheck.cpp` / `InformationTheory.cpp` -- is shipped; see
`doc/source/user/continuous-distributions.rst` (§Latent variables and
posterior inference), `doc/source/dev/continuous-distributions.rst`
(§Exact conjugate posteriors), and the `continuous_conjugate` regression
test. This file keeps only the open follow-ups, each independently
optional.

Anchored on:

- [`latent-variables.md`](latent-variables.md) -- the `observe` /
  likelihood-weighting surface this accelerates, and the collapsed
  posterior (`src/CollapsedAggMoment.{h,cpp}`) as the shape-recogniser
  precedent (it stays first in the dispatch ladder).
- [`conditioning.md`](conditioning.md) -- the `P(Q∧C)/P(C)` reduction and
  the FootprintCache coupling caveats.

## Out of scope

- **Multi-latent / hierarchical posteriors** (unknown mean *and*
  variance -- NormalGamma --, latents feeding latents, several latents
  coupled by one evidence set). Joint posteriors leave the
  one-`DistributionSpec` carrier; that is multivariate territory
  ([`continuous_distributions.md`](continuous_distributions.md) §A.2,
  §D.4). The recogniser declines.
- **Non-conjugate exact inference** (quadrature posteriors for arbitrary
  1-D latents). Worth considering someday as a generic 1-D fallback -- the
  `PivotIntegration.h` machinery could support it -- but the posterior is
  then *not* a registered family and would need a numeric-posterior
  representation.
- **Truncated priors / mixed truncation evidence on the latent** (a
  `gate_cmp` on the latent conjoined with the observations). The
  truncated-conjugate posterior generally leaves the family. Declines to
  importance sampling.
- **Soft / reliability-weighted observations**: no surface for them yet
  (see [`conditioning.md`](conditioning.md) §Soft conditioning); when
  tempered likelihoods land they compose with conjugacy for the Normal
  case only -- revisit then.
- **Beta as a likelihood** (deliberately absent from the rule table,
  alongside the σ-slot / Weibull-scale / `binomial(n_wired)` exclusions):
  a Beta likelihood with a latent shape has no fixed-dimension conjugate
  prior among registered families (the `1/B(α, β)` normaliser puts
  Gamma-function factors into the posterior kernel), so it cannot ride
  the `DistributionSpec` carrier. Beta appears as a *prior* in three
  rules.

## Plan

### Cancelling independent evidence factors  **[Phase 1.5]**

Evidence of the form `E_obs × B` where `B` is Boolean and structurally
independent of the block -- `FootprintCache` disjointness of `B` against
`footprint(target) ∪ ⋃ footprint(observed leafᵢ)` -- satisfies
`P(θ | E_obs ∧ B) = P(θ | E_obs)`, so such factors can be dropped rather
than declining. Cheap (the footprint machinery exists) and common (a
data-quality filter conjoined with observations). Per the
[`conditioning.md`](conditioning.md) caveat, any footprint overlap --
however indirect -- must decline: conditioning is the canonical
independence-defeating operation, and (per the sibling-arm-sharing
lesson) the check must run on raw gate footprints, not on any
deduplicated census.

### Later extensions

- **Affine slot matching → 1-D Bayesian linear regression.** Recognise
  the wired slot resolving to an affine `gate_arith` chain `aᵢ·θ + bᵢ`
  over the target with literal coefficients (a RangeCheck-style walk).
  For Normal likelihoods the precision-weighted update generalises
  (`τ += aᵢ²/σᵢ²`…), covering `observe(normal(a*R + b, σ), d)` -- regression
  with known per-row covariates. Similarly, a readout root that is affine
  in the latent applies `Distribution::affine` to the posterior.
- **Posterior predictive as a distribution.** When the readout root is a
  *fresh* parametric leaf over the same latent (not the latent itself),
  a per-rule predictive family closes some cases exactly:
  Normal-Normal → `normal(μₙ, √(σ² + σₙ²))`, Gamma-Poisson →
  `negative_binomial(k, λ/(λ+1))` -- both registered families. Others
  (Beta-binomial) have no registered predictive family and keep MC; their
  *mean* is already exact through the `meanIsAffine` linearity once the
  posterior spec substitutes for the latent.
- **Normal σ-slot conjugacy.** A prior on the σ slot with known mean is
  conjugate on σ⁻² (Gamma) / σ² (inverse-Gamma,
  `src/distributions/inverse_gamma.cpp`), not on σ. Cleanest
  route: one new self-registering family (the distribution of `√Y` for
  `Y ~ InvGamma` -- "root-inverse-Gamma" / scaled inverse-χ), whose
  conjugate update is `α += n/2`, `β += Σ(dᵢ−μ₀)²/2` in the σ carrier.
  One family file + one rule; no evaluator changes, by the family-file
  ethos. Do when a workload asks for variance learning.
- **Studio / introspection surface.** An SQL helper (e.g.
  `posterior_distribution(target, evidence)` returning family + parameter
  text, or an extension of `simplified_circuit_subgraph`) so Studio can
  annotate a conditioned latent with "≡ Normal(22.85, 1.13)". This is the
  only piece that would add SQL surface (and hence upgrade-script
  obligations at release time); everything else is C++-internal.

## Priorities

1. **Phase 1.5 (cancelling independent evidence factors)**: cheap,
   common, the footprint machinery exists.
2. **The later extensions**, each workload-gated: affine slots, posterior
   predictive, σ-slot conjugacy, the Studio introspection surface.

## Implementation observations

- **Why this beats a circuit-rewrite formulation.** A `HybridEvaluator`
  peephole that *rewrites* the conditioned pair into a folded leaf was
  considered and rejected: the conditioned readout is not a circuit node
  (conditioning arrives as a separate `event_root` argument), the fold
  must see the datum list that lives in `gate_observe` extras, and the
  read-time recogniser keeps the store untouched (no new gates minted per
  readout). The `DistributionSpec` return value gives all the reuse a
  rewrite would, without mutating anything.
- **Estimand identity.** The recogniser computes exactly what
  `importanceSampleConditional` estimates (`weight = ∏ pdf(dᵢ)` in
  `evalWeight`); there is no semantic-divergence risk, only variance
  removal. Any doubt about a rule reduces to checking its algebra against
  the IS answer at a large sample budget -- the property-test pattern the
  regression file's decline section uses.
- **Numerical posture.** Sequential updates are numerically benign
  (parameters grow linearly; the Normal update is a two-term
  precision-weighted mean). `log_predictive` accumulates in log space;
  `rv_evidence` returning `exp(Σ)` underflows for long evidence exactly
  as the MC mean weight does today (a log-evidence readout is a possible
  later surface).
