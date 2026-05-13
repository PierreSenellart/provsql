# Operation plan: continuous probability distributions

## Context

The branch `continuous_distributions` was opened to bring back continuous-distribution support that was prototyped in Timothy Leong's 2022 NUS BSc thesis but never integrated. The design document at `doc/TODO/continuous_distributions.md` is the authoritative source for *what* to build and *why*; this operation plan is the executable form — sequencing, verification gates, commit points, and corrections to a few line-number references in the TODO.

The TODO already commits to: two new gate types (`gate_rv`, `gate_arith`) appended to the on-disk gate-type ABI; reuse of pre-existing `gate_value`, `gate_cmp`, `gate_input`; a `random_variable` SQL composite type mirroring `agg_token`; planner-hook rewriting of inequalities against RV columns; a continuous Monte Carlo extension to `BooleanCircuit::monteCarlo`; an `Expectation` compiled semiring with structural independence detection; and Studio renderer/inspector enhancements that stay inside the existing Circuit mode (no new top-level mode).

Out of scope per the TODO and confirmed: EXCEPT, DISTINCT, where-provenance × RV design work, a "Continuous mode" in Studio, and an in-Studio distribution editor. Each remains a clearly-marked follow-up. Aggregation over RVs was originally out of scope and is now partially landed (`sum`, `avg`, `product`) — see Priority 10 below.

Per user direction (2026-05-09): all nine priorities land on the `continuous_distributions` branch. No PRs needed; commit at the end of each priority once `make installcheck` is green. Priorities 5 (peephole pruning) and 6 (expected value & moments) are firmly in scope; Priority 7 (hybrid evaluation) was added the same day after a design discussion on integrating continuous evaluation with the existing structural methods (treedec, d-DNNF, independent).

## Verified file references

The TODO cites several files with line numbers. Exploration on the current tip of `continuous_distributions` shows the following corrections to use during execution; the design itself is unaffected:

- `src/provsql_utils.h:55-73` (gate_type enum) — correct.
- `sql/provsql.common.sql:27-44` (provenance_gate enum) — correct.
- `src/having_semantics.cpp:115` (`extract_constant_C`) — function actually starts at **line 100**; line 115 is inside the body where `parse_int_strict(c.getExtra(w[1]), ok)` lives.
- `src/provsql.c:407-515` (`get_provenance_attributes`) — function actually ends at **line 531**, not 515.
- `src/provsql.c:1101-1392` (`make_provenance_expression`) — correct.
- `src/BooleanCircuit.cpp:287-307` (`monteCarlo`) — correct; uses `rand() *1. / RAND_MAX < getProb(in)` for leaf sampling.
- `sql/provsql.common.sql:215-221` (`add_provenance`) — correct.
- `sql/provsql.common.sql:586-611` (`provenance_cmp`) — correct.
- `src/MMappedCircuit.cpp:129-139` — only `setProb` lazy-creates an input gate; `setInfos` (146-154) and `setExtra` (160-169) **exit if the UUID is unknown**. The TODO overstates this; `set_distribution` should follow `setProb`'s pattern, not `setInfos`/`setExtra`'s.
- `src/MMappedCircuit.h:59-79` (`GateInformation`) — correct, modulo the detail that `extra` is split into `extra_idx` + `extra_len`.
- `sql/fixtures/provsql--1.0.0-common.sql:39` and `sql/upgrades/provsql--1.1.0--1.2.0.sql:48` — both correct.

Additional anchors found during exploration that the TODO doesn't pin:

- **`agg_token` definition**: `sql/provsql.common.sql:1112-1149` (forward decl, IO functions, full type) and `src/agg_token.c:27-120` (C IO). Mirror this for `random_variable`.
- **GUC registration**: `src/provsql.c:3557-3626`. `DefineCustomBoolVariable` / `DefineCustomIntVariable` / `DefineCustomStringVariable` patterns all live here.
- **Operator declarations** for `agg_token × numeric` comparisons: `sql/provsql.common.sql:1259-1358` (12 declarations, two per operator). Mirror this shape for `random_variable`.
- **Compiled-semiring dispatch table**: `src/provenance_evaluate_compiled.cpp:290-335` (string → semiring class via templated `pec(...)`). Add an `expectation` entry under the FLOAT block.
- **Studio gate rendering** is class-based (`node--<type>` from `studio/provsql_studio/static/circuit.js:361`, with CSS in `studio/provsql_studio/static/app.css:1772-1783`); server-side serialiser is `studio/provsql_studio/circuit.py:165-239`. The new `gate_rv` / `gate_arith` types need both client and server entries.
- **Test schedule**: append new `test: continuous_*` lines to `test/schedule.common`; never edit `test/schedule` (generated).

## Execution sequence

Each priority ends with a green `make installcheck`, a one-liner commit, and (where useful) a small demo SQL script committed alongside the tests. The user runs `sudo make install && sudo service postgresql restart` between code changes (per CLAUDE memory).

### Priority 1 — Foundations: type, constructors, value parsing

- Append `gate_rv`, `gate_arith` to `gate_type` in `src/provsql_utils.h:55-73` (before `gate_invalid`); update `gate_type_name[]`. Declare `provsql_arith_op` enum (PLUS=0, TIMES=1, MINUS=2, DIV=3, NEG=4) with the same "do not renumber" warning.
- Mirror the gate names in `sql/provsql.common.sql:27-44` (`provenance_gate` enum).
- New file `src/random_variable_type.c`: PG IO functions `random_variable_in` / `_out` / `_cast` patterned on `src/agg_token.c:27-120`.
- New file `src/RandomVariable.{h,cpp}`: helpers for parsing the `extra` blob (`"normal:μ,σ"` / `"uniform:a,b"` / `"exponential:λ"`), for analytical moments per distribution, and for sampling (used in priority 2).
- In `sql/provsql.common.sql` after the `agg_token` block (~line 1149): forward decl `random_variable`, IO functions, full type definition. SQL constructors `provsql.normal(numeric, numeric)`, `provsql.uniform(numeric, numeric)`, `provsql.exponential(numeric)`, `provsql.as_random(numeric)` — first three create `gate_rv`; `as_random` creates `gate_value`. All return `random_variable`.
- Extend `src/having_semantics.cpp:100` (`extract_constant_C`): add a sibling `extract_constant_double` that parses `gate_value`'s `extra` as `float8`. Existing int path stays.
- Verify gates round-trip through `MMappedCircuit` and survive a Postgres restart.

