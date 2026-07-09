# Exact conjugate-prior posteriors

**Status.** The core feature (priorities 1 and 2 of the original plan:
recogniser + registry + full MVP rule table + all seven hook sites,
including `evidence()` log-predictives and the histogram / entropy hooks)
is **shipped**; see `doc/source/user/continuous-distributions.rst`
(§Latent variables and posterior inference, recognised-pairs table),
`doc/source/dev/continuous-distributions.rst` (§Exact conjugate
posteriors), case study 8 Problem 13 (now exact; prose and notebook
adapted), and the `continuous_conjugate` regression test (exact pairs at
`rv_mc_samples = 0`, decline coverage validated against importance
sampling). Zero SQL / gate-ABI / mmap changes, so no upgrade-script
obligations.

Shipped shape:

- `src/ConjugatePosterior.{h,cpp}`: `conjugatePosterior(gc, target,
  evidence)` → `std::optional<DistributionSpec>` and
  `conjugateLogEvidence(gc, evidence)` → `std::optional<double>` (the
  latter infers the latent from the evidence itself). Recognised shape:
  bare all-literal `gate_rv` target; evidence flattens through the
  `gate_times` spine (multiplicity-preserving; `gate_one` factors
  skipped) into `gate_observe` atoms only; each observed leaf's template
  has exactly one wired slot whose wire is the target gate. Any mismatch
  → `nullopt` → importance sampling unchanged.
- **Conjugate-update registry** in `src/distributions/Distribution.{h,cpp}`
  (`ConjugateRule` = `update` + `log_predictive`, keyed on
  *(likelihood family, wired parameter position, running posterior
  family)*), self-registered from the likelihood family's file like the
  comparator / closure / transform rules.
- **Rule table** (all with exact log predictives): Normal-Normal (mean
  slot), Normal-LogNormal (log-location, update at `ln d`),
  Gamma-Exponential, Gamma-Poisson, Gamma-Gamma / Gamma-Erlang (rate
  slot, literal shape), Beta-Binomial, Beta-Geometric (the family's
  TRIALS convention: `(α+1, β+d−1)`), Beta-NegativeBinomial (failures),
  Pareto-Uniform (upper bound), Gamma-Pareto (tail shape).
- **Hook sites**: `conditional_raw_moment` / `conditional_central_moment`
  / `compute_quantile` / `rv_evidence` (`src/Expectation.cpp`; the
  attempt sits between `collapsedConditionalMoment` and the
  `circuitHasObserve` IS fallback), `rv_sample` (i.i.d. posterior draws
  through the now-exposed `seedRng`), `matchClosedFormDistribution`
  (`src/RangeCheck.cpp`: the posterior as an untruncated single-RV shape,
  which upgrades `rv_histogram` AND `rv_analytical_curves` / Studio
  curves), and `computeEntropy` (`src/InformationTheory.cpp`).

Deviations from the original plan, all deliberate:

- **Prior canonicalisation**: the SQL `gamma(k, λ)` constructor stores an
  integer-shape prior as an `erlang` leaf, and `Exponential(λ)` is
  `Gamma(1, λ)`, so the fold canonicalises erlang / exponential priors
  into the `gamma` carrier before the rule lookup (identical
  distributions; without this, `gamma(2, 1)` priors were never
  recognised).
- **Pareto-Uniform guards `a₀ = 0`**: the plan's table allowed a literal
  nonzero lower bound, but `1/(θ − a₀)` is Pareto-shaped in `θ` only for
  `a₀ = 0`; a nonzero `a₀` declines (regression-tested).
- **Integer-datum guards mirror the family pmfs**: counts are accepted
  within the same `nearbyint` 1e-9 tolerance the discrete `pdf()`s use,
  so the closed form fires exactly where the IS weight is positive.
- **Beta as a likelihood is deliberately absent** (alongside the plan's
  σ-slot / Weibull-scale / `binomial(n_wired)` exclusions): a Beta
  likelihood with a latent shape has no fixed-dimension conjugate prior
  among registered families (the `1/B(α, β)` normaliser puts
  Gamma-function factors into the posterior kernel), so it cannot ride
  the `DistributionSpec` carrier. Beta appears as a *prior* in three
  rules.

Anchored on:

- [`latent-variables.md`](latent-variables.md) – the `observe` /
  likelihood-weighting surface this accelerates, and the collapsed
  posterior (`src/CollapsedAggMoment.{h,cpp}`) as the shape-recogniser
  precedent (it stays first in the dispatch ladder).
