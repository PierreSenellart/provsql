# ProvSQL Random Variables – Feature Roadmap (open items)

This document lists, categorises, and prioritises the **still-open**
follow-up features to ProvSQL's continuous random-variable surface, with
a focus on **applicability** (does it solve a real user problem?) and
**user interface** (does it fit the existing `provsql.<name>(...)` /
infix-operator grammar?).

Features already shipped are not listed here; the shipped surface is
documented in the user manual
(`doc/source/user/continuous-distributions.rst`) and in `CLAUDE.md`.
Conditioning follow-ups are tracked in
[`conditioning.md`](conditioning.md), latent-variable inference
follow-ups in [`latent-variables.md`](latent-variables.md) and
[`conjugate-posteriors.md`](conjugate-posteriors.md).

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
| 1 | Native analytic discrete families | Parametric distributions | Mid-term |
| 2 | Multivariate Normal | Parametric distributions | Architectural |
| 3 | CDF / arbitrary monotone transforms | Expressivity completion | Mid-term |
| 4 | Frozen-distribution snapshots | Data-driven distributions | Quick win |
| 5 | Correlation / copulas | Structural extensions | Architectural |
| 6 | Stochastic processes | Structural extensions | Architectural |
| 7 | Causal interventions (`do`) | Structural extensions | Research |
| 8 | Provenance of sampled values | Provenance × probability | Research |
| 9 | Sampling under constraints with witness extraction | Provenance × probability | Research |
| 10 | Probabilistic-circuit subsystem (distribution-valued gates) | Structural extensions | Research (low priority) |

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
expressions share a base `gate_rv`. The `FootprintCache` is
the continuous analog of "which Boolean variables does this provenance
formula touch" – it tracks which base RVs each arithmetic subtree
depends on, and the structural-independence shortcut on
`gate_arith TIMES` is the continuous version of the disjoint-support
optimisation in Boolean probability computation.

### Where the analogy strains

In the discrete case, shared Boolean variables suffice to compute joint
probabilities by weighted model counting – the algebra is closed and
clean. In the continuous case, shared base `gate_rv`s give you
correlation, but whether you can *exploit it analytically* depends on
the marginal families and the operations involved:

- For **Gaussian marginals**, the function-of-independents trick is
  elegant and preserves analytical structure all the way through. A
  ρ-correlated bivariate Normal is `X = Z₁`, `Y = ρZ₁ + √(1−ρ²) Z₂`
  with `Z₁, Z₂` independent Normals; the simplifier can recognise this
  as Gaussian-coupled and fold subsequent linear combinations using the
  joint covariance. MVN (§A.2) can in principle be a *derived* feature
  rather than a fundamentally new primitive.
- For **non-Gaussian marginals coupled by a non-Gaussian copula**, the
  representation still exists in principle (Sklar + inverse Rosenblatt),
  but the functions to apply are inverse CDFs of compound special
  functions. The simplifier loses sight of the joint structure
  immediately, the analytical paths in `AnalyticEvaluator` do not fire,
  and evaluation falls back to MC – negating the analytical advantage
  that motivated base independence in the first place.

### Architectural choice

This creates a tension that does not really arise in the discrete case:

- **Purist position.** Keep base RVs independent; add only the
  operations needed (monotone function application, plus copulas as
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
layers – tuple existence (still Boolean provenance, still independent)
*and* per-row RV value draws (independently drawn per row from per-row
parametric distributions). Correlations across rows in the same column
(autoregressive structure, time series; §D.2) and correlations across
columns in the same row (joint Normal returns on the same day) are
both possible and both want first-class expression. The
function-of-independents argument covers both, but the UI for "AAPL
and MSFT are correlated within each trading day" is genuinely awkward
without a primitive – the user has to set up a per-row hidden
auxiliary `gate_rv` that both columns reference, and the simplifier
has to learn to recognise that pattern.

### Adopted position

This roadmap takes the **hybrid stance**. The discrete consensus *does*
carry over in the strict mathematical sense and is worth preserving as
the architectural backbone, so base `gate_rv` instances stay independent
and correlation is in principle introduced through operations that share
them. But `gate_copula` and `gate_mvnormal` (§D.1, §A.2) exist as
recognised sugar – the gate type serves as a *handle* for the
simplifier and as ergonomic surface for the user. This preserves the
discrete consensus philosophically while paying the UI and
simplifier-engineering cost the continuous setting demands.

---

## A. Parametric distributions

### A.1 Native analytic discrete families – **[Mid-term]**

Native self-registering `Distribution` families now exist for Poisson,
Binomial, Geometric, and Negative binomial (`src/distributions/`), but
only for *latent* (RV-parameterised) leaves; the literal-parameter
constructors still enumerate a log-space pmf into a truncated
`categorical` (`categorical_from_log_pmf`), which covers the textbook
workload exactly over the enumerated support. Open:

- **Literal parameters through the native families.** The categorical
  constructors cap the outcome count (e.g. `poisson` raises above
  λ ≈ 170 000 and suggests the Normal approximation); routing literal
  parameters through the native family carries the exact law at any
  parameter.
- **Sum-closure folds at simplifier time.** Poisson sums close,
  fixed-`p` Binomial sums close; as categoricals these are convolutions
  that grow the support instead of folding to one gate. The closure
  rules land in the existing sum-closure registry (no discrete family
  registers one today).
- **Memorylessness for Geometric** (truncated sampling), the discrete
  cousin of the Exponential truncation path.
- **A Hypergeometric family**: blocked on widening the two-parameter
  `Distribution` ABI (see [`latent-variables.md`](latent-variables.md)).

**Applicability.** Counts (arrivals, events per interval), success
counts in fixed trials, waiting-times-in-trials – at the scales where
enumeration stops being an option (high-rate arrival processes,
genome-scale counts).

### A.2 Multivariate Normal – **[Architectural]**

Every operation closes: linear combinations of MVN are MVN, marginals
are MVN, conditional of MVN given MVN is MVN. Unlocks correlation – the
single biggest expressive gap in the current setup.

**Framing.** Per the hybrid position in §Theoretical backbone, MVN is
*derivable* from base independence: the Cholesky decomposition
`Y = L Z` (with `Z` i.i.d. standard Normals and `L Lᵀ = Σ`) expresses
any MVN as a linear combination of independent base RVs, and the
simplifier can recognise that pattern and fold linear combinations
using the joint covariance. The MVN constructor therefore lands as a
recognised sugar that compiles to operations on independent base
`gate_rv`s – a *handle* for the simplifier and ergonomic surface for
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
  under sum is rare) but have undefined moments – breaks the
  `Expectation` contract. Either make moment evaluation
  partial or detect-and-error on those subtrees.
- **Student's t and F** are quotients of RVs; really useful only once
  the analytical evaluator exploits `gate_arith` division as a
  first-class case.

---

## B. Expressivity completion

### B.1 CDF / arbitrary monotone transforms – **[Mid-term]**

The smooth-transform whitelist (`pow` / `^` / `ln` / `exp` / `sqrt`,
with domain guards and the registry-driven family folds such as
`ln(lognormal) → normal`) and the monotone-piecewise subset
(`abs` / clamp / ReLU via `CASE`-over-RV) both ship. Two transform
shapes remain inexpressible:

- **The probability integral transform** `F_X(X) → Uniform(0,1)` – the
  CDF of a distribution applied *as a transform* to an RV. Useful for
  copula constructions (§D.1 compiles through exactly this map) and for
  calibration / PIT diagnostics.
- **General user-specified monotone maps** beyond the whitelist. Since
  `Distribution` already exposes `cdf` / `quantile` per family, a
  `gate_apply(rv, op)` carrying a named registered transform (rather
  than a new opcode per function) keeps `RangeCheck` exact – monotone
  transforms preserve CDF structure – with MC fallback for everything
  else.

**UI.**
```sql
SELECT provsql.cdf_of(x, x) AS pit  -- F_X(X), uniform under the model
FROM observations;
```

---

## C. Empirical / data-driven distributions

### C.1 Frozen-distribution snapshots – **[Quick win]**

`provsql.snapshot(rv, n_samples => 10000)` materialises a complex
`gate_arith` subtree as a frozen empirical-samples distribution. A
deliberate trade of analytical fidelity for query-time performance. The
provenance lineage of *which subtree was frozen and when* is preserved
naturally – a side benefit unique to a provenance-aware system.