**Tests**: `test/sql/continuous_basic.sql` — create distributions, dump them, restart-survival is implicit because pg_regress runs in a live cluster. Add to `test/schedule.common`.

**Commit gate**: `make installcheck` green, commit "TODO continuous_distributions: random_variable type, gate_rv/gate_arith, float8 value parsing".

### Priority 2 — Sampler: continuous Monte Carlo

- Extend `src/CircuitFromMMap.{cpp,hpp}` to load the new gate types (Boost-deserialise into the in-memory circuit).
- In `src/BooleanCircuit.cpp:287-307` (`monteCarlo`): switch leaf RNG from `rand()` to `std::mt19937_64` seeded from a new GUC `provsql.monte_carlo_seed` (int, registered alongside the existing GUCs in `src/provsql.c:3557-3626`); the Bernoulli path inherits this RNG. Add a per-call `unordered_map<uuid, double>` cache. Implement the four cases from the TODO: `gate_rv` → fresh draw via `<random>` (memoised by UUID per tuple-MC iteration); `gate_arith` → recurse on children, combine per `info1`; `gate_value` reached via an RV path → parse `extra` as `float8` (using `extract_constant_double`); `gate_cmp` over RV children → sample both operand sub-DAGs, return the comparator. Aggregate-vs-constant `gate_cmp` branch unchanged.
- Tests at this stage are circuit-level (no SQL rewriting yet): use `create_gate(...)` to hand-build small circuits and call `probability_evaluate(token, 'monte-carlo', n)` with the seed GUC pinned for determinism.

**Tests**: `test/sql/continuous_sampler.sql` — covers each new gate type, the per-iteration memoisation property (the same RV UUID inside an arith expression must use the same draw), and seed reproducibility.

**Commit gate**: `make installcheck` green, commit "TODO continuous_distributions: monteCarlo over gate_rv/gate_arith with std::mt19937_64".

### Priority 3 — Operator overloading: arithmetic and inequality

- In `sql/provsql.common.sql`: SQL operators `+ - * /` over (`random_variable × random_variable`) and (`random_variable × numeric`), each building a `gate_arith` with the right `info1` op tag and returning a `random_variable`. Comparison operators `< <= = <> >= >` over the same pairs, each building a `gate_cmp` and returning a Boolean token (`uuid`). Mirror `agg_token`'s 12 operator declarations at `sql/provsql.common.sql:1259-1358`.
- Tests at this stage hand-write the `provsql` column to verify that arithmetic and comparisons compose correctly, before the planner hook automates it.

**Tests**: `test/sql/continuous_arithmetic.sql` — `(a+b) > c` shapes, mixed `random_variable × numeric`, commutators, identity laws.

**Commit gate**: `make installcheck` green, commit "TODO continuous_distributions: SQL operators over random_variable".

### Priority 4 — Planner hook: SELECT, WHERE, JOIN, UNION on pc-tables

- `src/provsql.c:407-531` (`get_provenance_attributes`): recognise `random_variable` columns alongside `provsql UUID`.
- `src/provsql.c:1101-1392` (`make_provenance_expression`): when a comparison node anywhere in WHERE/SELECT compares a `random_variable` Var, translate it into a `gate_cmp` (reuse `provenance_cmp`, `sql/provsql.common.sql:586-611`) and conjoin the resulting Boolean token into the tuple's provsql via `provenance_times`. RV arithmetic in SELECT expressions creates `gate_arith`.
- Splice: WHERE clauses consumed by the rewriter are replaced with `TRUE` (peephole pruning passes from priority 5 fold the resulting comparators back where they decide). JOIN / UNION ALL paths already use `SR_TIMES` / `SR_PLUS` and need no change.

**Tests**: `test/sql/continuous_selection.sql`, `continuous_join.sql`, `continuous_union.sql`. The sensors example from the TODO (s1 normal(2.5, 0.5), s2 uniform(1,3); `WHERE reading > 2`; expect ≈0.84 and ≈0.50 with `monte_carlo_seed` pinned and large `n`) anchors the end-to-end test.

**Note — agg_token ↔ random_variable parallel.** The walker added in this priority (`extract_rv_cmps_from_quals` + `check_expr_on_rv`) is structurally isomorphic to `migrate_aggtoken_quals_to_having` + `check_expr_on_aggregate`. See the new "Cross-cutting: agg_token / random_variable unification" section below; the unified-classifier refactor and the sampler-side hybrid path are tracked there.

**Commit gate**: `make installcheck` green, commit "TODO continuous_distributions: planner-hook rewriting for random_variable".

### Priority 5 — Pruning and exact shortcuts

Two peephole shortcuts that run before MC, ordered cheapest-first. Both are sound: they only collapse cmp gates whose probability is provably 0, 1, or a closed-form value. Anything they cannot decide falls through to MC unchanged.

**(a) Support-based bound check (RangeCheck per the thesis).** Each distribution has a known support (uniform [a, b], exponential [0, ∞), normal ℝ). Interval arithmetic propagates `[lo, hi]` bounds through `gate_arith`: `[a₁,b₁] + [a₂,b₂]`, `× [c, d]`, etc. For a `gate_cmp` against a constant, if the propagated interval is strictly above/below, the comparator's probability is 0 or 1 exactly. No external dependency. New file `src/RangeCheck.{h,cpp}`.

**(b) Analytic CDF for closed-form `gate_cmp`.** When a comparator reduces to `X cmp c` for a single named distribution X with a closed-form CDF, return `F(c)` or `1 − F(c)`. Same for `X cmp Y` when X, Y are independent normals (reduce to `(X − Y) > 0` via Φ). New file `src/AnalyticEvaluator.{h,cpp}`. CDFs computed inline against `<cmath>` (`std::erf` for the normal, `std::expm1` for the exponential, arithmetic for uniform); no external math dependency.

