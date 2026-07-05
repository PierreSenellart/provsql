# Latent variables: RV-valued distribution parameters and posterior inference

**Status.** Part A (forward generative model) and Part B (likelihood-weighting
inference, including B.5 Shapley over evidence) are **shipped**; see
`doc/source/user/continuous-distributions.rst` (§Latent variables and posterior
inference), `doc/source/dev/continuous-distributions.rst`, and the
`continuous_latent` / `continuous_posterior` / `continuous_latent_aggregate` /
`continuous_latent_discrete` / `continuous_latent_usecases` /
`continuous_logistic` / `continuous_collapsed_posterior` regression tests. The
SQL surface is `provsql.observe` / `and_agg` / `evidence` / `shapley_observe`,
the conditioning operator `|` with an equality predicate (`X | (Y = c)`, and
now `R | (Y(R) = C)` against a count `agg_token`), the token-accepting
constructor overloads, and the `provsql.ess_warn_fraction` GUC; the one new
gate type is `gate_observe`. Several extensions landed on top of the original
plan:

- **Exact compound-leaf mean** for families whose mean is affine in their
  parameters (Normal, Uniform, inverse-Gaussian, Logistic, Poisson):
  `E[X] = mean(E[θ])` by linearity, so `expected(normal(uniform(0,1), 1))` is
  exact (no MC), and it composes with the linearity/mixture recursion.
  Authoritative per-family `Distribution::meanIsAffine()` flag.
- **Logistic(μ, s) family** -- the location-scale family whose CDF is the
  sigmoid, so a threshold event over `Logistic(0,1)` noise realises the **logit
  link** exactly (`P(eps < score) = sigma(score)`), the natural link for a
  log-odds / latent-utility selection model (a Normal `eps` gives the probit,
  off by a ~1.6 scale factor). `continuous_logistic` regression test.
