# Exact conjugate-prior posteriors

Feasibility study for recognising **conjugate prior/likelihood structure**
in an `observe`-evidence circuit and computing the latent's posterior in
**closed form** – exactly, deterministically, at `rv_mc_samples = 0` –
instead of likelihood-weighting importance sampling. This is the follow-up
explicitly deferred by [`latent-variables.md`](latent-variables.md) (§A.4
and its "Out of scope" bullet: *"recognising conjugacy and firing a closed
form is an optional simplifier follow-up"*), now assessed against the
shipped Part A/B machinery.

**Verdict: feasible with moderate effort, and unusually high leverage.**
Every prerequisite already exists in the tree:

- The evidence shape is fully structured: `and_agg` builds a `gate_times`
  spine over `gate_observe` atoms, each wrapping a **bare `gate_rv` leaf**
  with the datum in the observe gate's `extra`
  (`sql/provsql.common.sql:5399`; non-leaf arguments are refused).
- The observed leaf's parameters carry their literal-vs-wire structure in
  `DistributionTemplate` / `DistributionParam`
  (`src/RandomVariable.h:57-78`), so "which slot is the latent" is one
  parsed field, not an inference.
- Target and evidence are coupled through the joint circuit, so the shared
  latent is a single `gate_t` – identity of the wired parameter with the
  queried target is plain gate equality.
- The dispatch discipline is established: `conditional_raw_moment`
  (`src/Expectation.cpp:1821`) already tries an exact recogniser
  (`collapsedConditionalMoment`) before the `circuitHasObserve` →
  `importanceSampleConditional` fallback. A conjugacy recogniser is one
  more `std::optional`-returning attempt in the same ladder, with the
  invisible-fallback guarantee preserved (any shape mismatch → nullopt →
  importance sampling, unchanged).
- The registry architecture (`src/distributions/Distribution.h`:
  comparator, sum-closure, product-closure, transform rules) is the exact
  pattern a conjugate-update registry needs: family files self-register
  rules at static initialisation, no shared header or enum grows, and a
  registry miss is a silent decline, not an error.
- The family roster covers all the classic pairs: `normal`, `lognormal`,
  `exponential`, `gamma`/`erlang`, `beta`, `pareto`, `uniform`,
  `inverse_gamma`, plus the parametric discrete leaves `poisson`,
  `binomial`, `geometric`, `negative_binomial`.

The decisive design point: the recogniser's output is a
**`DistributionSpec`** – the posterior *is* a first-class distribution of
a registered family. One recognition therefore upgrades **every** readout
at once (moments, quantile, sample, histogram, entropy, marginal
likelihood), not just the moment that triggered it, because each readout
site simply proceeds as if the target were a bare leaf with the posterior
parameters.

Anchored on:

- [`latent-variables.md`](latent-variables.md) – the shipped `observe` /
  likelihood-weighting surface this accelerates, and the collapsed
  (Rao-Blackwellised) posterior as the shape-recogniser precedent
  (`src/CollapsedAggMoment.{h,cpp}`).
- [`conditioning.md`](conditioning.md) – the `P(Q∧C)/P(C)` reduction and
  the FootprintCache coupling caveats.
- Case study 8, Problem 13 (`doc/source/user/casestudy8.rst`) – the
  motivating workload: a Normal-Normal update whose exact answer the
  prose already derives by hand as ground truth for the MC output.

## Out of scope

- **Multi-latent / hierarchical posteriors** (unknown mean *and*
  variance – NormalGamma –, latents feeding latents, several latents
  coupled by one evidence set). Joint posteriors leave the
  one-`DistributionSpec` carrier; that is Part C / multivariate territory
  ([`continuous_distributions.md`](continuous_distributions.md) §A.2,
  §D.4). The recogniser declines.
- **Non-conjugate exact inference** (quadrature posteriors for arbitrary
  1-D latents). Worth considering someday as a generic 1-D fallback – the
  `PivotIntegration.h` machinery could support it – but it is a different
  feature: the posterior is then *not* a registered family, so it cannot
  ride the `DistributionSpec` carrier and would need a numeric-posterior
  representation. Not here.
- **Truncated priors / mixed truncation evidence on the latent** (a
  `gate_cmp` on the latent conjoined with the observations). The
  truncated-conjugate posterior generally leaves the family (truncated
  Normal is not a registered family). Declines to importance sampling.
- **Soft / reliability-weighted observations**: no surface for them yet
  (see [`conditioning.md`](conditioning.md) §Soft conditioning); when
  tempered likelihoods land they compose with conjugacy for the Normal
  case only – revisit then.

## Plan

### 1. The recogniser: `conjugatePosterior`

New `src/ConjugatePosterior.{h,cpp}`:

```cpp
/* The exact posterior of `target` given `evidence`, as a resolved
 * distribution spec, when the circuit matches the conjugate shape;
 * nullopt on any mismatch (caller falls back to importance sampling). */
std::optional<DistributionSpec>
conjugatePosterior(const GenericCircuit &gc, gate_t target, gate_t evidence);
```

Recognised shape (MVP):

1. **Target** is a bare, all-literal `gate_rv` (the prior; family π,
   parameters from `parse_distribution_spec`).
2. **Evidence** flattens through the `gate_times` spine (the same walk as
   `observe_atoms`) into factors that are **all** `gate_observe` atoms.
   Any other factor – a Boolean event, a `gate_cmp` – declines in the MVP
   (step 5 below relaxes this).
3. Each observed leaf parses to a `DistributionTemplate` with **exactly
   one wired slot**, and that wire is **the target gate itself** (gate
   identity in the joint circuit – not an `arith` composition of it);
   every other slot is literal. The datum comes from the observe gate's
   `extra` (`parseDoubleStrict`).
4. **Fold**: start from the prior spec; for each observation look up the
   rule keyed `(likelihood family, wired slot index, current posterior
   family)` and apply its update to the running parameters. Any lookup
   miss or in-rule guard failure (wrong literal-slot value, datum outside
   the likelihood's support…) declines the whole recognition.

Every listed update is exchangeable, so the left-nested `and_agg` fold
order is irrelevant; and because conjugacy is a property of each
*observation* against the *running* posterior family, mixed likelihoods
sharing one conjugate prior compose for free (a Gamma-prior rate observed
through interleaved Poisson counts and Exponential gaps stays Gamma).

Correctness is by construction identical to the importance-sampling
estimand: the IS weight is exactly `∏ f(dᵢ | θ)` (`evalWeight`), and the
conjugate posterior is `prior × ∏ f(dᵢ | θ)` renormalised – the same
measure, computed exactly. Where the recogniser fires, only the method
changes, never the semantics.

### 2. The registry: conjugate-update rules

Mirroring `ComparatorRuleRegistry` in
`src/distributions/Distribution.{h,cpp}`:

```cpp
struct ConjugateRule {
  /* Update the running posterior parameters (same family as the prior)
   * by one observation.  `lik` carries the observed leaf's family and
   * resolved literal slots; returns false to decline (guard failure). */
  bool (*update)(double &q1, double &q2,
                 const DistributionTemplate &lik, double datum);
  /* Optional: log predictive density log m(datum | q1, q2) BEFORE the
   * update -- accumulates to the exact log marginal likelihood for
   * evidence().  NaN = unavailable (evidence() then keeps its MC path). */
  double (*log_predictive)(double q1, double q2,
                           const DistributionTemplate &lik, double datum);
};
void registerConjugateRule(const char *likelihood_family, int wired_slot,
                           const char *prior_family, ConjugateRule rule);
```

Rules self-register from the family implementation files (the likelihood
family's file owns the rule, consistent with the comparator/closure
convention of name-token keys avoiding static-init order dependence).

**MVP rule table** – all against already-registered families, all
textbook:

| Observed leaf (wired slot) | Prior → posterior | Update per datum `d` |
|---|---|---|
| `normal(θ, σ)` (μ) | `normal(μ₀,σ₀)` → Normal | precision-weighted: `τ ← τ + 1/σ²`, `μ ← (τμ + d/σ²)/(τ + 1/σ²)` |
| `lognormal(θ, σ)` (μ) | `normal` → Normal | same with `ln d` (guard `d > 0`) |
| `exponential(θ)` (λ) | `gamma(k,λ)` → Gamma | `(k+1, λ+d)` (guard `d ≥ 0`) |
| `poisson(θ)` (λ) | `gamma` → Gamma | `(k+d, λ+1)` (guard `d` a non-negative integer) |
| `gamma(k₀, θ)` / `erlang(k₀, θ)` (rate; `k₀` literal) | `gamma` → Gamma | `(k+k₀, λ+d)` |
| `binomial(n, θ)` (p; `n` literal) | `beta(α,β)` → Beta | `(α+d, β+n−d)` (guard `d ∈ {0..n}`) |
| `geometric(θ)` (p) | `beta` → Beta | `(α+1, β+d)` (match the family's failure-count convention) |
| `negative_binomial(r, θ)` (p; `r` literal) | `beta` → Beta | `(α+r, β+d)` |
| `uniform(a₀, θ)` (b; `a₀` literal) | `pareto(xₘ,α)` → Pareto | `(max(xₘ, d), α+1)` (guard `d ≥ a₀`) |
| `pareto(xₘ₀, θ)` (α; `xₘ₀` literal) | `gamma` → Gamma | `(k+1, λ + ln(d/xₘ₀))` (guard `d ≥ xₘ₀`) |

Support-domain guards **decline** rather than raise: an out-of-support
datum falls through to importance sampling, whose zero-weight /
ESS-collapse diagnostics are the established UX for contradictory
evidence.

Deliberately *not* in the MVP table: a prior wired into `normal`'s σ slot
(conjugacy is on σ² / the precision, not σ – see §6), `weibull`'s scale
slot (conjugate only through `λ^{-k}`), `inverse_gaussian`, and
`binomial(n_wired, p_literal)` (no fixed-dimension conjugate family for
`n`).

### 3. Hook sites: one recogniser, every readout

Each site currently reads `if (circuitHasObserve(gc, event)) →
importance sampling`. Insert the recogniser attempt just before, and on
success continue with `makeDistribution(posterior_spec)` (or the existing
bare-leaf analytic path applied to the substituted spec):

1. `conditional_raw_moment` (`src/Expectation.cpp:1835`) – posterior raw
   moments via the family's moment closed forms.
2. `conditional_central_moment` (`src/Expectation.cpp:1871`) – likewise.
3. `compute_quantile` (`src/Expectation.cpp:2036`) – exact quantile via
   `Distribution::quantile` / `numericQuantile`, replacing the weighted
   empirical quantile.
4. `RvSample.cpp:86` – i.i.d. draws from the posterior distribution,
   replacing weighted-particle resampling (`posteriorResample`).
5. `RvHistogram.cpp` (its observe arm) – exact pdf/cdf on the grid.
6. `rv_evidence` – when every rule in the fold supplies
   `log_predictive`, the marginal likelihood is `exp(Σ log m(dᵢ|…))`,
   exact (Normal-Normal: the sequential Normal predictive; Gamma-Poisson:
   negative-binomial pmf factors; Beta-binomial: beta-function ratios via
   `lgamma` – the *predictive family* never needs to be registered, only
   its density value).
7. `InformationTheory` – `entropy(x, prov)` with observe evidence: the
   posterior spec resolves through the existing bare-leaf density view,
   turning the MC histogram plug-in into the exact per-family entropy.

Suggested factoring: sites 1-3 and 6-7 go through a small shared helper
in `ConjugatePosterior.h` so the try-order (`collapsedConditionalMoment`
first where applicable, then conjugacy, then `circuitHasObserve` IS)
stays uniform.

### 4. Tests, documentation, and case-study migration

The feature **ships with** its regression tests, its documentation
updates, and the text adaptation of every case study that exercises the
machinery – none of these are deferrable follow-ups.

New regression test `continuous_conjugate.sql` (added to
`test/schedule.common`), running at `SET provsql.rv_mc_samples = 0` so
any silent fall-through to sampling **fails loudly** – the same
discipline as `continuous_collapsed_posterior`:

- Normal-Normal: exactly the case-study-8 Problem 13 numbers
  (posterior mean `= (μ₀τ₀ + Σdᵢ/σ²)/(τ₀ + n/σ²)`, variance
  `= 1/(τ₀ + n/σ²)`) – now printed to full precision, not an MC
  approximation.
- Gamma-Poisson, Beta-binomial, uniform-Pareto, Pareto-Gamma; the mixed
  Gamma-prior Poisson+Exponential fold; `evidence()` against the
  hand-computed marginal.
- Exact posterior `quantile`, `rv_sample` moments under a seed,
  `rv_histogram` mass, `entropy`.
- **Decline coverage** (with `rv_mc_samples` restored): wired σ slot,
  latent reaching the leaf through `gate_arith`, a Boolean factor in the
  evidence, two distinct latents – all still answered by importance
  sampling within tolerance.

Existing observe-based tests (`continuous_posterior`,
`continuous_latent_usecases`…) whose shapes the recogniser now covers
will change output from seeded-MC values to exact values; the expected
files are updated as part of the feature (the change is the point).

**Documentation obligations:**

- **User docs** (`doc/source/user/continuous-distributions.rst`,
  §Latent variables and posterior inference): a *small* addition – the
  feature is transparent by design (same queries, same surface, answers
  become exact). One short paragraph stating that recognised conjugate
  shapes are computed in closed form (exact, deterministic, working at
  `rv_mc_samples = 0`), with the table of recognised pairs, and that
  everything else keeps the importance-sampling path and its ESS
  diagnostics.
- **Dev docs** (`doc/source/dev/continuous-distributions.rst`): the
  conditional-dispatch ladder description gains the conjugacy attempt
  (its position between `collapsedConditionalMoment` and the
  `circuitHasObserve` importance-sampling fallback), the new registry
  alongside the comparator/closure/transform rule registries, and
  `src/ConjugatePosterior.{h,cpp}` in the file inventory.
- **Case studies using the machinery**: their prose describes MC-flavoured
  outputs ("about 22.85", seed sensitivity, ESS remarks) and must be
  re-read against the now-exact answers. Today that is **case study 8
  only** – Problem 13 first (its text flips from "matches the conjugate
  closed form to MC tolerance" to "is the closed form", and its
  hand-derivation becomes the explanation of what the engine now
  computes), but every cs8 problem touching `observe` / `given` /
  `and_agg` / `evidence` needs the same re-read. The notebook twin
  `studio/provsql_studio/notebooks/cs8.ipynb` mirrors the chapter and is
  adapted in lockstep. Re-grep the case studies at implementation time in
  case a new one has started using the surface (cs2 / cs6 currently match
  "observe" only as ordinary prose).

### 5. Extension: cancelling independent evidence factors  **[Phase 1.5]**

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

### 6. Extensions, each independently optional  **[Later]**

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
  obligations at release time); everything above is C++-internal with
  **zero** SQL / gate-ABI / mmap changes.

## Priorities

1. **Recogniser + registry + MVP rule table + hook sites 1-4 (moments,
   quantile, sample).** The core feature; makes case-study-8 Problem 13
   and every one-latent conjugate workload exact and deterministic.
2. **`evidence()` log-predictives and the histogram / entropy hooks.**
   Small increments over the same fold.
3. **Independent-factor cancellation (§5).** Cheap, unlocks realistic
   mixed-evidence queries.
4. **Affine slots / posterior predictive / σ-slot family / Studio
   surface (§6).** Workload-gated, in whatever order demand dictates.

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
  the IS answer at a large sample budget – a good property-test pattern
  for the regression file.
- **Numerical posture.** Sequential updates are numerically benign
  (parameters grow linearly; the Normal update is a two-term
  precision-weighted mean). `log_predictive` accumulates in log space;
  `rv_evidence` returning `exp(Σ)` underflows for long evidence exactly
  as the MC mean weight does today – not a regression, but worth a doc
  note (a log-evidence readout is a possible later surface).
- **Effort estimate.** Registry plumbing ~80 lines; recogniser + fold +
  readout adapters ~400 lines; ~10 rules at 20-40 lines each in the
  family files; 7 hook edits; one regression file plus expected-file
  refreshes; the documentation and case-study obligations of §4 (short
  user-doc paragraph, dev-chapter dispatch/registry update, cs8 prose and
  notebook adaptation).
  Comparable to, and simpler than, the collapsed-posterior feature
  (`CollapsedAggMoment.cpp` is 672 lines with two quadratures; this is
  arithmetic).
- **No release-time obligations in the core.** No new gate type, no SQL
  functions, no mmap change – the upgrade script is untouched until the
  optional introspection helper (§6) lands.