The original plan also included an LP-based BoundCheck via `lp_solve` to decide tuples where (a)+(b) cannot but the joint feasibility of multiple comparators on shared RVs matters. After (a)'s joint-conjunction pass landed (intersecting per-RV intervals across AND-conjunct cmps) and Priority 7 took on the residual structurally-correlated cases via island decomposition + the simplifier, the LP path no longer earns its build-system + external-dependency cost. Dropped; revisit only if a workload turns up that neither (a)'s joint pass nor Priority 7 handles.

**Wire-up.** Both run as a peephole pass that, when it can decide a `gate_cmp`, replaces it by a Bernoulli `gate_input` with the determined probability — making the result transparent to *every* downstream evaluator (MC, independent, treedec, d-DNNF, d4), not just MC. Pass owned by `src/probability_evaluate.cpp` before sampler dispatch.

**GUCs.** None: (a) and (b) are unconditional — they're cheap and never wrong.

**Tests**: `test/sql/continuous_rangecheck.sql`, `continuous_rangecheck_having.sql`, `continuous_analytic.sql`. Cover support-based pruning (`U(1, 2) > 0` returns exactly 1.0, `−U(1, 2) > 0` returns exactly 0.0), analytic CDF (`N(0, 1) > 0` returns 0.5 exactly without MC, `U(0, 1) ≤ 0.3` returns 0.3).

**Commit gate**: `make installcheck` green, commit "TODO continuous_distributions: peephole pruning (RangeCheck, analytic CDF)".

### Priority 6 — Expected value and moments

- New file `src/semiring/Expectation.h`: compiled semiring computing `E[·]` over a circuit. `gate_rv` returns the analytical expectation per distribution (`μ` for normal, `(a+b)/2` for uniform, `1/λ` for exponential). `gate_value` returns the literal. `gate_arith(PLUS)` sums child expectations. `gate_arith(TIMES)` returns `∏ E[X_i]` iff children's base-RV footprints are disjoint; otherwise falls back to MC on the offending sub-circuit.
- A small precomputation: per gate, a Bloom filter (or `std::set<uuid>`) of base `gate_rv` UUIDs reachable below it, used for the structural independence test on TIMES.
- Wire `expectation` into the dispatch table at `src/provenance_evaluate_compiled.cpp:290-335` (FLOAT block).
- SQL functions in `sql/provsql.common.sql`: `expected_value(rv random_variable) -> float8`, `variance(rv random_variable) -> float8`, `moment(rv random_variable, k integer) -> float8`. Aggregate forms come for free from the existing aggregation pipeline.
- Higher moments via the same descent with the appropriate algebraic identities (variance via `Var(X+Y) = Var(X) + Var(Y) + 2 Cov(X,Y)`, with covariance zero when independent; closed form for the basic distributions, MC otherwise).

**Tests**: `test/sql/continuous_expectation.sql` — analytical case (sum of normals), structurally-independent case (product of disjoint normals), and MC fallback (product of two expressions sharing a base RV).

**Commit gate**: `make installcheck` green, commit "TODO continuous_distributions: Expectation semiring and moment SQL functions".

### Priority 7 — Hybrid evaluation: simplifier and island decomposition

The peephole pass from Priority 5 collapses *constant-probability* comparators. This priority generalises it in two directions: (a) simplify continuous sub-circuits via exponential-family closure rules so the analytic CDF from 5(b) reaches further, and (b) factor the circuit into a Boolean wrapper plus continuous "islands" so the existing structural evaluators (`independent`, `treedec`, `d-DNNF`, `d4`) can still evaluate the Boolean part once the islands are marginalised.