- **Discrete rv-parametrized families** `poisson(random_variable)`,
  `binomial(integer, random_variable)`, `geometric(random_variable)` and
  `negative_binomial(r, random_variable)` as parametric `gate_rv` leaves (self-
  registering `Distribution` subclasses; the literal constructors keep
  enumerating an exact categorical). Unblocks the discrete conjugate posteriors
  (Gamma-Poisson, Beta-Binomial, Beta-Geometric, Beta-NegativeBinomial) through
  the same `observe` machinery, with the pmf as the likelihood weight.
  Hypergeometric stays literal-only: its three parameters (N, K, n) do not fit
  the two-parameter `Distribution` ABI (widening it is the one remaining
  discrete family's cost).
- **`random_variable = agg_token` comparison** (an implicit cast -- an
  aggregate IS a random variable) so a latent can be conditioned on a **count**,
  e.g. `R | (poisson(scale*R) = count(*))`, resolving through the ordinary
  comparison operators the planner hook already rewrites.
- **Collapsed (Rao-Blackwellised) inference** -- both the correlated COUNT/SUM
  moments (`CollapsedAggMoment`, `agg_collapsed_moment`) and the **exact
  posterior of a latent conditioned on a correlated count**
  (`collapsedConditionalMoment`): the per-tuple noise marginalises to the count
  pmf (a Poisson-binomial 1-D quadrature over the shared latent), and the latent
  posterior is a second 1-D quadrature weighted by the count likelihood
  `L(r) = sum_j P(C=j) pmf_Y(j; theta(r))`. Exact (works at
  `rv_mc_samples = 0`), `O(n^2 + G K)`, replacing the degenerating
  point-equality importance sampler -- 100 tuples in ~40 ms, 500 in ~90 ms where
  the sampler was hopeless. `continuous_collapsed_posterior` regression test.

**Part C (SMC, then MCMC) remains deferred** and workload-gated, as below. The
collapse above already fires the exact posterior for its recognised shape (one
shared latent, one comparison per row, a discrete rv over the conditioned
latent equalling the count); the remaining open items are **recognising the
collapse / conjugacy structure more broadly** (several shared latents, a SUM
rather than a COUNT in the posterior, hierarchies of latents) and, for the
truly intractable residue, Part C. The release-time obligations in the final
section (upgrade script for `gate_observe` + the new functions, constructors and
families, the `extension_upgrade` canary) are still outstanding and belong to
the next release, not to this feature's development.

This plan covers letting a continuous distribution's **parameters** be
scalar provenance tokens (`gate_rv`, `agg_token`, or a `gate_arith`
composition of them) rather than concrete `double`s, and the inference
engine that direction unlocks. A parameter that is itself a random
variable turns `provsql.normal(M, 1)` with `M ~ normal(0, 10)` into a
**compound (hierarchical) distribution**; conditioning such a leaf on
observed data is **latent-variable posterior inference** in a relational
setting.

The work splits cleanly in two:

- **Part A -- forward generative model.** The prior-predictive
  direction (sample latent, then sample the leaf that depends on it).
  Small, MC-only, no new theory, no `Distribution`-interface change.
- **Part B -- inference engine.** The reverse direction
  (`P(latent | observed = data)`) via **likelihood weighting /
  self-normalised importance sampling**, generalising the shipped
  rejection-based conditioning to continuous-density evidence.

Anchored on:

- The base-independence backbone and the "two independence layers" /
  "per-row hidden auxiliary `gate_rv`" discussion in
  [`continuous_distributions.md`](continuous_distributions.md)
  (§Theoretical backbone, §D.4).
- The conditioning primitive and its explicitly-deferred
  soft/weighted-conditioning item in [`conditioning.md`](conditioning.md)
  -- the likelihood-weighting engine here **is** that deferred item, now
  motivated by a concrete workload.
- §E.1 (Shapley over RV-valued payoffs) in
  [`continuous_distributions.md`](continuous_distributions.md): the
  per-observation posterior-attribution readout that falls out of Part B
  for free.

## Out of scope

- **Observing a derived quantity.** `observe(X + Y, d)` needs a
  change-of-variables (Jacobian) density or an ABC kernel; `observe`
  here binds a datum to a **bare `gate_rv` leaf** only. Approximate
  (kernel) observation of composites is a later, separate feature.
- **MCMC and gradient methods (HMC/NUTS).** Deferred to Part C with
  rationale; the circuit is non-differentiable (`gate_cmp`, mixtures,
  Boolean provenance) and has no point-density evaluation mode, so
  gradient methods are structurally out.
- **Multivariate Normal / copulas** as a *primitive*: a shared latent
  reproduces the correlation informally, but the ergonomic joint
  primitive is [`continuous_distributions.md`](continuous_distributions.md)
  §A.2 / §D.1, not this plan.
- **Analytic conjugate closed forms** (Normal-Normal → Normal,
  Gamma-Poisson → NegBin, Beta-Bernoulli). A parametric leaf falls
  through to MC by design; recognising conjugacy and firing a closed
  form is an optional simplifier follow-up (§A.4), not the MVP. *(Shipped
  for one important shape: the collapsed posterior of a latent conditioned
  on a correlated count -- `collapsedConditionalMoment` -- is exact by
  quadrature and subsumes the Gamma-Poisson / Beta-Binomial update against
  a count; general conjugacy detection remains the follow-up.)*

---

## Part A -- Forward generative model  **[Mid-term]**

### A.1 Gate ABI: parameter wires on `gate_rv`

Today a `gate_rv` is a childless leaf; its parameters live only as
literal doubles in the `extra` text (`"normal:μ,σ"`, parsed by
`parse_distribution_spec`, `src/RandomVariable.cpp:79`). `create_gate`
already appends an arbitrary child vector generically
(`src/MMappedCircuit.cpp:67`), and `gate_rv` is already in the
`extra`-payload gate group on export (`src/MMappedCircuit.cpp:740`), so
**no mmap format-version bump is required**: a `gate_rv` may simply carry
wires, which older circuits never do.

**`extra` grammar extension.** A parameter slot is either a literal or a
wire reference `$i` (0-based index into the gate's wire vector):

```
"normal:$0,1.0"   -- μ from wire 0, σ literal 1.0
"gamma:$0,$1"     -- both parameters from wires
"normal:2.5,0.5"  -- unchanged; back-compatible literal form
```

Only token-valued parameters are wired; literal parameters stay in the
text. This keeps the wire vector minimal and the literal fast-path
byte-identical to today.

**Parsing.** Keep `DistributionSpec` (`src/RandomVariable.h:38`, the
resolved `{family*, double p1, double p2}` POD) **unchanged** -- it stays
the concrete, resolved form that every analytic call site already
consumes. Add a parallel *template* parser:

```cpp
// src/RandomVariable.h
struct DistributionParam {
  double literal;     // used when wire_slot < 0
  int    wire_slot;   // >= 0: resolve from gate's wires[wire_slot]
};
struct DistributionTemplate {
  const DistributionFamily *family;
  DistributionParam p1, p2;
  bool parametric() const;   // any wire_slot >= 0
};
std::optional<DistributionTemplate>
parse_distribution_template(const std::string &extra);
```

`parse_distribution_spec` is then `parse_distribution_template` followed
by a "must be all-literal" check -- so every existing caller keeps
working, and only the new code paths look at wire slots.

### A.2 SQL constructors: token-accepting overloads

Add overloads alongside each numeric constructor
(`sql/provsql.common.sql:3038` etc.). For `normal`:

```sql
-- (random_variable, float8), (float8, random_variable),
-- (random_variable, random_variable); agg_token via its uuid likewise.
CREATE OR REPLACE FUNCTION normal(mu random_variable, sigma double precision)
  RETURNS random_variable AS $$
DECLARE token uuid;
BEGIN
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', ARRAY[(mu).provenance]);
  PERFORM provsql.set_extra(token, 'normal:$0,' || sigma);
  RETURN provsql.random_variable_make(token);
END $$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;
```

The wired child is the scalar gate behind the `random_variable` (its
`provenance` uuid); `agg_token` parameters wire their uuid the same way.
The literal-degenerate reroutes (`sigma = 0` → `as_random`) stay only on
the all-literal overloads. Any scalar subcircuit that `evalScalar`
already handles (`gate_value` / `gate_rv` / `gate_arith` /
`gate_mixture` / `gate_agg` / `gate_case`) is a legal parameter.

### A.3 Sampler: resolve parameter wires per iteration

The whole engine change is in the `gate_rv` arm of `Sampler::evalScalar`
(`src/MonteCarloSampler.cpp:194-206`). Today it builds the
`Distribution` once and caches it forever in `dist_cache_` (never reset,
`:79-82`). For a parametric gate the parameters vary per iteration, so
`dist_cache_` must be bypassed and the two doubles resolved from wires
(which lands them in `scalar_cache_`, so a latent shared across leaves
couples automatically):

```cpp
case gate_rv: {
  auto tmpl = parse_distribution_template(gc_.getExtra(g));   // cache the template, not the Distribution
  double p1 = tmpl->p1.wire_slot < 0 ? tmpl->p1.literal
                                     : evalScalar(wires[tmpl->p1.wire_slot]);
  double p2 = tmpl->p2.wire_slot < 0 ? tmpl->p2.literal
                                     : evalScalar(wires[tmpl->p2.wire_slot]);
  result = tmpl->family->factory(p1, p2)->sample(rng_);        // (double,double) factory unchanged
  break;
}
```

For the all-literal fast path, keep today's `dist_cache_` behaviour
verbatim (build once, reuse). Crucially **`DistributionFactory`,
`DistributionFamily`, and `BaseDistribution` are untouched**
(`src/distributions/Distribution.h:233`,
`src/distributions/DistributionCommon.h:157`): the distribution still has
exactly two scalar parameters; only their *source* changes.

**Parameter-domain policy.** A drawn parameter may violate a family's
support (a sampled `σ ≤ 0`). Do **not** silently drop such draws: that
implicitly truncates the prior and biases every downstream moment (the
same reasoning as the `gate_arith POW` guard,
`src/MonteCarloSampler.cpp:249-293`). Raise a specific, actionable error
("scale parameter drawn ≤ 0; put a positive-support prior on it, e.g.
`gamma`/`lognormal`"), consistent with the total-function / clear-error
convention.

### A.4 Analytic evaluators: fall through to MC (correct by design)

Every closed form in `src/Expectation.cpp` and
`src/AnalyticEvaluator.cpp` assumes a bare leaf with constant `p1,p2`
(`Expectation.cpp:1216`, `:239-258`; `AnalyticEvaluator.cpp:130`). A
parametric leaf must **not** take those paths. Add one guard at each
`gate_rv` analytic entry:

```cpp
if (parse_distribution_template(gc.getExtra(g))->parametric())
    return mc_raw_moment(gc, g, k, ...);   // existing MC fallback
```

This is the invisible-fallback principle: the answer stays correct, only
the method degrades. The compound moment is exact-in-expectation under
MC (e.g. `E[normal(M,1)] = 0`, `Var = 1 + Var(M)` for `M ~ normal(0,σ)`
-- a deterministic-under-seed regression target).

*Optional follow-up (deferred):* a simplifier rule recognising conjugate
shapes (Normal-Normal, Gamma-Poisson, Beta-Bernoulli) and folding them
to a closed-form leaf, restoring the analytic path for the common cases.
Not in the MVP.

### A.5 Tests (Part A)

- Parametric sampling: `expected(normal(normal(0,3), 1))` ≈ 0,
  `variance` ≈ 1 + 9 (seeded MC tolerance).
- Shared-latent coupling: two leaves `normal(M,1)` over the same `M`
  are positively correlated; `covar_pop` ≈ Var(M) (the `scalar_cache_`
  coupling is what makes this hold).
- Domain guard: a prior that can emit `σ ≤ 0` raises the specific error.
- Back-compat: every existing all-literal RV test is byte-identical.

---

## Part B -- Inference engine (likelihood weighting)  **[Architectural]**

Part A is the prior-predictive direction. Inference is
`P(latent | observed = data)`. The engine is **self-normalised
importance sampling**: draw latents from the prior via the existing
forward recursion, weight each draw by the observed leaves' densities at
the data, report weighted posterior moments/samples. It is the
continuous generalisation of the shipped rejection conditioning
(`monteCarloConditionalScalarSamples`, `src/MonteCarloSampler.cpp:666`),
which is importance sampling with 0/1 weights.

### B.1 The `observe` surface and `gate_observe`

Bind a datum to a leaf with a new **append-only** gate type
`gate_observe` (one wire → the observed `gate_rv` leaf; datum in
`extra`). It is an *evidence* node: it composes into an evidence circuit
by `gate_times` exactly like a Boolean conditioning event, but
contributes a continuous density factor instead of a truth value.

```sql
-- observe(X, d): X must be a bare gate_rv leaf. Returns an evidence uuid.
SELECT provsql.expected(
         mu,
         given => provsql.and_agg(provsql.observe(provsql.normal(mu, 1), value)))
FROM   observations;                       -- one leaf per row, all sharing mu
```

`and_agg` conjoins the per-row evidence into one root via `gate_times`
(reusing the conditioning conjunction). This unifies with conditioning:
a Boolean event in the evidence tree contributes a 0/1 weight
(rejection), a `gate_observe` contributes a pdf weight (importance) --
same evidence-conjunction machinery, same `P(query ∧ evidence) /
P(evidence)` normaliser, now weighted. `observe` rejects a non-leaf
argument with a clear error (see Out of scope).

### B.2 The importance-sampling evaluator

New recursion alongside `evalScalar` / `evalBool`, walking the evidence
circuit to a **weight** rather than a scalar or a bool:

```cpp
double Sampler::evalWeight(gate_t g) {          // evidence circuit -> importance weight
  switch (gc_.getGateType(g)) {
    case gate_times:  { double w = 1; for (auto c : wires) w *= evalWeight(c); return w; }
    case gate_observe: {                         // wire[0] = observed leaf, extra = datum d
      double d   = parseDoubleStrict(gc_.getExtra(g));
      auto   tmpl = parse_distribution_template(gc_.getExtra(wires[0]));
      double p1  = resolve(tmpl->p1), p2 = resolve(tmpl->p2);   // couples shared latents via scalar_cache_
      return tmpl->family->factory(p1, p2)->pdf(d);
    }
    default: return evalBool(g) ? 1.0 : 0.0;     // subsumes rejection conditioning
  }
}
```

Top-level loop (a weighted variant of the shipped scalar loop,
`src/MonteCarloSampler.cpp:620`), consuming one joint circuit so the
latent shared between `root` and the evidence is a single `gate_t`
(`getJointCircuit`, `src/CircuitFromMMap.cpp:256`):

```cpp
WeightedPosterior importanceSampleConditional(
    const GenericCircuit &gc, gate_t root, gate_t evidence, unsigned samples) {
  Sampler s(gc, seedRng());
  double sw = 0, sw2 = 0, swx = 0; std::vector<std::pair<double,double>> particles;
  for (unsigned i = 0; i < samples; ++i) {
    s.resetIteration();
    double w = s.evalWeight(evidence);          // evaluate evidence FIRST: fills scalar_cache_ for shared latents
    if (w == 0) continue;
    double x = s.evalScalar(root);              // same caches -> latent draw shared with the weight
    sw += w; sw2 += w*w; swx += w*x; particles.push_back({x, w});
  }
  return { particles, /*mean*/ swx/sw, /*evidence*/ sw/samples, /*ESS*/ sw*sw/sw2 };
}
```

`WeightedPosterior` carries weighted particles (for arbitrary readouts),
the self-normalised posterior mean, the **evidence** `P(data)` (mean raw
weight -- the marginal likelihood, and the same quantity conditioning
computes as `P(C)`), and the **effective sample size** ESS =
`(Σw)² / Σw²`.

### B.3 Posterior readouts: no new dispatcher signatures

The moment/quantile dispatchers already take
`prov uuid DEFAULT gate_one()` (the cross-cutting UI principle:
"conditioning is always a parameter"). Route on the evidence circuit's
content: if it contains a `gate_observe`, dispatch to
`importanceSampleConditional`; otherwise keep today's analytic /
rejection path. So `expected(mu, evidence)`, `variance(mu, evidence)`,
`quantile(mu, f, evidence)`, `rv_sample(mu, n, evidence)` all gain
posteriors with **no surface change**. Posterior predictive is
`rv_sample` on a fresh leaf that reuses the latent.

### B.4 Evidence and the ESS diagnostic

- `provsql.evidence(evidence_uuid)` (or the existing `probability` on a
  weighted evidence root) returns the marginal likelihood `P(data)`.
- Reuse `provsql.rv_mc_samples` for the sample budget. When
  `ESS / accepted` falls below a threshold, emit a `provsql_warning`
  ("posterior effective sample size low (X of N); likelihood weighting
  is degenerating -- raise `rv_mc_samples`, or the model has many
  observations per latent (defer to SMC)"). This keeps the fallback
  invisible until it actually strains, per the MC-UI principle. Optional
  GUC `provsql.ess_warn_fraction` (default e.g. 0.1).

### B.5 Shapley over evidence -- the payoff (§E.1)  **[Research, connecting code]**

Because the importance weight is a **product of per-atom densities**
(`weight = ∏ pdf_atom`), dropping an observation is dropping one factor.
The existing Shapley/Banzhaf machinery over provenance atoms therefore
answers "which observation most shifted my posterior moment" directly
over the `gate_observe` atoms:

```sql
SELECT obs_id, provsql.shapley(posterior_mu, obs_id, payoff => 'expected')
FROM   observations;
```

This is the publishable, ProvSQL-unique explainable-inference angle from
[`continuous_distributions.md`](continuous_distributions.md) §E.1: the
attribution signal is a byproduct of the weights, not a separate
computation. Mostly connecting code once B.1-B.4 land.

### B.6 Tests (Part B)

- **Conjugate ground truth:** Normal-Normal posterior of `μ` given N
  observations has a closed form; the IS posterior mean/variance must
  match within MC tolerance. Beta-Bernoulli likewise.
- **Evidence:** the Normal-Normal marginal likelihood is a known Normal
  density; `evidence(...)` matches.
- **Rejection subsumption:** a purely Boolean evidence tree through the
  new engine reproduces `monteCarloConditionalScalarSamples`.
- **ESS warning** fires as observation count grows; **degenerate**
  (contradictory) evidence yields ESS → 0 with the diagnostic, not a
  silent wrong answer.
- **Shapley:** a dominant outlier observation gets the largest
  posterior-shift attribution.

---

## Part C -- Deferred scale-up: SMC, then MCMC

Likelihood weighting draws latents from the prior, so with many
observations per latent (the relational regime: one latent, N rows) the
weight is a product of N densities and ESS collapses. The mitigation
ladder, in order, deferred until a real workload shows collapse:

1. **Sequential Monte Carlo / particle resampling** *(preferred second
   step)*. Resample particles between observation batches to hold ESS
   up. Still forward-only, no point-density evaluation mode needed;
   natural fit for sequentially-arriving observations and the stochastic
   processes of
   [`continuous_distributions.md`](continuous_distributions.md) §D.2.
2. **MCMC (Metropolis-Hastings / Gibbs)** *(last resort)*. Does not
   degenerate with many observations, but fights the architecture: it is
   **stateful** (the whole sampler is built on `resetIteration()` wiping
   state each draw, `src/MonteCarloSampler.cpp:66`), and its acceptance
   ratio needs the **joint density evaluated at a proposed assignment** --
   precisely the "third evaluation mode" that
   [`continuous_distributions.md`](continuous_distributions.md) §D.4
   notes the RV layer lacks ("never evaluates a joint density at an
   assignment"). It also needs burn-in / R-hat / ESS convergence UX,
   which breaks the invisible-fallback principle. Scope it, if ever, as
   that new point-density evaluation mode, not a bolt-on. Gradient
   methods (HMC/NUTS) are structurally out: the circuit is
   non-differentiable (`gate_cmp`, mixtures, Boolean provenance) with no
   autodiff.

---

## Priorities

1. **Part A -- forward generative model [Mid-term].** Self-contained,
   MC-only, no interface churn, immediately useful (hierarchical priors,
   informal correlation via shared latents). Ship first.
2. **Part B.1-B.4 -- likelihood-weighting inference [Architectural].**
   The core new engine; generalises shipped conditioning and yields the
   marginal likelihood for free. Depends on Part A (observed leaves are
   parametric).
3. **Part B.5 -- Shapley over evidence [Research].** Connecting code
   over B plus the existing Shapley infra; the publishable payoff.
4. **Part C -- SMC then MCMC [deferred].** Workload-gated; do not build
   speculatively.

## Implementation observations

- **`DistributionSpec` stays fixed-two-double.** The forward model needs
  no `Distribution`-interface change; the family still has two scalar
  parameters, only their source (sampled wire vs literal) changes. Resist
  widening the factory to a parameter vector -- that is the §D.4 PC
  subsystem's concern, not this one.
- **`gate_observe` is the one new gate type.** The `provenance_gate`
  enum is append-only and persisted; append `gate_observe` at the end.
  Parameter wires on `gate_rv` need **no** new type and **no** mmap
  format bump (wires are a generic mechanism `gate_rv` simply never
  used).
- **Couple via the joint circuit, always.** Any readout where the latent
  is shared between `root` and evidence must go through `getJointCircuit`
  so the shared leaf is one `gate_t`; otherwise `scalar_cache_` cannot
  couple the weight and the value (`src/CircuitFromMMap.cpp:256`,
  `src/MMappedCircuit.cpp:691`).
- **Release-time obligations (do at release, not during dev).** The new
  `gate_observe` enum value plus every new constructor / `observe` /
  `and_agg` / `evidence` function must be replicated in the
  `sql/upgrades/provsql--<prev>--<new>.sql` script (with the append-only
  enum handled by `ALTER TYPE ... ADD VALUE` and a closing
  `SELECT reset_constants_cache();`), and `test/sql/extension_upgrade.sql`
  should gain one deterministic call (e.g. a Normal-Normal posterior).
  See the release discipline in `CLAUDE.md`.