- [`conditioning.md`](conditioning.md) – the `P(Q∧C)/P(C)` reduction and
  the FootprintCache coupling caveats.

## Out of scope (unchanged)

- **Multi-latent / hierarchical posteriors** (unknown mean *and*
  variance – NormalGamma –, latents feeding latents, several latents
  coupled by one evidence set). Joint posteriors leave the
  one-`DistributionSpec` carrier; that is Part C / multivariate territory
  ([`continuous_distributions.md`](continuous_distributions.md) §A.2,
  §D.4). The recogniser declines.
- **Non-conjugate exact inference** (quadrature posteriors for arbitrary
  1-D latents). Worth considering someday as a generic 1-D fallback – the
  `PivotIntegration.h` machinery could support it – but the posterior is
  then *not* a registered family and would need a numeric-posterior
  representation.
- **Truncated priors / mixed truncation evidence on the latent** (a
  `gate_cmp` on the latent conjoined with the observations). The
  truncated-conjugate posterior generally leaves the family. Declines to
  importance sampling.
- **Soft / reliability-weighted observations**: no surface for them yet
  (see [`conditioning.md`](conditioning.md) §Soft conditioning); when
  tempered likelihoods land they compose with conjugacy for the Normal
  case only – revisit then.

## Remaining follow-ups, each independently optional

### Cancelling independent evidence factors  **[Phase 1.5]**

Evidence of the form `E_obs × B` where `B` is Boolean and structurally
independent of the block – `FootprintCache` disjointness of `B` against
`footprint(target) ∪ ⋃ footprint(observed leafᵢ)` – satisfies
`P(θ | E_obs ∧ B) = P(θ | E_obs)`, so such factors can be dropped rather
than declining. Cheap (the footprint machinery exists) and common (a
data-quality filter conjoined with observations). Per the
[`conditioning.md`](conditioning.md) caveat, any footprint overlap –
however indirect – must decline: conditioning is the canonical
independence-defeating operation, and (per the sibling-arm-sharing
lesson) the check must run on raw gate footprints, not on any
deduplicated census.

### Later extensions

- **Affine slot matching → 1-D Bayesian linear regression.** Recognise
  the wired slot resolving to an affine `gate_arith` chain `aᵢ·θ + bᵢ`
  over the target with literal coefficients (a RangeCheck-style walk).
  For Normal likelihoods the precision-weighted update generalises
  (`τ += aᵢ²/σᵢ²`…), covering `observe(normal(a*R + b, σ), d)` – regression
  with known per-row covariates. Similarly, a readout root that is affine
  in the latent applies `Distribution::affine` to the posterior.
- **Posterior predictive as a distribution.** When the readout root is a
  *fresh* parametric leaf over the same latent (not the latent itself),
  a per-rule predictive family closes some cases exactly:
  Normal-Normal → `normal(μₙ, √(σ² + σₙ²))`, Gamma-Poisson →
  `negative_binomial(k, λ/(λ+1))` – both registered families. Others
  (Beta-binomial) have no registered predictive family and keep MC; their
  *mean* is already exact through the `meanIsAffine` linearity once the
  posterior spec substitutes for the latent.
- **Normal σ-slot conjugacy.** A prior on the σ slot with known mean is
  conjugate on σ⁻² (Gamma) / σ² (inverse-Gamma), not on σ. Cleanest
  route: one new self-registering family (the distribution of `√Y` for
  `Y ~ InvGamma` – "root-inverse-Gamma" / scaled inverse-χ), whose
  conjugate update is `α += n/2`, `β += Σ(dᵢ−μ₀)²/2` in the σ carrier.
  One family file + one rule; no evaluator changes, by the family-file
  ethos. Do when a workload asks for variance learning.
- **Studio / introspection surface.** An SQL helper (e.g.
  `posterior_distribution(target, evidence)` returning family + parameter
  text, or an extension of `simplified_circuit_subgraph`) so Studio can
  annotate a conditioned latent with "≡ Normal(22.85, 1.13)". This is the
  only piece that would add SQL surface (and hence upgrade-script
  obligations at release time); everything else is C++-internal.

## Implementation observations (kept for the follow-ups)

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
  the IS answer at a large sample budget – the property-test pattern the
  regression file's decline section uses.
- **Numerical posture.** Sequential updates are numerically benign
  (parameters grow linearly; the Normal update is a two-term
  precision-weighted mean). `log_predictive` accumulates in log space;
  `rv_evidence` returning `exp(Σ)` underflows for long evidence exactly
  as the MC mean weight does today – not a regression (a log-evidence
  readout is a possible later surface).