**(a) Peephole simplifier on `gate_arith`** (evaluation-time, never mutates the persisted DAG). Closure rules worth implementing:
- Linear combinations of *independent* normals: `aX + bY → N(aμ_X + bμ_Y, a²σ²_X + b²σ²_Y)` iff X and Y reach disjoint base-RV UUIDs (the same independence test as Priority 6's Expectation semiring; share the implementation).
- Sums of i.i.d. exponentials with the same rate → Erlang. Different rates: skip (hypoexponential is messy, MC is fine).
- Trivial folds: `arith(PLUS, 0, X) → X`, `arith(TIMES, 1, X) → X`, etc. — these are essentially constant folding and complement (a)+(b) of Priority 5.
- Constant folding of `gate_arith` over all-`gate_value` subtrees: collapse to a single `gate_value`. Concrete motivating case: `(reading > -100)` parses as `rv_cmp_gt(reading, -(100::random_variable))`, producing `gate_arith(NEG, gate_value:100)` rather than `gate_value:-100`. RangeCheck (per-cmp interval pass and joint AND-conjunction pass) only treats direct `gate_value` / `gate_rv` operands as constants today; tests parenthesise as `(-100)::random_variable` to sidestep this. Once the simplifier folds `arith(NEG, value:100) → value:-100` (and analogously for other unary/n-ary arith over constants), the parenthesisation can be removed and the corresponding branches in RangeCheck and AnalyticEvaluator will start firing on naturally-written predicates.

Skip uniform + uniform (Irwin–Hall, not in our family), normal × normal (product distribution), exponential × constant (becomes a different exponential — only marginally useful).

**(b) Island decomposition.** A circuit factors into a *Boolean wrapper* (input/plus/times/monus over `gate_cmp` outcomes and Bernoulli leaves) plus *continuous islands* — connected components in `arith`/`rv` whose only outward edges leave through `gate_cmp` gates.
- **Each island feeds a single cmp** ⇒ marginalise it (analytically when possible per Priority 5(b)+(a) above, MC otherwise), replace the cmp with a Bernoulli `gate_input` carrying the marginal. The resulting purely-Boolean circuit feeds *every* existing evaluator unchanged.
- **One island feeds multiple cmps** ⇒ the cmps are jointly distributed. Compute the 2^k joint table over the k cmps sharing that island (analytically when feasible, MC otherwise) and inline it as a small Boolean sub-circuit. Tree decomposition still applies as long as k per island is small (the realistic case for HAVING/WHERE).
- Whole-circuit MC (Priority 2 behaviour) stays as a fallback.

**Wire-up.** New file `src/HybridEvaluator.{h,cpp}` owns the simplifier and the decomposer. `src/probability_evaluate.cpp` runs the peephole pass (Priority 5) → simplifier (7a) → decomposer (7b) before dispatching to a Boolean-only evaluator selected by the existing `'monte-carlo' / 'independent' / 'treedec' / 'd-DNNF' / 'd4'` method string. New GUC `provsql.hybrid_evaluation` (bool, default `on`) lets us A/B against pure MC during development.

**Note — sampler-side unification with agg_token.** The island decomposer's marginalisation step is exactly the analogue of `provsql_having`'s possible-world enumeration (which the legacy boolean MC path uses to resolve HAVING `gate_cmp` over `gate_agg`). Implementing 7b should let `monteCarloRV` handle `gate_cmp(gate_agg, …)` for the first time, removing the priority-4 limitation that made `continuous_selection.sql` section G structural-only. See the cross-cutting section below; if the unified classifier from priority 4's follow-up has already landed, the routing of mixed `agg ⊗ rv` conjuncts in WHERE/HAVING falls out of it.

**Tests**: `test/sql/continuous_hybrid.sql`:
- Sum of two independent normals against a constant ⇒ exact via simplifier + analytic CDF, no MC noise (`abs(p − truth) < 1e-12`).
- Boolean disjunction over two cmps with disjoint islands ⇒ marginalised independently, then evaluated with `'independent'` → matches inclusion-exclusion analytically.
- Boolean structure over two cmps that share an RV (e.g. `X > 0 OR X > 1`) ⇒ joint table on the shared island, then treedec on the wrapper. The dependence-aware result must equal `P(X > 0)` exactly (the OR-of-dependent special case), not the MC-leaning estimate of independent ORs.
- Sanity: with `provsql.hybrid_evaluation = off`, the same queries fall through to whole-circuit MC and produce within-tolerance answers.

**Doc note.** `doc/source/dev/probability-evaluation.rst` gains an "Hybrid evaluation for continuous distributions" subsection: peephole pruning, family-closure simplifier, island decomposition.

**Commit gate**: `make installcheck` green, commit "TODO continuous_distributions: hybrid evaluation (simplifier + island decomposition)".

### Priority 8 — Studio

All inside the existing Circuit mode. No new top-level mode.

- **Frontend renderer**: add CSS rules `.node--rv` / `.node--arith` in `studio/provsql_studio/static/app.css` (mirroring `.node--leaf` at lines 1772-1783) and conditional label branches in `studio/provsql_studio/static/circuit.js` (mirroring the `'project'` / `'value'` / `'agg'` branches at lines 689-703). For `gate_rv`: distribution name and parameters with a small inline analytical-PDF thumbnail (no sampling at the leaf — closed form for normal / uniform / exponential). For `gate_arith`: operator glyph from `info1` (`+`, `-`, `×`, `÷`, `−x`).
- **Server-side serialiser**: extend the SQL CASE block in `studio/provsql_studio/circuit.py:165-175` if special `info1`/`info2` extraction is needed for the new types.
- **Node inspector**: distribution preview — analytical density for `gate_rv` leaves, empirical histogram from a quick MC pull for `gate_arith` and `gate_cmp` over RVs.
- **Landed (Studio frontend, scalar-root tooling, demo, e2e).** Distribution-profile evaluation method (C SRF `provsql.rv_histogram`, Studio backend route, symmetric dropdown filter, inline-SVG profile panel, unit tests); simplified-circuit rendering driven by `provsql.simplify_on_load` (new `get_simplified_*` accessors + serialiser switch); config-panel rows for `monte_carlo_seed` / `rv_mc_samples` / `simplify_on_load` with the screenshot recapture; `studio/scripts/demo_continuous.{sh,py}` demo loader; Playwright e2e at `studio/tests/e2e/test_continuous.py` (sensors fixture with `gate_arith` + `gate_rv` shapes, `WHERE reading > 2`, asserts the new gate types render with their specific labels and that monte-carlo returns `p ∈ (0, 1)`).
- **User documentation**: new "Continuous distributions" subsection in `doc/source/user/studio.rst`, with one circuit-panel screenshot showing a `gate_rv` node with its preview, and one of the side-by-side comparison.

**Commit gate**: Studio CI green (lint + matrix + Playwright); commit "TODO continuous_distributions: Studio renderer, inspector preview, demo, e2e".

### Priority 9 — Polish

- **`doc/source/user/continuous-distributions.rst`** (new): tutorial covering the sensors example, RV arithmetic, expected / variance / moment / central_moment / support, and the `monte_carlo_seed` / `rv_mc_samples` / `simplify_on_load` GUCs.
- **`doc/source/user/probabilities.rst`**: cross-reference the new continuous-distributions page.
- **`doc/source/user/studio.rst`**: bump the compatibility table (extension floor) in lockstep with the extension version that ships these gate types.
- **`doc/source/dev/probability-evaluation.rst`**: architecture section on continuous Monte Carlo, the Expectation semiring, and structural independence detection.
- Run `make docs` (per CLAUDE memory).
- Leave `EXCEPT`, `DISTINCT`, `GROUP BY` / HAVING with RV operands as a clearly-marked TODO at the bottom of `doc/source/user/continuous-distributions.rst`.

**Commit gate**: docs build clean, commit "TODO continuous_distributions: user manual, dev architecture, compatibility-floor bump".

### Priority 10 — Aggregation over `random_variable` (originally out of scope; partially landed)

When the operation plan was drafted, aggregation over RVs was deferred because the existing semimodule-provenance machinery (`agg_token`, `provenance_semimod`, `agg_raw_moment`) is wired for numeric `M`: each per-row value lands in a `gate_value`'s `extra` blob via `CAST(val AS VARCHAR)`, which is nonsensical for a `random_variable`. The follow-up landed in increments on this branch, in the order SUM → AVG → PRODUCT.

**Strategy: inline `gate_arith`-of-mixtures rewrite, not M-polymorphic `gate_agg`.** Each RV-returning aggregate is implemented as a pl/pgsql aggregate whose FFUNC builds a `gate_arith`-of-mixtures shape directly, parallel to the existing `gate_agg`/`gate_semimod` shape for numeric M:

```
SUM(x)  ⟶  gate_arith(PLUS, [mixture(prov_1, X_1, as_random(0)),
                              mixture(prov_2, X_2, as_random(0)),
                              …])
```

The planner-hook dispatch in `make_aggregation_expression` (`src/provsql.c`) routes on `agg_ref->aggtype == OID_TYPE_RANDOM_VARIABLE`, so *any* aggregate whose result type is `random_variable` gets its per-row argument wrapped in `mixture(prov_i, X_i, as_random(0))` via the `rv_aggregate_semimod` helper. The wrap routine is named `make_rv_aggregate_expression` and preserves the original `aggfnoid`, so future RV-returning aggregates require zero C delta — only a pl/pgsql FFUNC and `CREATE AGGREGATE`. The semantic content is `SUM(x) = Σ_i 1{φ_i} · X_i` — the direct extension of semimodule-provenance from numeric M to RV-valued M. `MonteCarloSampler::evalScalar` walks the resulting tree with `bool_cache_` coupling provenances and `rv_cache_` coupling RVs; `Expectation::rec_expectation` collapses the closed-form branch to `Σ P(prov_i) · E[X_i]` (linearity, regardless of dependence between the `φ_i` and the `X_i`); `RangeCheck::intervalOf` returns `[Σ min, Σ max]` over branches whose indicator can be true.

The M-polymorphic alternative (extend `gate_agg`/`gate_semimod` to carry RV-rooted values) was considered and deferred. Its only structural advantage is representational uniformity (one shape for SUM-over-numeric vs SUM-over-RV); it would require allowing `gate_semimod`'s second child to be any scalar RV root and auditing every consumer of `(get_children(pair))[2]` (`aggregation_evaluate`, `agg_raw_moment`, the Studio aggregate panel). No capability gap motivates it today — variance and higher moments come from `Expectation::rec_*` over the existing `gate_arith` shape, which already handles dependence correctly.

**Landed aggregates** (each `provsql.<name>(random_variable) → random_variable`):

- `sum` — `SUM(x) = Σ_i 1{φ_i} · X_i`. Empty group returns `as_random(0)` (additive identity). `sum_rv_sfunc` collects per-row UUIDs; `sum_rv_ffunc` builds `gate_arith(PLUS, …)`. Singleton group returns the single child without minting a useless single-child arith root.
- `avg` — `AVG(x) = SUM(x) / Σ_i 1{φ_i}`. Empty group returns SQL `NULL` (matching standard SQL `AVG`, intentionally differing from `sum`'s `as_random(0)`: the additive identity is the right empty-SUM, no multiplicative identity is the right empty-AVG). `avg_rv_ffunc` walks each state UUID, recovers `prov_i` from each mixture's first child, builds the matching `mixture(prov_i, as_random(1), as_random(0))` for the denominator via the same `rv_aggregate_semimod` helper, and emits `gate_arith(DIV, sum_num, sum_denom)`. State entries whose gate type is not `mixture` (direct/untracked call) take an unconditional `as_random(1)` denominator contribution, so `SELECT avg(x) FROM untracked_t` returns `sum(x) / as_random(n)`.
- `product` — `PRODUCT(x) = Π_i (X_i if φ_i else 1)`. Empty group returns `as_random(1)` (multiplicative identity). `product_rv_ffunc` patches each mixture's else-branch from `as_random(0)` to `as_random(1)` by reconstructing it through `provsql.mixture` (preserving v5-hash sharing with any other path that produces the same `(prov, X_i, as_random(1))` triple), then builds `gate_arith(TIMES, …)`. The "in-FFUNC fix-up" route was chosen over introducing a second wrapper helper (`rv_aggregate_semimod_one`) because it keeps the C dispatch trivial — the per-row cost is one extra `provsql.mixture` call, dominated by the actual gate construction anyway.

The `INITCOND='{}'` convention makes the FFUNC fire even on empty input, which is how each aggregate decides its empty-group identity.

**AVG division-by-zero in the all-`φ_i`-false world.** AVG produces `gate_arith(DIV, sum_num, sum_denom)` where numerator and denominator share per-row provenance, so in the world where every `φ_i` is false both reduce to `as_random(0)` and the gate evaluates to `0/0 = NaN`. Under MC, `provsql.expected(avg(x))` returns NaN whenever `P(all φ_i false) > 0`. Two mitigations are deferred:

1. **User-side conditioning.** An SFUNC variant minting a conditioning gate so `provsql.expected(...)` returns `E[AVG | at least one row]`.
2. **NULL-on-NaN finalisation.** Detect a provably-zero-denominator world via `RangeCheck` over the denominator subtree and short-circuit to SQL `NULL`. RangeCheck extension, not a planner-hook change.

**Tests**: `test/sql/continuous_aggregation.sql` covers six SUM cases (basic, uncertain provenance, coupling through shared atoms, coupling through shared RVs, empty group, COUNT(*) lift), four AVG cases (basic, uncertain-but-nonempty, empty group, direct/untracked), and three PRODUCT cases (basic, uncertain provenance, empty group). All on a green `make installcheck` (128/128).

**Commits**:
- `d92292a` continuous_aggregation: rv_sum aggregate + planner rewrite of rv_sum(rv_col) into gate_arith of mixtures
- `8d45a7c` continuous_aggregation: rename rv_sum aggregate to sum (overload of the standard sum)
- `f82c635` continuous_aggregation: dispatch RV-returning aggregates on aggtype, not aggfnoid
- `2dbb94d` continuous_aggregation: avg aggregate over random_variable
- `0a73de8` continuous_aggregation: product aggregate over random_variable

#### Feasibility of further aggregates over `random_variable`

With the aggtype-based dispatch in place, the cost of a new RV-returning aggregate collapses to "pl/pgsql FFUNC + `CREATE AGGREGATE`." The question becomes which aggregates have a clean lowering into the existing gate algebra and which need new gate primitives.

**Tier A — trivial extensions of the SUM template.**
- `sum_squares(x)` is already expressible as `sum(reading * reading)` via the existing `*` operator on `random_variable` — no new aggregate needed; document the pattern in the user manual.

**Tier B — composable from SUM/AVG via `gate_arith` (next to land).**
- `covar_pop(x, y) = E[XY] − E[X]E[Y]`, with a per-row `X_i · Y_i` cross-term. This is the headline Tier B candidate: captures cross-column joint behaviour over the group (portfolio correlations, sensor cross-correlations, regression-style covariance) and is not expressible as a composition of single-RV moments, because the per-row joint draw of `X` and `Y` carries information that no single-column aggregate exposes.
- `var_pop(x) = avg(x²) − avg(x)²` and `var_samp(x)` (Bessel-corrected) are mechanically straightforward but pull weak weight. Priority 11's `variance(rv)` already covers the canonical single-distribution case (`variance(provsql.normal(2.5, 0.5)) = 0.25`); the aggregate form gives the sample variance across the group as a `random_variable`, which a user can already build by hand via `avg(x*x) - avg(x)*avg(x)` against the existing aggregates and the operator overloads.  Worth landing if a real workload turns up that wants the convenience; otherwise the SQL-compatibility motivation alone doesn't justify the FFUNC + tests.

**Tier C — needs a `SQRT` op on `gate_arith`.**
- `stddev(x) = sqrt(var(x))` and `corr_pop(x, y) = covar_pop(x, y) / sqrt(var_pop(x) · var_pop(y))`.  `gate_arith` carries only `{PLUS, TIMES, MINUS, DIV, NEG}` today (`PROVSQL_ARITH_*` in `src/provsql_utils.h`). Adding `SQRT` requires evaluator entries in `MonteCarloSampler::evalScalar` (trivial), `Expectation::rec_*` (no closed form for `E[sqrt(X)]` in general → MC fallback), `RangeCheck::intervalOf` (interval `sqrt` is standard), and the simplifier passes.  Worth doing once a second consumer appears (corr, RMS, geometric mean).

**Tier D — needs `LOG` / `EXP` ops on `gate_arith`.**
- `geomean(x) = exp((1/n) · Σ log X_i)`. Domain check `X_i > 0`. Lower priority than SQRT.

**Tier E — fundamentally different representation.**
- `MIN(rv)` / `MAX(rv)` / `PERCENTILE_CONT` / `PERCENTILE_DISC` / `MEDIAN`. MIN doesn't distribute over indicators the way SUM does; the natural lowering routes through the `gate_mulinput`-over-winner-identity primitive (one mulinput per possible "winner" row, with that row's conditional distribution as the value). This shares the `gate_mulinput`-over-key primitive already used for categorical RVs. Genuinely a separate design pass — flag as a follow-up; left in the out-of-scope follow-ups at the bottom of `doc/source/user/continuous-distributions.rst`.

**Tier F — N/A for `random_variable`.**
- `STRING_AGG`, `BIT_AND`, `BIT_OR`, `BOOL_AND/EVERY`, `BOOL_OR`. `ARRAY_AGG(random_variable)` is fine (returns `random_variable[]`, not a scalar RV — no overload needed).

**Commit gate (whenever a Tier B aggregate lands)**: `make installcheck` green, commit per the same `continuous_aggregation: <name> aggregate over random_variable` convention.

### Priority 11 – Conditional moments / support / histogram / sampling on `random_variable` (originally out of scope; landed)

When Priority 6 drafted the moment family it punted on the "conditioning event" argument: `expected(rv)`, `variance(rv)`, `moment(rv, k)`, `support(rv)`, `rv_histogram(rv, bins)` all returned the *unconditional* answer, and the four functions raised on any non-trivial event.  Doing the conditioning properly requires a joint circuit so the indicator's draw and the value's draw share their per-iteration state (otherwise the truncated answer is biased by the independence assumption); the joint loader didn't exist, so the feature waited.

Joint loader landed.  Each scalar-moment function now takes an optional `prov UUID DEFAULT gate_one()` argument, plus a new `rv_sample(token, n, prov)` SRF that draws raw conditional samples via rejection sampling.  In a planner-hook query the standard idiom is `expected(reading, provenance())` (or any of the moment family); on a row that survived a `WHERE reading > c` filter the planner-hook-lifted cmp becomes the conditioning event automatically.

**Closed-form fast paths.**  When `collectRvConstraints` can fold the event's AND-conjunct rv-op-const cmps into a single interval on a bare `gate_rv`, the moment family uses the truncated-distribution closed form: Normal via Mills ratio + integration-by-parts, Uniform on the intersected support, Exponential by memorylessness on the lower bound (or finite-interval truncation).  Anything else (gate_arith expressions, multiple disjoint events, shared atoms across cmps) falls through to MC rejection on the joint circuit.  `support()` returns the intersected interval directly; `rv_histogram` / `rv_sample` route through MC with the conditioning event applied at the acceptance check.

**Shared-atom coupling.**  The MC rejection sampler loads `token` and `prov` into a single `GenericCircuit` so any `gate_rv` reachable from both roots collapses to a single `gate_t`; that's the invariant `monteCarloConditionalScalarSamples` relies on to couple the indicator's draw with the value's draw.  `MMappedCircuit` grew a `getJointCircuit(token, prov)` entry point; `createGenericCircuit` accepts multiple roots and BFS-merges them.

**Zero-probability events.**  When the conditioning acceptance rate drops below the `provsql.rv_mc_samples` budget can support, the C SRF raises an actionable error naming the GUC; RangeCheck's joint AND-conjunction pass also short-circuits on a provably-infeasible interval.

**Landed pieces.**
- `src/CircuitFromMMap.{cpp,h}`: `getJointCircuit`, multi-root `createGenericCircuit` BFS.
- `src/MMappedCircuit.{cpp,h}`: shared-circuit loader plumbing.
- `src/RangeCheck.{cpp,h}`: joint-interval pass over multiple conjuncts.
- `src/MonteCarloSampler.{cpp,h}`: `monteCarloConditionalScalarSamples`, joint-circuit rejection loop, shared `mt19937_64` state across indicator and value draws.
- `src/Expectation.{cpp,h}`: conditional `rec_expected` / `rec_variance` / `rec_raw_moment` / `rec_central_moment`, truncated closed-form for Normal / Uniform / Exponential.
- `src/RvHistogram.cpp`: `prov` arg threaded into the rejection loop; existing `bins` semantics unchanged.
- `src/RvSample.cpp`: new C SRF returning `SETOF float8`; emits a `NOTICE` when fewer samples than requested land within the budget.
- `sql/provsql.common.sql`: `prov` argument on `rv_moment` / `rv_support` / `rv_histogram`; new `rv_sample(token, n, prov)`; the `expected` / `variance` / `moment` / `central_moment` / `support` SQL dispatchers route `random_variable` inputs through the conditional path.
- `test/sql/continuous_conditioning.sql`: eight sections covering truncated Normal (Mills-ratio mean + second raw moment vs `1 + α·M`), truncated Uniform with two cmps, truncated Exponential (memorylessness + finite interval), `gate_one()` round-trip, WHERE end-to-end, shared-atom coupling, sample/histogram support truncation, zero-probability error.
- Studio: new `Sample` semiring under the Distribution optgroup; `Condition on` text input on the eval strip (`distribution-profile` / `moment` / `sample`); the result-table renderer stamps each clickable cell with `data-row-prov` so the eval strip's `autoPresetConditionInput` defaults the Condition input to the row's provenance the moment the user clicks into a row's circuit.  Auto-preset is row-context aware: clicking the `random_variable` cell (scene root = the rv itself) still conditions on the row.  Manual edits stick within a row, get overwritten on row navigation.  The `Conditioned by:` chip is a toggle: clicking the active chip clears the conditioning (unconditional answer); clicking the muted chip restores the row prov.  `rv_sample` result renders as a `<details>` panel with an inline preview of the first six values, expandable full list, and a hint pointing at `provsql.rv_mc_samples` when MC's acceptance rate truncated the run.

**Commits**:
- `a5092e1` continuous_distributions: conditional moments / support / histogram / sampling on rv with planner-hook prov
- `8ae52c4` studio: auto-preset row provenance into the eval-strip Condition input with a toggleable badge
- `664c289` studio: preserve scroll position across eval-strip Run to keep the distribution-profile panel in view on Firefox
- `74eadc0` studio: render rv_sample as a details panel with inline preview, full list, and conditional-MC budget hint

**Documentation follow-ups (open).**
- `doc/source/user/continuous-distributions.rst` needs a "Conditional inference" subsection: the closed-form table (Normal / Uniform / Exponential truncations), the `provenance()` idiom in tracked queries, the `provsql.rv_mc_samples` budget GUC, the `rv_sample` SRF.
- `doc/source/user/studio.rst` needs a paragraph on the `Conditioned by:` badge + Sample semiring + the row-prov auto-preset workflow.
- `doc/source/dev/probability-evaluation.rst` needs an "Conditional evaluation" subsection (joint circuit, closed-form decision tree, MC rejection invariants).

**Open follow-ups.**
- Variance closed-form via `rec_variance` is currently routed through `rec_raw_moment + rec_expected²` for the truncated branch; if any new distribution lands whose Var formula isn't `m₂ − m₁²` (mixtures, sums) the dispatcher in `Expectation.cpp` will need a direct-variance arm.
- Joint circuit cache currently rebuilds on every call; if profiling shows it's hot, memoise per `(token, prov)` pair the way `circuit_cache` does for plain `getGenericCircuit`.
- Studio: the `[[name]]` link from the `rv_sample` panel back to the `rv_mc_samples` config row is hand-written prose right now; if the Config panel grows a programmatic anchor we can `<a href>` it.

## Files affected (consolidated)

**Modified**:
- `src/provsql_utils.h` — gate_type enum + gate_type_name[] + provsql_arith_op enum.
- `sql/provsql.common.sql` — provenance_gate enum, random_variable type, constructors, operators, expected_value/variance/moment.
- `src/having_semantics.cpp` — `extract_constant_double` sibling.
- `src/provsql.c` — planner hook (discovery + expression building) + new GUCs.
- `src/BooleanCircuit.{cpp,h}` — extended monteCarlo + per-call sample cache + `<random>` switch.
- `src/CircuitFromMMap.{cpp,hpp}` — load new gate types.
- `src/probability_evaluate.cpp` — no API change, routes through extended sampler.
- `src/provenance_evaluate_compiled.{cpp,hpp}` — dispatch entry for `expectation`.
- `test/schedule.common` — new `continuous_*` test entries.
- `studio/provsql_studio/static/circuit.js`, `app.css` — new gate-type rendering.
- `studio/provsql_studio/circuit.py` — server-side serialiser tweaks if needed.
- `doc/source/user/probabilities.rst`, `doc/source/user/studio.rst`, `doc/source/dev/probability-evaluation.rst`.

**New**:
- `src/RandomVariable.{cpp,h}` — distribution helpers (parse extra, sample, analytical moments).
- `src/random_variable_type.c` — PG IO functions for `random_variable`.
- `src/semiring/Expectation.h` — Expectation compiled semiring.
- `src/RangeCheck.{cpp,h}` — interval-arithmetic support analysis (Priority 5a).
- `src/AnalyticEvaluator.{cpp,h}` — closed-form CDF for trivial `gate_cmp` (Priority 5b).
- `src/HybridEvaluator.{cpp,h}` — peephole simplifier + island decomposer (Priority 7).
- `test/sql/continuous_basic.sql`, `continuous_sampler.sql`, `continuous_arithmetic.sql`, `continuous_selection.sql`, `continuous_join.sql`, `continuous_union.sql`, `continuous_boundcheck.sql`, `continuous_expectation.sql`, `continuous_hybrid.sql` (+ `expected/*.out`).
- `studio/scripts/demo_continuous.{sh,py}`.
- `studio/tests/e2e/test_continuous.py`.
- `doc/source/user/continuous-distributions.rst`.

## Verification

Per CLAUDE memory: between any code change and `make installcheck`, the user runs `sudo make install && sudo service postgresql restart`. I will not run sudo and will ask the user to install.

- After every priority: `make -j$(nproc)` clean (no new warnings); `make installcheck` green.
- After priority 4: end-to-end demo from the TODO succeeds within sampling error:
  ```sql
  CREATE TABLE sensors(id text, reading provsql.random_variable);
  INSERT INTO sensors VALUES ('s1', provsql.normal(2.5, 0.5)), ('s2', provsql.uniform(1, 3));
  SELECT add_provenance('sensors');
  SET provsql.monte_carlo_seed = 42;
  SELECT id, probability_evaluate(provsql, 'monte-carlo', '100000')
    FROM (SELECT id, reading FROM sensors WHERE reading > 2) t;
  -- s1 ≈ 0.84, s2 ≈ 0.50
  ```
- After priority 5: `P(N(0, 1) > 0)` returns exactly 0.5 (analytic CDF); `P(U(1, 2) > 0)` returns exactly 1.0 (RangeCheck); a tuple whose two AND-conjunct cmps have empty joint per-RV interval (e.g. `X > 5 AND X < 0` for the same `X ~ N(0, 1)`) is pruned by the joint-conjunction pass.
- After priority 6: `expected_value(provsql.normal(2.5, 0.5))` returns 2.5 closed-form; `expected_value(s1.reading + s2.reading)` returns 4.5 (linearity); a structurally-dependent product falls back to MC and matches a hand-computed value within sampling error.
- After priority 7: a Boolean disjunction over two cmps with disjoint islands evaluated with `'independent'` matches inclusion-exclusion analytically; `X > 0 OR X > 1` (shared RV) equals `P(X > 0)` exactly via the joint table on the shared island; whole-circuit MC (`provsql.hybrid_evaluation = off`) still matches within sampling error.
- After priority 8: Studio CI green; manual smoke test of the demo fixture in a real browser, capturing the two new screenshots for `doc/source/_static/studio/`.
- After priority 9: `make docs` clean; the new tutorial renders correctly.

## Cross-cutting: agg_token / random_variable unification

`agg_token` and `random_variable` are two presentations of the same algebraic object: a *probabilistic scalar* paired with a UUID that encodes its possible-world structure. Both ride the same comparator surface (`< <= = <> >= >`), both materialise into `gate_cmp` UUIDs, both want the planner hook to lift comparisons out of WHERE and route them into provenance. Where they differ is the distribution machinery — `agg_token`'s is *combinatorial* (finite worlds enumerated by `provsql_having` over the surrounding `gate_agg`'s children); `random_variable`'s is *continuous* (sampled by `monteCarloRV`).