Both halves already exist – `rv_sample(token, n)` draws and
`empirical_samples(samples)` loads – so the remaining work is the
one-call sugar that composes them server-side plus the lineage record
of the frozen source token.

**Applicability.** Hot paths where the same expensive composite RV is
queried repeatedly; ad-hoc exploration; checkpointing.

**UI.**
```sql
UPDATE forecasts
SET demand = provsql.snapshot(demand, n_samples => 10000)
WHERE expensive_to_evaluate;
```

---

## D. Structural extensions

Larger architectural moves that open new classes of query.

### D.1 Correlation / copulas – **[Architectural]**

The biggest expressive hole *practically*, though not theoretically –
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

MVN (§A.2) is the Gaussian-copula special case where marginals are
also Normal, and its Cholesky decomposition is the same kind of rule.
The inverse-Rosenblatt compilation route runs through the
CDF-as-transform map (§B.1).

**Interaction.** Coupled RVs are explicitly dependent – the
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

-- Or: full multivariate construction (the MVN route from §A.2)
```

### D.2 Stochastic processes – **[Architectural]**

Nothing in the current model speaks to AR(1), random walks, Brownian
motion, Markov chains. Hand-building via recursive CTE + conditioning
gates is possible but awkward and gives up analytical handling.

**Mechanism.** First-class constructors returning
`SETOF random_variable` indexed by step. Internally they compile to
correlated `gate_rv` chains using the §D.1 copula machinery.

**Applicability.** Time-series forecasting, path-dependent financial
options, queueing models, epidemiological compartment models – the
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

### D.3 Causal interventions (`do`-calculus) – **[Research]**

ProvSQL is unusually well-positioned here: the provenance circuit *is* a
DAG, gates are explicit, and severing incoming edges is mechanically
simple. A `provsql.intervene(rv, value)` gate replaces a sub-circuit
with a fixed value while leaving downstream consumers in place – giving
you Pearl's `do(X := x)`.

**Combined with the conditioning gate**, that's observational vs
interventional probability – the core counterfactual machinery. No
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

### D.4 Probabilistic-circuit subsystem (distribution-valued gates) – **[Research, low priority]**

The gate DAG plus the `gate_mixture` node make ProvSQL look one step away
from hosting **probabilistic circuits** (PCs: arithmetic circuits / SPNs /
PSDDs, in the Darwiche / Vergari–Choi–Peharz–Van den Broeck sense) – a
mixture is a PC sum node, so surely we just add a product node and we have
PCs. The reality is sharper, and worth recording so the analogy is not
misused.

**ProvSQL's native circuit is not a PC, and need not become one.** A PC
*is* a distribution `p(x)`: its weights and leaf parameters are the model,
and a feed-forward pass answers density-at-a-point (EVI), marginals (MAR),
and MAP in time linear in the circuit, *because* the circuit is built to be
smooth, decomposable, and (for MAP) deterministic. ProvSQL's circuit is
**provenance**: a semiring-generic symbolic object carrying no
probabilities, where probability is one *downstream* interpretation that
first needs **knowledge compilation** to a d-DNNF (tree-decomposition / d4).
The two families meet only at ProvSQL's *output of compilation*: the
compiled d-DNNF already **is** a deterministic, decomposable arithmetic
circuit, and `dDNNF::probabilityEvaluation` already **is** the PC
sum-product pass. On the discrete side, therefore, the "PC product" already
exists – it is `gate_times` over disjoint variable scopes – and there is
nothing to add.

**Where a product node is genuinely new is the continuous value layer, and
there it is a *type* change, not a gate.** The value layer's invariant is
that every token evaluates to **one scalar random variable**. `gate_mixture`
fits because a mixture of two scalars is still a scalar (`rec_expectation`
evaluates it as `π·E[x] + (1−π)·E[y]`). `gate_arith TIMES` also exists, but
it is **scalar multiplication** `X·Y` (a new RV, density via Mellin
convolution), *not* a PC product. A PC product `p(X)·p(Y)` is a
**factorization over disjoint scopes**: its value is a *joint distribution
over a vector*, not a number – which the scalar-per-token typing cannot
express. This is the same wall MVN (§A.2) and copulas (§D.1) hit:
representing a *joint* rather than a draw.

**And a gate is not a PC without the matching evaluation mode.** What makes
a PC a PC is the query it answers – point density, marginals, MAP, via
sum-product over its structural invariants. ProvSQL's RV layer answers a
*different* set: moments and `P(event)` via `Expectation` /
`HybridEvaluator` / Monte Carlo. It never evaluates a joint density at an
assignment or marginalizes by circuit recursion. A product gate without
such an evaluator only gives the sampler independent children to draw – not
PC inference.

So hosting PCs is a coherent **subsystem**, not a one-gate add. It needs:
1. **scope-typed, distribution-valued gates** (a gate as `c: assignment →
   ℝ≥0` over a scope), alongside the existing scalar-RV typing – the same
   architectural move §A.2 / §D.1 require;
2. **normalized leaves** evaluable both at a point and by integration –
   `gate_rv` is most of the way there, and the shipped GMM constructor and
   empirical-distribution loaders are exactly these leaves;
3. a **third evaluation mode** – point/marginal evaluation, distinct from
   both `evaluate<S>` (semiring, no assignment) and `Expectation` (moments).

Then *sum (have it) + disjoint-scope product + normalized leaves* = a PC,
and a GMM is the smallest instance.

**Two caveats that should gate the work.** First, this is a **parallel
inference engine** sharing the gate store, not a unification with
provenance: the Boolean-WMC path and the PC path stay separate computations.
Second, decide it by *which query it newly answers*: if the goal is
marginals / MAP / density of a **learned joint** (GMM, copula, empirical),
PCs are the right tool and the path above is clean; if it is still moments
of transformed RVs, the existing algebra already covers it and a product
node buys nothing. Hence: low priority, and most naturally a by-product of
§§A.2/D.1 rather than a goal in itself.

---

## E. Provenance × probability – ProvSQL-specific directions

Capabilities that exploit the provenance circuit specifically. Almost
no other system can offer them.

### E.1 Provenance of sampled values – **[Research]**

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

### E.2 Sampling under constraints with witness extraction – **[Research]**

A close relative of E.1. Rejection sampling already happens for
conditioning. The bool gate that *accepted* a sample is itself a
provenance object – returning it alongside the sample lets users inspect
the rejection witness, which is sometimes more informative than the
sample itself.

---

## Cross-cutting UI design principles

The established grammar is good. Extensions should preserve it.

1. **Same constructor pattern.** All new distributions enter as
   `provsql.<name>(params)` returning `random_variable`. No surface
   change to insert statements, joins, or aggregations.
2. **Comparison and arithmetic stay infix.** `gate_cmp` rewriting and
   `gate_arith` construction happen at plan time. Users never write
   gate constructors directly.
3. **Polymorphic dispatchers grow uniformly.** `expected`, `variance`,
   `moment`, `central_moment`, `support`, `quantile`, `entropy` all
   take an optional `prov uuid DEFAULT gate_one()`; any new readout
   must too. Conditioning is *always* a parameter or a
   `gate_conditioned` wrap, never a separate function.
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

1. **Quick win.** Frozen-distribution snapshots (§C.1) – one-call sugar
   over the shipped `rv_sample` + `empirical_samples`.
2. **Mid-term batch.** Native analytic discrete families (§A.1) and the
   CDF / monotone-transform surface (§B.1). The latter is also the
   compilation substrate for copulas.
3. **First architectural step: correlation (§D.1 + §A.2).** The MVN
   constructor lands as a Gaussian-copula special case; copulas land
   simultaneously. Largest expressivity gain in the entire roadmap.
4. **Second architectural step: stochastic processes (§D.2).** Builds
   on §D.1.
5. **Research track in parallel.** Causal interventions (§D.3) and
   provenance of sampled values (§E.1, with §E.2 alongside) can be
   explored independently of (2)–(4), and would each plausibly anchor a
   paper. The probabilistic-circuit subsystem (§D.4) is lowest priority
   and most naturally falls out of §§A.2/D.1 rather than being pursued
   on its own.
