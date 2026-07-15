# Multivalued (vector-valued) distributions

Plan for extending the continuous random-variable surface from scalar
to **vector-valued** random variables (values in R^d): joint
constructors (multivariate Normal, Dirichlet, empirical point clouds),
componentwise algebra, and distance/dot-product readouts that plug into
the existing scalar event machinery. pgvector was evaluated as a
carrier and as an interop target and is **not used**: the surface is
plain `float8[]`, and the one thing pgvector uniquely offers (ANN
indexing at scale) is recorded as a demand-gated follow-up (§2).
"Multivalued" here means vector-valued RVs; it is unrelated to the
discrete `gate_mulinput` multivalued-constant gates. Anchored on:

- [`continuous_distributions.md`](continuous_distributions.md) §A.2
  (multivariate Normal), §D.1 (copulas), §D.4 (distribution-valued
  gates): all three defer to a "vector typing" architectural move; this
  document is that move.
- [`feature-gap-analysis.md`](feature-gap-analysis.md) candidate #3
  (correlated-attribute uncertainty, Orion 2.0 / MCDB), tiered P2.
- The pgvector extension (<https://github.com/pgvector/pgvector>):
  types `vector` (float4, ≤16000 dims, indexable ≤2000), `halfvec`,
  `sparsevec`, `bit`; distance operators `<->` (L2), `<#>` (neg. inner
  product), `<=>` (cosine), `<+>` (L1); elementwise `+ - *`, `sum`/`avg`
  aggregates, `subvector`, `l2_normalize`. Casts: `integer[]`/`real[]`/
  `double precision[]`/`numeric[]` → `vector` (ASSIGNMENT),
  `vector` → `real[]` (IMPLICIT). PostgreSQL-licensed C.
- PostgreSQL extension packaging
  (<https://www.postgresql.org/docs/current/extend-extensions.html>):
  `requires` in a control file is a *hard* dependency; the standard
  pattern for optional cross-extension features is a separate bridge
  extension (cf. `postgis_tiger_geocoder`, `vectorscale`).

## Out of scope

- **Copula families beyond the Gaussian one** – the `gate_copula`
  sugar layer stays in `continuous_distributions.md` §D.1; the MVN
  landing here is its Gaussian special case and provides the
  decomposition machinery §D.1 will reuse.
- **Probabilistic-circuit inference modes** (joint density at a point,
  MAP) – `continuous_distributions.md` §D.4. The `gate_vec` typing
  below is deliberately the "scope-typed gate" §D.4 asks for, but the
  third evaluation mode is not part of this plan.
- **ANN index support on uncertain vectors** (indexing expected
  vectors is covered via the bridge; indexing *distributions* is
  research).
- **Matrix-valued RVs** (Wishart etc.) – nothing below precludes them,
  but no surface is planned.

## Plan

### 1. Datatype: `float8[]` as the canonical carrier

Three candidate carriers for vector values at the SQL boundary were
considered:

1. **pgvector `vector`** – rejected. Its elements are float4 (lossy
   for distribution parameters such as covariance entries), it would
   make every ProvSQL build – including the WASM/Playground build and
   every packaged target (PGXN, Docker, distro) – depend on an
   external extension, and the circuit-internal representation is C++
   `double`s regardless, so nothing is gained from it.
2. **`double precision[]`** – **adopted**. Native, lossless, already
   the parameter surface proposed for `provsql.mvnormal` in §A.2,
   works everywhere including PGlite.
3. A bespoke value type – no benefit over `float8[]`.

Choosing `float8[]` does not wall off users whose data already sits in
pgvector columns: pgvector ships an IMPLICIT `vector → real[]` cast,
so `as_random_vector(v::real[]::float8[])` lifts an existing embedding
column with zero pgvector awareness on ProvSQL's side.

**The tracked type is a new `random_vector`**, not a reuse of
`random_variable`. Like `random_variable` it is a 16-byte UUID wrapper
(`internallength = 16`, binary-coercible with `uuid`, all semantics in
the referenced gate). A separate type is forced, not stylistic:

- PostgreSQL return types are static: `expected(random_variable)`
  returns `float8`; the vector reading must return `float8[]`.
  Overloading on a distinct argument type is the only clean dispatch.
- The scalar comparison operators `< <= > >=` assume a total order and
  must **not** exist on vectors; with a shared type they would
  type-check and silently compute nonsense.
- The planner routes on type OID (`aggtype ==
  OID_TYPE_RANDOM_VARIABLE`, comparison funcoid matching); a second
  OID is how the vector arms are told apart.

Dimension is **gate metadata, not a typmod**: `random_vector(3)`-style
typmods complicate every cast and function signature for little gain;
`dims(x)` reads the gate, and dimension mismatches raise at
construction/evaluation time. Componentwise access `component(x, i)`
(and a subscript sugar later) returns a scalar `random_variable` – the
bridge into the *entire* existing scalar machinery.

### 2. pgvector: assessed, skipped, one demand-gated follow-up

An itemised assessment of what pgvector would actually contribute:

- **Core representation** – nothing (§1).
- **Reading existing embedding columns** – nothing that plain casts do
  not already provide (§1: `vector → real[]` is IMPLICIT on pgvector's
  side).
- **Operator spelling** (`x <-> q` vs `l2_distance(x, q)`) – cosmetic.
- **ANN indexing (HNSW / IVFFlat)** – the single genuine capability:
  `float8[]` cannot be ANN-indexed, so similarity queries over the
  vector-RV surface are sequential scans. That is fine at case-study /
  Playground scale (10³–10⁵ rows) but rules out the
  "shortlist-by-index, re-rank with ProvSQL" pattern on large
  embedding corpora.

Conclusion: no pgvector anywhere in this plan. If a concrete
large-scale similarity workload ever materialises, the recorded shape
of the fix is a **SQL-only bridge extension** (`provsql_vector`,
control file `requires = 'provsql, vector'`, no new .so, second
EXTENSION in the same Makefile): `expected_vector(random_vector)
RETURNS vector` so an HNSW index over expected vectors can generate
candidates, plus one-line distance-operator delegations. Nothing in
§§3–6 needs to anticipate it; it layers on afterwards, and `CREATE
EXTENSION provsql_vector` failing cleanly when pgvector is absent is
the entire optionality story. Should the C core ever need to read
`vector` values directly, the optional-OID self-disable pattern used
for `random_variable` itself (`src/provsql_utils.c`,
`initialize_constants`: no `CheckOid`, `InvalidOid` disables the arm)
plus `OidFunctionCall` covers it without linking.

### 3. Circuit structure

**Primary route: compile to scalar gates.** Consistent with §A.2's
"recognised sugar" position, a vector RV is represented as a tuple of
scalar-valued gates under one new packing gate:

- **`vec` gate** (appended to the `provenance_gate` enum – on-disk ABI,
  append-only): children are the d scalar component wires, in order.
  No `extra`, no new `GateInformation` field. A `random_vector` token
  points at a `vec` gate exactly as a `random_variable` points at a
  scalar-valued gate.
- **MVN** compiles by Cholesky: d independent standard-Normal
  `gate_rv` leaves Z, `gate_arith` affine combinations μ + L·Z, packed
  under `vec`. Correlation is carried by *shared leaves*, which the MC
  sampler already couples correctly (one draw per gate per iteration)
  and which `FootprintCache` already treats as dependence.
- **Dirichlet** compiles to normalised independent Gammas
  (Gamma_i / Σ_j Gamma_j): `gate_arith` DIV over a shared-sum
  subcircuit.
- **Componentwise algebra** (`+`, `-`, scalar `*`) is d per-component
  `gate_arith` gates repacked under a new `vec`.
- **Dot product / L2 distance / norms** compile to scalar `gate_arith`
  trees over the components (dot = PLUS of TIMES; L2 = POW 0.5 of a
  sum of squares). The result is an ordinary scalar
  `random_variable`, so `l2_distance(x, y) < 0.5` engages the existing
  comparison lift, `gate_cmp`, conditioning, HAVING and probability
  methods with **zero** new planner or evaluator machinery. No new
  `provsql_arith_op` opcode is needed for v1 (a fused distance opcode
  is a later efficiency option; the enum is append-only).

This route reuses nearly the entire scalar stack: analytically, an
affine functional a·X + b of a compiled MVN folds through the existing
Normal sum-closure registry to an *exact* scalar Normal in the
peephole, so §A.2's headline queries (expected value / variance of a
portfolio combination) come out closed-form for free.

**Later route: native vector leaves (`rv_vec` gate).** For families
with no useful scalar decomposition – above all **empirical point
clouds** (bags of embeddings: a categorical over points in R^d, where
per-component encoding would explode into selector/case chains),
directional families (von Mises–Fisher), and **multinomial count
vectors** (negatively correlated components with no independent-leaf
decomposition; the MC sampler draws them natively, and their Gaussian
limit N(nq, n(diag(q) − qqᵀ)) is already covered by the compiled MVN,
which is the right surrogate for large n anyway) – a second appended
gate type
`rv_vec` with `extra = "family:d:params…"` and a parallel
`VectorDistribution` interface (see §5). Deliberately phased after the
compile-to-scalar route ships.

**Size limits to resolve.** The mmap store is fine (variable-length
`extra`, `unsigned` length; children unbounded), but the multi-process
IPC path caps each message at one `PIPE_BUF` (typically 4096 bytes)
atomic write: a `vec` create message carries all children (~250 UUIDs
max), and an `rv_vec` extra carrying a Cholesky factor is O(d²) text.
Either add a chunked create/set_extra protocol (an "append children" /
"append extra" opcode pair) or enforce and document a dimension cap in
v1 (d ≤ 100 covers the geo/sensor/portfolio cases; embeddings need the
chunked protocol or the in-process store, which already grows its
buffer).

### 4. Planner

Follows the `random_variable` precedent exactly, and self-disables on
schemas that lack the type:

- `OID_TYPE_RANDOM_VECTOR` (+ array type) added to
  `initialize_constants` as an *optional* OID (no `CheckOid`).
- **Aggregates** `sum` / `avg` (= centroid) over `random_vector`
  route on `aggtype` like the RV aggregates and lower to the same
  semimodule construction componentwise (`rv_aggregate_semimod`
  pattern under each component, repacked). Vector aggregation *is* a
  semimodule over the provenance semiring, so aggregation provenance
  (`gate_agg`/`gate_semimod`) extends canonically – this is the
  m-semiring story, not a probability feature.
- **No comparison operators** on `random_vector` (no total order); the
  event surface is distance predicates, which are scalar-RV
  comparisons and reuse the existing lift (with its known
  position-specific limits: WHERE/HAVING and top-of-target-entry).
- Projection/relabelling of `random_vector` columns through the
  rewriter mirrors the `wrap_random_variable_uuid` handling.

### 5. Evaluation

- **Phase-1 (compile-to-scalar) needs no sampler change**: a `vec`
  gate is never evaluated as a value; readouts iterate its component
  wires through the existing `evalScalar` / `compute_expectation`.
  Joint sampling of a whole vector (`rv_sample(x, n) RETURNS SETOF
  float8[]`) is one MC pass with the per-iteration caches shared
  across components – exactly what `getJointCircuit`'s multi-rooted
  BFS plus the sampler's `rv_cache_` already provide for covariance.
- **Readouts** (all core, all `float8[]`-surfaced):
  `expected(random_vector) → float8[]`, `variance → float8[]`
  (componentwise), `covariance_matrix(x) → float8[]` (d×d array; one
  coupled pass, analytic where the pairwise machinery applies),
  `support → (lo float8[], hi float8[])` (box), marginal
  `quantile(x, p) → float8[]` (documented as per-component),
  `rv_histogram` gains a 2-D mode for d = 2.
- **Phase-3 native leaves** add to `MonteCarloSampler` a
  `vector_cache_ : unordered_map<gate_t, std::vector<double>>` and an
  `evalVector` entry, plus a `VectorDistribution` interface *parallel*
  to `Distribution` (the scalar interface is structurally 1-D:
  two-double factory, `nparams ≤ 2`, scalar `pdf/cdf/sample`, interval
  `DistSupport`, `affine(double, double)`; generalising it in place
  would churn every family for no shared code). `component(x, i)` on a
  native leaf becomes a marginal-projection gate evaluated through the
  vector cache.
- **Analytic extensions**, ordered by value: MVN differential entropy
  (closed form) in `InformationTheory`; a **Gaussian orthant-CDF arm**
  (see below); Mahalanobis / squared-norm events on Gaussians
  (generalised chi-square CDF) in `AnalyticEvaluator`; everything else
  falls back to MC as usual.
- **Gaussian orthant CDF for correlated comparison conjunctions.** An
  AND-conjunction of comparison events whose operands are all affine
  combinations of shared Normal leaves (exactly what a compiled MVN
  produces) is a multivariate-normal orthant probability
  Φ_d(τ; μ, Σ). Today such a conjunction falls through to MC, which
  (a) converges at the O(N^-1/2) sampling rate when a
  quasi-deterministic quadrature (Genz–Bretz) evaluates the same
  quantity to near machine precision at O(d³)-ish cost, and (b) simply
  returns 0 on rare orthants (Pr ≲ 10⁻⁶ is already unresolvable at
  realistic sample budgets, whereas applications legitimately need
  10⁻¹² – rare-event workloads pair the orthant CDF with a tilted
  importance-sampling fallback). Shape of the fix: after the peephole
  has folded the affine arithmetic through the Normal sum-closure
  registry, detect in `AnalyticEvaluator` that every conjunct of a
  `walkAndConjunctIntervals`-style joint pass is a linear comparison
  over jointly-Gaussian scalars, assemble (μ, Σ) from the shared-leaf
  decomposition, and call a Genz–Bretz routine; decline to MC on any
  non-Gaussian leaf. This subsumes the pairwise Normal-vs-Normal
  closed form (d = 1) and gives "min/max/rank of correlated Gaussians"
  events – first-elimination / argmin probabilities, order statistics
  of correlated portfolio components – an exact path. Depends on the
  same P0 census fix; a correlated conjunction of shared-leaf
  comparisons is precisely the shape the sibling-arm bug corrupts.
- **Prerequisite**: the HybridEvaluator sibling-arm census bug (shared
  leaves across sibling arms de-duplicated as if independent) must be
  fixed first – compiled MVNs are *made of* shared leaves, and the
  current census would silently drop the very correlations this
  feature exists to model.

### 6. SQL surface

Mirrors the `random_variable` DDL discipline (shell type, C in/out on
16 bytes, `uuid` casts with the ASSIGNMENT-not-IMPLICIT rationale
preserved):

- Constructors: `provsql.mvnormal(mu float8[], sigma float8[])`
  returning one `random_vector` (the §A.2 table-returning named-column
  form becomes a thin wrapper that returns the components of this
  gate); `provsql.dirichlet(alpha float8[])`;
  `provsql.vec(VARIADIC random_variable[])` (pack existing scalar RVs,
  including correlated ones); `as_random_vector(float8[])` (Dirac
  lift, IMPLICIT cast); phase 3: `empirical_vectors(float8[][])`.
- Accessors: `component(x, i) → random_variable`, `dims(x) → int`.
- Algebra: `+ - ` and scalar `*` operators; `dot(x, y)`,
  `l2_distance(x, y)`, `l2_norm(x)`, `cosine_distance(x, y)` all
  returning `random_variable` (names follow the established
  vector-search vocabulary, which also keeps the §2 fallback a
  one-line delegation if it is ever built).
- Aggregates: `sum(random_vector)`, `avg(random_vector)`.
- Registry surface: `rv_families()` caps at two scalar parameters and
  is consumed by Studio; vector families get a separate additive
  `vector_families()` (or additive columns) rather than a breaking
  change.

### 7. Studio and Playground

- **Circuit mode**: a glyph + label for `vec` gates (component count
  in the label, like the existing `ξ` RV leaves get family labels from
  `rv_families()`); the pinned-node inspector lists components as
  clickable links into their marginal subcircuits. The
  `node["density"]` PDF plot is 1-D today: for `random_vector` nodes
  render per-component small multiples, and for d = 2 a joint
  scatter/density from `rv_sample` – correlation is the whole point,
  so the 2-D view is the didactic payoff.
- **Eval strip**: readouts returning `float8[]` (expected, marginal
  quantiles, support boxes) rendered as vectors, covariance as a
  matrix (KaTeX already available). Entries appear only when the
  server has the surface: probe `pg_type` for `random_vector`,
  matching Studio's existing degrade-when-absent pattern and its
  compatibility-floor table.
- **Schema panel / results**: a type pill for `random_vector` columns;
  result cells show the token chip (as `random_variable` does) plus
  `d`.
- **Playground**: the full surface runs unchanged (everything is
  `float8[]`, so PGlite needs nothing extra). A sensor-fusion or
  embedding-search **case study** (see §8) is the natural companion,
  pre-seeded like cs1–cs7; notebook deep links make it a good demo of
  correlated uncertainty.
- **Version gating**: features keyed on the extension version that
  ships `random_vector`, added to the studio.rst compatibility table.

### 8. Use cases beyond probability inference

The point of doing this in ProvSQL rather than in a statistics library
is the combination with the *rest* of the provenance machinery:

1. **Provenance-aware similarity search / RAG auditing.** Embeddings
   (even deterministic Dirac vectors) in tracked tables: a kNN or
   similarity-join result carries why/how-provenance of which chunks
   and documents produced it; Shapley/Banzhaf over source documents
   quantifies which sources drove a retrieved answer; the counting
   semiring measures duplicated-chunk support. Uncertainty (encoder
   ensembles, quantisation error) upgrades this to
   `P(dist(x, q) < r)` retrieval – but the provenance value is there
   at probability zero. Scale caveat: without an ANN index this scans
   (§2); the intended scope is corpus sizes where a scan is fine, and
   a large-corpus demand is exactly what would trigger the §2
   follow-up.
2. **Sensor fusion / uncertain geospatial data.** Positions as 2-D/3-D
   Gaussians (GPS error ellipses), fused readings as
   provenance-tracked aggregates; combined with the temporal
   semirings (`sr_temporal`, interval-union) for "when was the vehicle
   plausibly inside zone Z", and with update provenance for
   recalibration audit trails.
3. **Data valuation on vector aggregates.** Group centroids and
   feature-vector sums are `agg_token`s with semimodule provenance;
   Shapley on "how much does row t move the centroid / the distance
   between class centroids" is data debugging, not inference; expected
   Shapley composes with per-row uncertainty.
4. **Entity resolution.** Match decisions `dist(e1, e2) < τ` are
   Boolean events; clusters are provenance formulas over them –
   knowledge compilation gives merge probabilities, why-provenance
   explains a merge, `repair_key` models conflicting embedding
   sources.
5. **Compositional / topic data.** Dirichlet topic vectors per
   document; dominant-topic events via pairwise component comparisons;
   formula/counting semirings give the lineage and multiplicity of
   topic-share aggregates.
6. **Election forecasting with partially observed ballots** (e.g.
   probabilistic ranked-choice voting: unobserved ballots as
   multinomial draws from a polling prior, "candidate a currently
   last" as the conjunction of pairwise tally comparisons). One
   round's elimination probabilities map cleanly onto this plan: the
   Gaussian surrogate of the multinomial tally vector is a compiled
   MVN, the pairwise differences are componentwise `gate_arith`, and
   the argmin event is exactly the correlated-Gaussian-conjunction
   shape the §5 orthant-CDF arm evaluates analytically – without that
   arm, MC reduces the whole exercise to naive sampling and cannot
   resolve the rare orthants such workloads care about. **Scope
   limit**, recorded so it is not rediscovered: the *multi-round*
   recursion (chaining stage probabilities over the 2^k survivor-set
   DAG, per-round re-randomisation, uniform tie-breaking among argmin
   candidates) is an iterative algorithm, not a query; it stays in
   application code that calls one-stage ProvSQL evaluations, and is
   not a target for the rewriter (recursive-CTE positions are anyway
   outside the comparison lift's supported positions). A small-k
   single-stage version makes a good correlated-uncertainty case
   study once the orthant arm exists.

## Priorities

1. **P0 – prerequisites**: HybridEvaluator sibling-arm census fix;
   decide chunked-IPC vs documented dimension cap (§3).
2. **P1 – core vector surface**: `random_vector` type + optional-OID
   plumbing, `vec` gate,
   `mvnormal` / `dirichlet` / `vec` / `as_random_vector` constructors,
   `component`/`dims`, componentwise algebra, dot/distances,
   `float8[]` readouts + `covariance_matrix`, joint `rv_sample`,
   `sum`/`avg` aggregates, pg_regress coverage.
3. **P2 – UX**: Studio (§7) rendering + eval strip + capability
   probes; user-manual chapter; case study (Playground-seeded).
4. **P3 – native vector leaves and analytic depth**: `rv_vec` gate +
   `VectorDistribution` + sampler vector cache, `empirical_vectors`
   for embedding bags, multinomial family, chunked `set_extra`, MVN
   entropy + Gaussian orthant-CDF (Genz–Bretz) + Mahalanobis analytic
   arms, `vector_families()`. The orthant arm is independent of the
   native-leaf machinery (it works on phase-1 compiled MVNs) and can
   be pulled forward on its own if a rare-event or argmin workload
   (§8.6) materialises first.
5. **Demand-gated**: the `provsql_vector` ANN-indexing bridge (§2) –
   only if a large-corpus similarity workload shows up.
6. **Research follow-ups**: copula sugar over this machinery
   (`continuous_distributions.md` §D.1), PC evaluation modes (§D.4).

## Implementation observations

- **Why the scalar `Distribution` interface is not generalised in
  place**: it is 1-D by construction – `DistributionFactory` takes
  exactly `(double p1, double p2)`, `DistributionFamily.nparams ∈
  {1,2}`, `DistSupport` is a scalar interval, and
  `quantile`/`truncatedRawMoment`/`iidOrderStatMean`/`affine` have no
  scalar-free meaning. Every registry (comparator, sum/product
  closure, transform, conjugate) traffics in scalar coefficients. A
  parallel `VectorDistribution` in phase 3 is cheaper than churning
  nine families.
- **The scalar carrier is end-to-end**: the MC sampler memoises
  `unordered_map<gate_t, double>`; `Expectation`/`RangeCheck`/
  `AnalyticEvaluator`/`PivotIntegration`/`InformationTheory` are
  1-D throughout; every SQL readout returns `float8`. This is why the
  compile-to-scalar route (which touches none of it) ships first and
  the native-leaf route is a separate phase.
- **`random_variable` optional-OID precedent**
  (`src/provsql_utils.c`, comment above the `random_variable` lookup):
  missing type ⇒ `InvalidOid` ⇒ every planner comparison self-disables.
  `random_vector` copies this, so pre-upgrade schemas and the 1.0.0
  upgrade canary stay green.
- **pgvector cast matrix** (from `sql/vector.sql`): arrays→`vector`
  are ASSIGNMENT (four element types), `vector`→`real[]` is IMPLICIT;
  `vector` elements are float4 – fine for values, wrong for
  parameters, which is the core argument for `float8[]` at the core
  boundary.
- **Rejected: pgvector as the core carrier** (float4 precision, hard
  dependency on all build targets including WASM, no gain in C++).
- **Rejected: reusing `random_variable` for vectors** (static return
  types, ill-defined total order would type-check, OID routing needs
  the distinction).
- **Rejected: new fused arith opcodes in v1** (dot/L2 compile to
  existing PLUS/TIMES/POW trees; the enum is append-only so a fused
  opcode can come later purely as an optimisation).