Three unification opportunities, ordered by ROI. None are blockers; they are listed here so subsequent priorities don't drift further apart and so the follow-up commits land in the right order.

**(a) Unified WHERE classifier &mdash; landed.** The historical pair `migrate_aggtoken_quals_to_having` and (priority-4-era) `extract_rv_cmps_from_quals` were structurally isomorphic: each walked `q->jointree->quals`, classified each top-level conjunct, and routed pure-X conjuncts somewhere semantic. They have been consolidated into `migrate_probabilistic_quals` in `src/provsql.c`, driven by a single `qual_class` enum (`QUAL_PURE_AGG`, `QUAL_PURE_RV`, `QUAL_DETERMINISTIC`, plus three mixed-error classes). Routing:
- pure-agg → HAVING (existing flow)
- pure-RV → `provenance_times` splice into `prov_atts` (priority 4 flow)
- deterministic (e.g. `id = 's1'`) → leave in WHERE
- mixed `agg ⊗ rv` inside the same Boolean op → distinct error message naming both flavours; the natural place to grow into priority 7
- mixed `prob ⊗ deterministic` inside the same Boolean op → error (same pattern as agg_token's existing rejection of `WHERE c1 = 1 OR agg_count > 5`); the natural place to grow into a CASE-based lift in priority 7

No on-disk or behaviour change in the supported cases; the new mixed-`agg_token`-and-`random_variable` error is a previously unhandled edge case that would have errored anyway, just less informatively.

**(b) Unified scalar-source dispatch in MC (medium effort, unlocks HAVING+RV).** Today `monteCarloRV::evalScalar` handles `gate_value`, `gate_rv`, `gate_arith`; `gate_agg` throws. Adding a `gate_agg` arm that calls the same possible-world enumeration `provsql_having` uses (or shares its core) closes the gap that forced `continuous_selection.sql` section G to be structural-only. This is the natural intersection of priority 7's island decomposer (which already knows how to marginalise non-Boolean gates) and the existing HAVING resolution; bake it in there. After this, `WHERE rv > 0 GROUP BY x HAVING count(*) > 1` runs end-to-end under MC.

**(c) Type-level merger (low value).** Defining a common base type that both `agg_token` and `random_variable` inhabit looks tempting algebraically but pays poorly: their physical layouts differ (117 B vs 24 B), `agg_token`'s GUC-flipped output (`agg_token_out` reading `provsql.aggtoken_text_as_uuid`) does not generalise, and existing on-disk data would need migration. Skip.

**Implications for downstream priorities.**
- Priority 4 lands the second walker but stays consistent in pattern (a) — no regression in agg_token handling. The classifier refactor is a follow-up.
- Priority 7 should consume (b) as part of its island decomposer rather than reinvent it.
- Studio (priority 8) renders both `gate_agg` and `gate_rv` as scalar-source nodes; the inspector preview can share a common widget that asks the source for "give me a value distribution" rather than special-casing each gate type.

## Risks and follow-ups

- **`MMappedCircuit::setInfos` / `setExtra` do not lazy-create gates** (only `setProb` does). Ensure the `gate_rv` constructor goes through a path that *does* create the gate before setting `extra` on it. Likely pattern: emit a `setProb` (or equivalent gate-creation IPC) first, then `setExtra` for the distribution parameters.
- **Monte Carlo seeding contract**. The `provsql.monte_carlo_seed` GUC must be respected by all samplers (Bernoulli + continuous + boundcheck negative tests) so that regression tests are deterministic with large `n`.
- **Compatibility floor bump** on Studio happens in lockstep with the extension version that ships these gate types; CHANGELOG entries are written by the maintainer at release time per CLAUDE memory.
- **Out-of-scope follow-ups** to leave clearly TODO'd at the bottom of `doc/source/user/continuous-distributions.rst`: EXCEPT, DISTINCT, MIN/MAX/percentile aggregates over RVs (Priority 10's Tier E), where-provenance × RVs, in-Studio distribution editor.
