# ProvSQL Random Variables — Feature Roadmap

A synthesis of the design discussion around the `continuous_distributions`
branch of [ProvSQL](https://github.com/PierreSenellart/provsql). The branch
introduces first-class continuous random-variable columns, a three-stage
hybrid analytic + Monte Carlo evaluator (`RangeCheck` → `AnalyticEvaluator` →
`Expectation`, with whole-circuit MC as the safety net), and the
`HybridEvaluator` simplifier that folds family-preserving combinations.
This document lists, categorises, and prioritises follow-up features,
with a focus on **applicability** (does it solve a real user problem?)
and **user interface** (does it fit the existing `provsql.<name>(...)` /
infix-operator grammar?).

The prioritisation uses four labels:

| Label | Meaning |
|---|---|
| **[Quick win]** | Small implementation, large user value, fits existing infrastructure cleanly. |
| **[Mid-term]** | Meaningful implementation effort, high value, mostly local extensions. |
| **[Architectural]** | Touches the type system, the gate ABI, or core evaluator algorithms. Opens major new capabilities. |
| **[Research]** | Novel territory, potentially publishable, may need theoretical work first. |

---

## At-a-glance summary

| # | Feature | Category | Priority |
|---|---|---|---|
| 1 | Gamma (+ Chi-squared) | Parametric distributions | Quick win |
| 2 | Log-normal | Parametric distributions | Quick win |
| 3 | Beta | Parametric distributions | Mid-term |
| 4 | Poisson, Binomial, Geometric | Parametric distributions | Mid-term |
| 5 | Multivariate Normal | Parametric distributions | Architectural |
| 6 | Quantiles / inverse CDF | Expressivity completion | Quick win |
| 7 | RV-vs-RV analytical comparisons | Expressivity completion | Quick win |
| 8 | Function application (`log`, `exp`, …) | Expressivity completion | Mid-term |
| 9 | Order-statistic aggregates (`MIN`, `MAX`, percentile) | Expressivity completion | Mid-term |
| 10 | Information-theoretic primitives (entropy, KL, MI) | Expressivity completion | Mid-term |
| 11 | Empirical samples gate | Data-driven distributions | Mid-term |
| 12 | Empirical CDF gate | Data-driven distributions | Mid-term |
| 13 | GMM constructor | Data-driven distributions | Quick win |
| 14 | Frozen-distribution snapshots | Data-driven distributions | Mid-term |
| 15 | Conditioning as a gate | Structural extensions | Architectural |
| 16 | Correlation / copulas | Structural extensions | Architectural |
| 17 | Stochastic processes | Structural extensions | Architectural |
| 18 | Causal interventions (`do`) | Structural extensions | Research |
| 19 | Shapley over RV-valued payoffs | Provenance × probability | Research |
| 20 | Provenance of sampled values | Provenance × probability | Research |
| 21 | Per-distribution class hierarchy (`src/distributions/`) | Internal architecture | Architectural (prerequisite) |

---

## Theoretical backbone: base independence and where it strains

The discrete probabilistic-database consensus is well established: base
tuple-existence events are independent Bernoullis, and arbitrary
correlations are introduced through query operations (joins, unions,
selections) that share Boolean variables across provenance formulas.
The Boolean algebra is closed and clean; weighted model counting handles
probability computation.

The same principle carries over to the continuous setting *in the strict
mathematical sense*. Sklar's theorem combined with the inverse
Rosenblatt transformation gives universality: any joint distribution
over `(X₁, …, X_n)` can be written as a deterministic function of `n`
independent uniforms. Independent base RVs plus a rich enough operation
set is therefore universal, exactly as Boolean operations over
independent Bernoullis are universal in the discrete case.

The mechanism of correlation maps cleanly. Two tuples in discrete PDBs
become correlated when their provenance formulas share a Boolean
variable; two continuous RVs become correlated when their arithmetic
expressions share a base `gate_rv`. The branch's `FootprintCache` is
the continuous analog of "which Boolean variables does this provenance
formula touch" — it tracks which base RVs each arithmetic subtree
depends on, and the structural-independence shortcut on
`gate_arith TIMES` is the continuous version of the disjoint-support
optimisation in Boolean probability computation.

### Where the analogy strains

In the discrete case, shared Boolean variables suffice to compute joint
probabilities by weighted model counting — the algebra is closed and
clean. In the continuous case, shared base `gate_rv`s give you
correlation, but whether you can *exploit it analytically* depends on
the marginal families and the operations involved:

- For **Gaussian marginals**, the function-of-independents trick is
  elegant and preserves analytical structure all the way through. A
  ρ-correlated bivariate Normal is `X = Z₁`, `Y = ρZ₁ + √(1−ρ²) Z₂`
  with `Z₁, Z₂` independent Normals; the simplifier can recognise this
  as Gaussian-coupled and fold subsequent linear combinations using the
  joint covariance. MVN (§A.5) can in principle be a *derived* feature
  rather than a fundamentally new primitive.
- For **non-Gaussian marginals coupled by a non-Gaussian copula**, the
  representation still exists in principle (Sklar + inverse Rosenblatt),
  but the functions to apply are inverse CDFs of compound special
  functions. The simplifier loses sight of the joint structure
  immediately, the analytical paths in `AnalyticEvaluator` do not fire,
  and evaluation falls back to MC — negating the analytical advantage
  that motivated base independence in the first place.

### Architectural choice

This creates a tension that does not really arise in the discrete case:

- **Purist position.** Keep base RVs independent; add only the
  operations needed (function application from §B.3, plus copulas as
  syntactic sugar compiling to inverse Rosenblatt). Universal,
  architecturally clean, faithful to the discrete consensus. Cost: the
  simplifier must be smart enough to recognise "this `gate_arith`
  subtree is really a Gaussian copula in disguise" and route it to
  joint-aware closed forms. That recognition machinery is non-trivial.
- **Pragmatic position.** Add `gate_copula` (and `gate_mvnormal`) as
  primitive gate types. The joint distribution lives in the gate with
  a parametrised family; the simplifier and `AnalyticEvaluator` get
  explicit hooks per copula family. Cost: a new gate type per axis of
  expressivity, and base independence is no longer the unique source
  of correlation.
- **Hybrid.** Both. `gate_copula` exists as a primitive for ergonomic
  UI and for cases with direct analytical handling (Gaussian, t,
  Clayton, Gumbel), but the simplifier can also *decompose* it into
  operations on independents when that path is cleaner (sampling,
  arithmetic over components, marginalisation by projection). The gate
  type acts as a *handle* for the simplifier and as ergonomic surface
  for the user, not as a fundamentally new primitive.

### Two independence layers

In discrete PDBs, "independence of base events" refers to
tuple-existence Bernoullis. In the continuous setting there are *two*
layers — tuple existence (still Boolean provenance, still independent)
*and* per-row RV value draws (independently drawn per row from per-row
parametric distributions). Correlations across rows in the same column
(autoregressive structure, time series; §D.3) and correlations across
columns in the same row (joint Normal returns on the same day) are
both possible and both want first-class expression. The
function-of-independents argument covers both, but the UI for "AAPL
and MSFT are correlated within each trading day" is genuinely awkward
without a primitive — the user has to set up a per-row hidden
auxiliary `gate_rv` that both columns reference, and the simplifier
has to learn to recognise that pattern.

### Adopted position

This roadmap takes the **hybrid stance**. The discrete consensus *does*
carry over in the strict mathematical sense and is worth preserving as
the architectural backbone, so base `gate_rv` instances stay independent
and correlation is in principle introduced through operations that share
them. But `gate_copula` and `gate_mvnormal` (§D.2, §A.5) exist as
recognised sugar — the gate type serves as a *handle* for the
simplifier and as ergonomic surface for the user. This preserves the
discrete consensus philosophically while paying the UI and
simplifier-engineering cost the continuous setting demands.

---

## A. Parametric distributions

The branch supports Normal, Uniform, Exponential, Erlang, Categorical,
Mixture, and Dirac. The following extend that set, prioritised by ratio
of analytical leverage to implementation cost.

### A.1 Gamma (and Chi-squared as a special case) — **[Quick win]**

Erlang is already Gamma with integer shape; the regularised lower
incomplete gamma CDF is already in the codebase. Relaxing `k` to non-integer
gives Gamma in full generality, and Chi-squared falls out as `Gamma(k/2, ½)`.
Sums of Gammas with equal rate close.

**Applicability.** Waiting times, rainfall, financial returns, Bayesian
posteriors for Poisson rates (Gamma is the conjugate prior). Chi-squared
is the foundation of most frequentist hypothesis tests.

**UI.**
```sql
SELECT provsql.gamma(2.5, 0.4);
SELECT provsql.chi_squared(3);    -- syntactic sugar for gamma(k/2, 0.5)
```

**Sequencing.** Gamma is the natural proof-of-concept for the
per-distribution refactor in §F.1: under the current layout it lands
as a coordinated patch across seven `switch`-on-`DistKind` sites; under
the post-refactor layout it lands as one new file in
`src/distributions/` plus a single registry entry.  Doing the refactor
first absorbs the migration of the four existing families with the
test suite as the regression net, then exercises the new interface
with Gamma before it has to absorb a wider family set.

### A.2 Log-normal — **[Quick win]**

Composes with the existing Normal infrastructure: `log(X)` of a log-normal
is Normal, so products of log-normals fold via Normal's linear-combination
closure in log-space. The simplifier gains a multiplicative-closure ruleset
that mirrors Normal's additive one.

**Applicability.** File sizes, network traffic, biological measurements,
financial returns (geometric Brownian motion), any positive-valued
multiplicative process.

**UI.**
```sql
SELECT provsql.lognormal(0.0005, 0.02);

-- Compounded multi-day return: PRODUCT of log-normals folds analytically
SELECT expected(product(daily_return))
FROM asset_returns WHERE asset = 'AAPL';
```

### A.3 Beta — **[Mid-term]**

Bounded support on `[0,1]`. CDF is the regularised incomplete beta, dual
to the Gamma machinery. Conjugate with Bernoulli/binomial, pairing
naturally with the conditioning-gate work below.

**Applicability.** Click-through rates, conversion rates, estimated
probabilities, any bounded proportion. A/B testing.

**UI.**
```sql
INSERT INTO variant_ctr VALUES
  ('A', provsql.beta(45, 120)),
  ('B', provsql.beta(72, 88));

-- P(B beats A) — gate_cmp on two Betas; the closed form requires the
-- pairwise RV comparator work in §B.2 to stay analytical.
SELECT probability_evaluate(provenance())
FROM variant_ctr a, variant_ctr b
WHERE a.variant = 'A' AND b.variant = 'B' AND b.ctr > a.ctr;
```

### A.4 Discrete families: Poisson, Binomial, Geometric — **[Mid-term]**

Categorical handles fully-enumerated outcomes; these three are the
analytical workhorses. Poisson sums close, fixed-`p` Binomial sums
close, Geometric is the discrete cousin of Exponential and inherits
memorylessness (the `gate_cmp` truncation path already exploits this for
Exponential).

**Applicability.** Counts (arrivals, events per interval), success
counts in fixed trials, waiting-times-in-trials. The bread and butter of
operational analytics.

**UI.**
```sql
INSERT INTO store_traffic VALUES
  (9,  'mon', provsql.poisson(15.2)),
  (10, 'mon', provsql.poisson(22.7)),
  (11, 'mon', provsql.poisson(28.1));

-- Sum-closure of Poisson folds to Poisson(66.0) at simplifier time
SELECT expected(sum(arrivals))
FROM store_traffic WHERE day = 'mon' AND hour BETWEEN 9 AND 11;
```

### A.5 Multivariate Normal — **[Architectural]**

Every operation closes: linear combinations of MVN are MVN, marginals
are MVN, conditional of MVN given MVN is MVN. Unlocks correlation — the
single biggest expressive gap in the current setup.

**Framing.** Per the hybrid position in §Theoretical backbone, MVN is
*derivable* from base independence: the Cholesky decomposition
`Y = L Z` (with `Z` i.i.d. standard Normals and `L Lᵀ = Σ`) expresses
any MVN as a linear combination of independent base RVs, and the
simplifier can recognise that pattern and fold linear combinations
using the joint covariance. The MVN constructor therefore lands as a
recognised sugar that compiles to operations on independent base
`gate_rv`s — a *handle* for the simplifier and ergonomic surface for
the user, not a fundamentally new primitive.

**Cost.** The `random_variable` type currently scalarises. MVN wants
vectors and covariance matrices threaded through `gate_arith`, and the
simplifier needs the Cholesky-recognition rule. The gate ABI should be
designed knowing this is coming, even if the implementation lands later.

**Applicability.** Portfolio risk, sensor fusion, any physical process
where variables correlate. The bridge to formal joint modelling.

**UI.** Table-returning constructor that produces one row of named
`random_variable` columns sharing an internal covariance reference:
```sql
INSERT INTO stocks (trade_date, AAPL, MSFT, GOOG)
SELECT '2026-05-14', AAPL, MSFT, GOOG
FROM provsql.mvnormal(
  μ => ARRAY[185.0, 412.0, 178.0],
  Σ => ARRAY[[2.5, 1.8, 1.6],
             [1.8, 3.1, 1.9],
             [1.6, 1.9, 2.2]],
  names => ARRAY['AAPL', 'MSFT', 'GOOG']
);

SELECT expected(0.5*AAPL + 0.3*MSFT + 0.2*GOOG),
       variance(0.5*AAPL + 0.3*MSFT + 0.2*GOOG)
FROM stocks WHERE trade_date = '2026-05-14';
```

### Cautions

- **Cauchy and other stable distributions** are tempting (closure
  under sum is rare) but have undefined moments — breaks the
  `Expectation` semiring contract. Either make moment evaluation
  partial or detect-and-error on those subtrees.
- **Student's t and F** are quotients of RVs; really useful only once
  the analytical evaluator exploits `gate_arith` division as a
  first-class case.

---

## B. Expressivity completion

Cases where the analytical machinery has a natural fit but no current
surface.

### B.1 Quantiles and inverse CDF — **[Quick win]**

Currently only `expected`, `variance`, `moment`, `central_moment`,
`support` exist. Quantiles are a clear gap. Every closed-form family
has an analytical inverse CDF (Normal via Beasley-Springer-Moro is
already in the branch; Exponential and Uniform trivially; Gamma/Beta
via root-finding on the regularised incomplete special functions).
Empirical samples just sort and look up.

**Applicability.** Median, percentiles, Value-at-Risk, credible
intervals — anywhere "what value would I exceed with probability p"
matters. Required for finance and risk applications.

**UI.** Polymorphic dispatcher alongside `expected`:
```sql
SELECT provsql.quantile(posterior, 0.025) AS lower_95,
       provsql.quantile(posterior, 0.975) AS upper_95
FROM model_posteriors WHERE param = 'mu_revenue';
```

`quantile` is currently absent from every per-family code path,
so it is a natural method to land on the abstract `Distribution`
interface introduced by the §F.1 refactor: the SQL dispatcher
becomes one virtual call regardless of family, instead of a fresh
switch on `DistKind` paralleling the existing PDF/CDF/moment
switches.

### B.2 RV-vs-RV analytical comparisons — **[Quick win]**

`gate_cmp` handles `X < c` brilliantly. `X < Y` for two RVs goes to MC
even when there's a clean form — Normal-Normal has
`P(X<Y) = Φ((μ_Y − μ_X) / √(σ_X² + σ_Y²))`, Exponential-Exponential has
`P(X<Y) = λ_X / (λ_X + λ_Y)`.

**Applicability.** A/B tests, rankings, tournament probabilities, "which
sensor is reading higher" — any "which is bigger" comparison.

**UI.** No new surface; pairwise RV comparators are intercepted at plan
time exactly like `X < c`. The work is adding lookup-table entries in
`AnalyticEvaluator`.

### B.3 Function application beyond +, −, ×, ÷ — **[Mid-term]**

`gate_arith` covers arithmetic. There's no `log`, `exp`, `sqrt`, `abs`,
`pow`, or general monotonic transform. This bites immediately:

- `log(lognormal) → normal` and `exp(normal) → lognormal` are the
  natural bridges between the two families; without them the simplifier
  cannot fold either direction.
- The probability integral transform `F_X(X) → Uniform(0,1)` has no
  expression.
- Box-Cox-style transforms aren't expressible.

**UI.** A `gate_apply(rv, op)` with a whitelist of known-closure
transforms (MC fallback otherwise). Monotonic transforms preserve CDF
structure, so `RangeCheck` extends naturally.
```sql
SELECT expected(log(asset_return))   -- log of lognormal → Normal
FROM positions;
```

### B.4 Order-statistic aggregates — **[Mid-term]**

`MIN`, `MAX`, percentile aggregates over `random_variable` are listed as
deferred in the 1.5.0 release notes. The theory is clean:
`P(min < c) = 1 − ∏(1 − F_i(c))` for any family, and exponential-family
min/max have especially nice closures (min of i.i.d. exponentials is
exponential at the summed rate).

**Applicability.** Survival analysis (time-to-first-failure), competitive
events, SLA tail-latency analysis.

**UI.** No new syntax; promotes `MIN`/`MAX`/`percentile_cont` to RV-aware
versions exactly like the existing `SUM`/`AVG`/`PRODUCT` aggregates.

### B.5 Information-theoretic primitives — **[Mid-term]**

Entropy, KL divergence, mutual information aren't expressible. All have
closed forms for major families (Normal-Normal KL is famously clean) and
obvious MC fallbacks.

**Applicability.** Model comparison, variable importance, feature
selection — the language of ML evaluation.

**UI.**
```sql
SELECT provsql.entropy(prior),
       provsql.kl(posterior, prior),
       provsql.mutual_information(X, Y)
FROM ...;
```

---

## C. Empirical / data-driven distributions

The integration point for ML-fitted, simulation-derived, or
sample-based distributions. Two gate types cover essentially everything;
specific constructors smooth the common cases.

Under the §F.1 refactor, the empirical gates land as ordinary
`Distribution` subclasses (`EmpiricalSamples`, `EmpiricalCdf`, `Gmm`)
alongside the analytical ones — same virtual interface, same
registry, but the `pdf`/`cdf`/`quantile`/`sample`/moment methods are
backed by sorted arrays, piecewise-linear tables, or mixture
dispatch instead of closed forms.  Call sites in the evaluators stay
oblivious to which storage strategy a given gate uses.

### C.1 GMM constructor — **[Quick win]**

Gaussian mixtures decompose internally into `gate_mixture` over
`gate_rv:normal` children — both already exist. A direct constructor
just packages the common pattern.

**Applicability.** Output of EM-fitting routines, variational autoencoder
priors, density estimation. Probably the single most commonly-fitted
distribution in applied ML.

**UI.**
```sql
INSERT INTO customer_ltv VALUES (
  'segment_A',
  provsql.gmm(
    weights => ARRAY[0.3, 0.5, 0.2],
    μ       => ARRAY[120.0, 380.0, 1200.0],
    σ       => ARRAY[40.0, 90.0, 250.0]
  )
);
```

### C.2 Empirical samples gate — **[Mid-term]**

A new `gate_empirical_samples(arr float8[])`. CDF via sorted-array
binary search (compute once during the simplify-on-load pass), PDF via
KDE on request. Comparisons against a constant become exact Bernoulli
leaves immediately ("fraction of samples below c") — analytical, not MC,
for that one variable. Moments come for free from the samples.

**Applicability.** MCMC posteriors, variational inference samples,
normalising-flow outputs, bootstrap distributions — anything an ML
pipeline can dump as a sample bundle.

**UI.**
```sql
INSERT INTO model_posteriors VALUES (
  'mu_revenue',
  provsql.empirical_samples(ARRAY[3.21, 3.18, 3.24, /* ... */])
);

-- Bulk load via array_agg over a sample table
INSERT INTO model_posteriors
SELECT param, provsql.empirical_samples(array_agg(value ORDER BY sample_idx))
FROM mcmc_chain
GROUP BY param;
```

### C.3 Empirical CDF gate — **[Mid-term]**

A new `gate_empirical_cdf(grid float8[], cdf float8[])`. Piecewise-linear
CDF table. Sampling is one inverse-CDF lookup; moments via numerical
integration over the grid. Truncation is closed (clip the grid).

**Applicability.** Physical simulation outputs (climate, fluid, particle
codes), risk-model output tables, expert-elicited CDFs.

**UI.**
```sql
INSERT INTO weather_forecast VALUES (
  'paris', '2026-05-15',
  provsql.empirical_cdf(
    grid => ARRAY[0.0, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0],
    cdf  => ARRAY[0.32, 0.51, 0.67, 0.82, 0.94, 0.99, 1.0]
  )
);
```

### C.4 Frozen-distribution snapshots — **[Mid-term]**

`provsql.snapshot(rv, n_samples => 10000)` materialises a complex
`gate_arith` subtree as a frozen `gate_empirical_samples`. A deliberate
trade of analytical fidelity for query-time performance. The provenance
lineage of *which subtree was frozen and when* is preserved naturally —
a side benefit unique to a provenance-aware system.

**Applicability.** Hot paths where the same expensive composite RV is
queried repeatedly; ad-hoc exploration; checkpointing.

**UI.**
```sql
UPDATE forecasts
SET demand = provsql.snapshot(demand, n_samples => 10000)
WHERE expensive_to_evaluate;
```

### Callback gates — **explicitly not recommended**

A `gate_callback(pl_function_oid)` that dispatches sampling/CDF queries
to PL/Python or PL/R would buy live PyTorch/JAX/Stan integration, but it
punctures parallel safety, breaks mmap snapshot semantics (the function
can be redefined under you), and makes circuits non-portable. Worth
offering as an escape hatch only; the sample-bundle route should be the
default.

---

## D. Structural extensions

Larger architectural moves that open new classes of query.

### D.1 Conditioning as a gate — **[Architectural]**

The most architecturally interesting addition, and the closest to "free"
given the branch's existing infrastructure.

A `gate_conditioned(rv_subcircuit, bool_subcircuit)` meaning "the
distribution of `rv` restricted to the event where `bool` holds." Self-
contained: flows through any subsequent operation. Sampling is rejection
(already the MC fallback's behaviour when `prov` is passed); analytical
evaluation reuses the existing closed-form paths because they are
already conditional internally.

**Pipeline placement.**

- **Simplifier** gets `cond(cond(X, A), B) → cond(X, A ∧ B)`,
  `cond(X, true) → X`, and `cond(X, A) → X` when A is independent of
  X's footprint (the `FootprintCache` already gives you this).
- **RangeCheck** treats `cond(X, X ∈ [a,b])` as truncation — the
  current closed-form-truncated path becomes the *specialisation* of a
  general `gate_conditioned` rule rather than a parallel codepath.
  Two near-parallel codepaths collapse into one.
- **AnalyticEvaluator** picks up conditional CDFs where they exist;
  conditioning on independent events factors as `P(A) × (unconditional CDF)`.
- **Expectation** semiring: every dispatcher that takes
  `prov uuid DEFAULT gate_one()` becomes the special case "no explicit
  conditioning gate at the root."
- **FootprintCache** caveat: `cond(X, A)` has effective footprint
  `footprint(X) ∪ footprint(A)`. The structural-independence shortcut on
  `gate_arith TIMES` must back off accordingly. *This is the one
  soundness risk and should land with a regression test that constructs
  `cond(X, A) * cond(Y, A)` and confirms the shortcut does not fire.*

**New directions it opens.**

- **Materialised conditional tables.** Store `cond(rv, evidence)` in a
  regular `random_variable` column and drop the source tuples. Solves
  the "carrying both the distribution and its conditioning" problem.
- **Sequential Bayesian updates.** Each piece of evidence is another
  `cond(..., new_event)` wrap; the `A ∧ B` fold avoids depth blow-up.
- **Truncation generalises** to the canonical degenerate case of
  same-RV-comparator conditioning.
- **Shapley over evidence** (see §E.1).

**UI.** A `provsql.condition(rv, event_uuid)` function, plus optionally
an infix `|` operator reading as "given":
```sql
-- Bayesian update with materialisation
UPDATE patient_risk
SET risk = provsql.condition(
             risk,
             (SELECT provenance() FROM tests
              WHERE patient_id = 1 AND result = 'positive')
           )
WHERE patient_id = 1;

-- Operator sugar — RangeCheck recognises same-RV bool as truncation
SELECT expected(measurement | (measurement > 0.5))
FROM sensor_readings;

-- Recursive Bayesian update over an evidence log
WITH RECURSIVE updates(step, dist) AS (
  SELECT 0, provsql.normal(0, 10)
  UNION ALL
  SELECT step + 1, provsql.condition(dist, e.evidence_token)
  FROM updates u, evidence_log e
  WHERE e.confirmed AND e.step_idx = u.step + 1
)
SELECT expected(dist), variance(dist)
FROM updates WHERE step = (SELECT max(step) FROM updates);
```

### D.2 Correlation / copulas — **[Architectural]**

The biggest expressive hole *practically*, though not theoretically —
see §Theoretical backbone. The current model treats RVs as independent
unless they share leaves via `gate_arith`; in principle that is
universal (Sklar + inverse Rosenblatt), but in practice non-Gaussian
correlations expressed as functions of independents lose analytical
tractability immediately. This roadmap takes the hybrid position:
`gate_copula` exists as a recognised sugar layer over the
function-of-independents construction, with explicit decomposition
rules.

**Mechanism.** A `gate_copula` connecting marginal RVs via Gaussian /
t / Clayton / Gumbel copulas, each carrying:

- a *joint-aware closed form* used by `AnalyticEvaluator` when the
  query asks for joint CDFs, moments, or comparisons that the family
  handles directly;
- a *decomposition rule* rewriting the gate as an arithmetic expression
  over independent base `gate_rv`s, used by the simplifier whenever the
  decomposed form is more amenable to subsequent analysis (sampling,
  marginalisation by projection, composition with operations that
  don't have a direct joint-aware path).

MVN (§A.5) is the Gaussian-copula special case where marginals are
also Normal, and its Cholesky decomposition is the same kind of rule.

**Interaction.** Coupled RVs are explicitly dependent — the
`FootprintCache` structural-independence shortcut backs off, exactly
as for conditioning. The footprint of a `gate_copula(X, Y, …)` is the
union of `X`'s and `Y`'s footprints *plus* a shared auxiliary
footprint representing the copula's hidden dependence, so two
copula-coupled RVs are correctly recognised as non-independent even
when their marginal footprints are otherwise disjoint.

**Applicability.** Portfolio risk, sensor fusion, joint physical
processes. Without this (or a heroic simplifier), real-world joint
modelling is impractical even though it remains mathematically
expressible.

**UI.**
```sql
-- Couple two already-built marginal RVs
SELECT provsql.couple(X, Y,
         copula => 'gaussian',
         params => ARRAY[0.7]) AS XY
FROM marginals;

-- Or: full multivariate construction (the MVN route from §A.5)
```

### D.3 Stochastic processes — **[Architectural]**

Nothing in the current model speaks to AR(1), random walks, Brownian
motion, Markov chains. Hand-building via recursive CTE + conditioning
gates is possible but awkward and gives up analytical handling.

**Mechanism.** First-class constructors returning
`SETOF random_variable` indexed by step. Internally they compile to
correlated `gate_rv` chains using the §D.2 copula machinery.

**Applicability.** Time-series forecasting, path-dependent financial
options, queueing models, epidemiological compartment models — the
provenance circuit already represents the dependency structure; what is
missing is the language to construct chains compactly.

**UI.**
```sql
WITH walk AS (
  SELECT t, value
  FROM provsql.brownian(sigma => 1.0, steps => 100)
)
SELECT expected(value), variance(value)
FROM walk WHERE t = 50;

-- AR(1): X_t = φ X_{t-1} + ε_t,  ε_t ~ N(0, σ²)
SELECT t, value FROM provsql.ar1(phi => 0.85, sigma => 0.2,
                                  x0 => 0.0, steps => 50);
```

### D.4 Causal interventions (`do`-calculus) — **[Research]**

ProvSQL is unusually well-positioned here: the provenance circuit *is* a
DAG, gates are explicit, and severing incoming edges is mechanically
simple. A `provsql.intervene(rv, value)` gate replaces a sub-circuit
with a fixed value while leaving downstream consumers in place — giving
you Pearl's `do(X := x)`.

**Combined with conditioning (§D.1)**, that's observational vs
interventional probability — the core counterfactual machinery. No
other relational system offers this, and it places ProvSQL in
conversation with the causal-inference literature.

**Applicability.** Treatment-effect estimation, what-if analyses,
policy evaluation, the entire counterfactual-reasoning toolkit.

**UI.**
```sql
-- P(Y | do(X = 1))  vs  P(Y | X = 1)
SELECT probability_evaluate(provenance())
FROM model
WHERE provsql.intervene(X, 1.0) AND Y > threshold;
```

---

## E. Provenance × probability — ProvSQL-specific directions

Capabilities that exploit the provenance circuit specifically. Almost
no other system can offer them.

### E.1 Shapley over RV-valued payoffs — **[Research]**

The existing Shapley/Banzhaf machinery over Boolean provenance
generalises directly to "contribution of evidence atom *e* to posterior
moment *m*". With the conditioning gate (§D.1) plus the existing
Shapley infrastructure, this is mostly *connecting code*, not new theory.

**Applicability.** Explainable Bayesian inference in a relational
setting — "which observations most shifted my posterior?" This is a
publishable angle that builds on existing ProvSQL infrastructure rather
than competing with PPL systems.

**UI.**
```sql
SELECT evidence_id,
       provsql.shapley(posterior_risk, evidence_id, payoff => 'expected')
FROM posteriors, evidence_atoms
WHERE patient_id = 1;
```

### E.2 Provenance of sampled values — **[Research]**

When MC sampling fires, can a user ask "which gates' draws produced this
sample"? Currently opaque. A per-sample provenance trace exposed via
`rv_sample_with_witness(...)` would be unique to a provenance-aware
system and concretely useful for debugging surprising tail behaviour.

**Applicability.** Debugging unexpected outputs, root-cause analysis on
risk-model outliers, regulatory explainability ("why did this model
predict this?").

**UI.**
```sql
SELECT value, witness  -- witness is a uuid into the per-sample trace
FROM provsql.rv_sample_with_witness(loss_distribution, n => 1000)
WHERE value > 1e6;     -- inspect the gates that drove the tail samples
```

### E.3 Sampling under constraints with witness extraction — **[Research]**

A close relative of E.2. Rejection sampling already happens for
conditioning. The bool gate that *accepted* a sample is itself a
provenance object — returning it alongside the sample lets users inspect
the rejection witness, which is sometimes more informative than the
sample itself.

---

## F. Internal architecture

Refactors that pay no immediate user-visible dividend but reshape the
codebase so the feature work in §§A–E lands as additions rather than
edits to scattered switch statements.  None of them change the gate
ABI or the on-disk encoding; they are correct iff the existing
`test/` suite still passes.

### F.1 Per-distribution class hierarchy in `src/distributions/` — **[Architectural, prerequisite for §§A.1–A.5, B.1, B.3–B.5, C.1–C.4]**

**The expression problem, in the small.** The current `gate_rv` family
set (Normal, Uniform, Exponential, Erlang) is encoded as a `DistKind`
enum, and every operation that needs to discriminate on it opens a
`switch` and handles each kind inline:

- analytical PDF, CDF, and the `X < c` decision rule in
  `AnalyticEvaluator.cpp`,
- truncation support and bound recovery in `RangeCheck.cpp`,
- inverse-CDF / direct sampling in `MonteCarloSampler.cpp`,
- closed-form moments in `Expectation.cpp` and the parsing layer in
  `RandomVariable.cpp`,
- plot-range selection in `RvAnalyticalCurves.cpp`,
- family-closure folding rules in `HybridEvaluator.cpp`.

This is row-major: operations are grouped, families are scattered.  At
the four families the branch ships with, the pattern is tolerable; at
the twelve-plus families implied by §§A and C it becomes the dominant
cost of every feature in §A, and a meaningful share of §§B–C.  Adding
Gamma under the existing layout is a coordinated patch across seven
files; the patch is mostly mechanical but has to be reviewed in full
each time because the cases interleave with non-family-specific logic.

**Proposal.** A `src/distributions/` directory with one
`<name>.{cpp,h}` per family — Normal, Uniform, Exponential, Erlang
to start; Gamma, LogNormal, Beta, Poisson, Binomial, Geometric,
EmpiricalSamples, EmpiricalCdf, Gmm as they land.  Each subclass of an
abstract `Distribution` interface implements the methods the existing
call sites need:

- `support()` returning a structured `Support` (whole line, half-line,
  closed interval, discrete enumeration), encapsulating the bounds
  that `RangeCheck` currently reconstructs from `DistKind` cases.
- `pdf(x)`, `cdf(x)`, `log_pdf(x)` for `AnalyticEvaluator`.
- `quantile(p)` for §B.1, currently absent — landing it on the
  interface makes the §B.1 dispatcher one virtual call.
- `mean()`, `variance()`, `raw_moment(k)`, `central_moment(k)` for
  `Expectation`.
- `sample(rng)` for `MonteCarloSampler`.
- `plot_range(quantile_lo, quantile_hi)` for `RvAnalyticalCurves`.
- `serialise()` plus a registered `parse()` for the on-disk text
  encoding that `RandomVariable.cpp` currently centralises.

A `DistributionRegistry` keyed on the encoded family name maps to a
factory.  Adding a new family becomes one new file plus one registry
entry; no existing file is touched.  The architecture mirrors the
per-file-per-semiring layout in the companion Lean formalisation
(`Provenance/Semirings/<name>.lean` in
[`provenance-lean`](https://github.com/PierreSenellart/provenance-lean)),
where each concrete provenance semiring lives in its own file and
exposes the `SemiringWithMonus` instance the rest of the library
consumes — the same expression-problem pressure, the same answer.

**What the refactor does *not* absorb.** Single-class virtual dispatch
captures *unary* operations on one distribution.  Pairwise and joint
behaviour does not fit cleanly, and trying to force it through the
`Distribution` interface re-creates the row-major coupling the
refactor was meant to dissolve:

- **Family-closure folds** — Normal + Normal = Normal, Erlang sum
  closure, fixed-`p` Binomial sum closure, Poisson sum closure,
  exponential min closure — are intrinsically pairwise on
  `(DistKind, DistKind)`.  These belong in a separate
  `ClosureRuleRegistry` consulted by the `HybridEvaluator` simplifier,
  keyed by family pair and arithmetic operator.  Per-family files can
  *register* their participation in such rules without owning them.
- **RV-vs-RV comparators (§B.2)** are similarly pairwise:
  a `ComparatorRuleRegistry` mapping `(DistKind, op, DistKind)` to a
  closed-form-probability lambda.
- **Copulas (§D.2)** are inherently multi-distribution; the
  `gate_copula` itself is the natural locus, optionally with
  per-copula-family files in a parallel `src/copulas/` directory if
  symmetry helps.
- **Conditioning (§D.1)** wraps a `Distribution` rather than being
  one; the wrapper delegates to the wrapped family for the easy cases
  and falls back to a generic rejection path otherwise.

Keeping these in dedicated registries — rather than over-loading the
`Distribution` interface with multi-dispatch hacks — preserves the
single-responsibility property that makes the per-distribution files
worth having in the first place.

**Sequencing.**  The refactor produces no user-visible feature on its
own, so its payoff is entirely a function of the features it
unblocks.  Doing it *before* the Quick-wins batch (§A.1 Gamma,
§A.2 Log-normal, §B.1 quantiles) gives:

1. The four existing families migrate as a behaviour-preserving
   change, with the existing `test/` suite as the regression net.
2. Gamma lands as the first proof-of-concept family under the new
   layout — one file in `src/distributions/`, one registry entry, no
   edits anywhere else.  Anything missing from the abstract interface
   surfaces here, while the cost of patching is still small.
3. Every subsequent family inherits the cleaned-up cost profile.

Doing the refactor *after* Gamma would mean migrating five families
instead of four, with the same end state.

**Cost.** A mechanical translation of existing `switch` blocks into
virtual method bodies, plus the closure-rule and comparator
registries lifted out of `HybridEvaluator.cpp` and the relevant
`AnalyticEvaluator.cpp` paths.  No new functionality, no changes to
the gate ABI or the on-disk text encoding.  The hardest design
choices are the shape of the `Support` value and the boundary between
`Distribution` methods and registry-held pairwise rules — both are
internal and revisable.

---

## Cross-cutting UI design principles

The branch's existing grammar is good. Extensions should preserve it.

1. **Same constructor pattern.** All new distributions enter as
   `provsql.<name>(params)` returning `random_variable`. No surface
   change to insert statements, joins, or aggregations.
2. **Comparison and arithmetic stay infix.** `gate_cmp` rewriting and
   `gate_arith` construction happen at plan time. Users never write
   gate constructors directly.
3. **Polymorphic dispatchers grow uniformly.** `expected`, `variance`,
   `moment`, `central_moment`, `support`, and the new `quantile`,
   `entropy`, `kl`, `mutual_information` all take an optional
   `prov uuid DEFAULT gate_one()`. Conditioning is *always* a parameter
   or a `gate_conditioned` wrap, never a separate function.
4. **One escape hatch per direction.** Empirical samples for arbitrary
   ML output; copulas for arbitrary dependence; intervention for
   arbitrary causal structure. Each is a *single* gate type, not a
   proliferation of one-off constructors.
5. **MC fallback stays invisible.** Users should only learn the
   evaluator decomposition exists when they tune GUCs
   (`rv_mc_samples`, `monte_carlo_seed`) or hit a budget-exhaustion
   warning. The default behaviour remains: analytical where possible,
   MC silently when not.
6. **Base independence as architectural backbone, primitives as
   handles.** Base `gate_rv` instances are always independent (the
   continuous analog of the discrete PDB consensus on tuple-existence
   Bernoullis); correlation enters the system through operations that
   share base RVs, exactly as discrete provenance correlation enters
   through shared Boolean variables. Primitives like `gate_copula`,
   `gate_mvnormal`, and the stochastic-process constructors exist as
   ergonomic sugar and as recognised simplifier handles, with
   decomposition rules that rewrite them as operations on independent
   base RVs when that view is cleaner. See §Theoretical backbone.

---

## Suggested execution order

A defensible sequencing that front-loads value and back-loads
architectural risk:

1. **Prerequisite refactor: per-distribution class hierarchy (§F.1).**
   Behaviour-preserving migration of the four existing families into
   `src/distributions/`, gated by the existing test suite.  Lands no
   user-visible feature; cuts the per-family cost of everything below.
2. **Quick wins, in parallel.** Gamma (§A.1) — the proof-of-concept
   for the new layout — together with Log-normal (§A.2), quantiles
   (§B.1), RV-vs-RV comparisons (§B.2), and GMM constructor (§C.1).
   All are small, mostly independent, and ship as one minor release.
3. **Solid mid-term batch.** Beta (§A.3) and the discrete families
   (§A.4) round out the parametric set. Function application (§B.3)
   then unlocks the Normal ↔ Log-normal bridge analytically.
   Order-statistic aggregates (§B.4) and information-theoretic
   primitives (§B.5) close the expressivity gap.
4. **Empirical track.** Samples gate (§C.2) and CDF gate (§C.3), then
   snapshot (§C.4). Each lands as a `Distribution` subclass under the
   §F.1 hierarchy, reusing the moment dispatchers already in place.
5. **First architectural step: conditioning as a gate (§D.1).** The
   most leverage per architectural unit; collapses the existing
   truncation codepath into a general mechanism; unlocks §E.1.
6. **Second architectural step: correlation (§D.2 + §A.5).** The MVN
   constructor lands as a Gaussian-copula special case; copulas land
   simultaneously. Largest expressivity gain in the entire roadmap.
7. **Third architectural step: stochastic processes (§D.3).** Builds
   on §D.2.
8. **Research track in parallel.** Causal interventions (§D.4),
   Shapley over RV-valued payoffs (§E.1), and provenance of sampled
   values (§E.2) can be explored independently of (4)–(7), and would
   each plausibly anchor a paper.
