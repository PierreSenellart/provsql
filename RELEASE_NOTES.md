# Draft release notes for ProvSQL 1.5.0

Staging file. Hand off to `release.sh`'s `$EDITOR` buffer at tag
time, which will propagate the content into
`provsql.common.control`, `CHANGELOG.md`,
`website/_data/releases.yml`, `CITATION.cff`, and `META.json`.
Delete this file after the tag is cut.

## What's new in 1.5.0

Major release headlining first-class **continuous random-variable
columns** and a hybrid analytic + Monte Carlo evaluator. The gate
ABI is extended (three new gate types appended; no renumbering of
older values); the mmap circuit format is otherwise compatible
and an `ALTER EXTENSION provsql UPDATE` is sufficient.

## First-class random variables

A new `random_variable` type (a thin UUID wrapper,
binary-coercible with `uuid`) carries a probability distribution
per row.  Constructors live in the `provsql` schema:

- `provsql.normal(μ, σ)`, `provsql.uniform(a, b)`,
  `provsql.exponential(λ)`, `provsql.erlang(k, λ)` for the four
  continuous families;
- `provsql.categorical(probs, outcomes)` for discrete categorical
  random variables;
- `provsql.mixture(p, x, y)` (two overloads: shared Boolean gate
  vs ad-hoc Bernoulli probability) for probabilistic mixtures;
- `provsql.as_random(c)` for deterministic point-mass lifts.

Implicit casts from `integer`, `numeric`, and `double precision`
to `random_variable` make `WHERE reading > 2` work without an
explicit wrapper. Arithmetic operators `+ - * /` and unary `-`
build `gate_arith` over the operands; comparison operators
`< <= = <> >= >` are intercepted at planning time and rewritten
into `gate_cmp` calls conjoined into each row's provenance.

The new gate types `gate_rv`, `gate_arith`, `gate_mixture` are
appended to the `gate_type` enum (with a parallel append to the
SQL `provenance_gate` enum). `gate_value` gains a float8 mode
parsed via `extract_constant_double` in `having_semantics.cpp`.

## Hybrid analytic + Monte Carlo evaluation

A three-stage evaluator decides every probabilistic query
analytically where possible and falls back to Monte Carlo
otherwise:

- **`RangeCheck`** propagates support intervals through
  `gate_arith` and tests every `gate_cmp` against the propagated
  interval; decidable comparators collapse to Bernoulli leaves.
  A joint AND-conjunction pass intersects per-variable intervals
  across conjuncts before the decision.
- **`AnalyticEvaluator`** computes the closed-form CDF for any
  single-distribution `gate_cmp` (Normal via `erf`, Uniform by
  arithmetic, Exponential by `log1p`/`expm1`, Erlang via the
  regularised lower incomplete gamma).
- **`Expectation`** semiring runs analytical mean / variance /
  moments per distribution with structural-independence
  detection on `gate_arith TIMES` via a `FootprintCache`.

The **`HybridEvaluator`** simplifier folds family-preserving
combinations (normals close under linear combination; sums of
i.i.d. exponentials with the same rate fold to Erlang; the
affine shapes `-N`, `-U`, `c+N`, `c-N`, `N-c`, `c-U`, `U-c`,
`U+c` fold via a `MINUS → PLUS` canonicalisation plus a uniform
shift-closure rule; `c·X`-style shifts thread through mixtures
and categoricals; single-child arith roots and semiring
identities collapse; deterministic `gate_arith` subtrees are
folded to `gate_value` at load time). The **island decomposer**
splits multi-cmp queries into independent sub-problems on
shared base-RV footprints. Whole-circuit Monte Carlo remains as
the safety net for anything not analytically tractable.

EQ/NE comparators take an analytical path whenever both sides
have extractable Dirac mass-maps with disjoint random-leaf
footprints, resolving to a sum-product over the discrete masses;
the same shortcut also widens to `gate_arith` composites and to
Bernoulli mixtures whose continuous arm fully covers the
support, so equality checks against an outcome of a categorical
or a mixture resolve symbolically.

Symbolic prints in the simplifier use `std::to_chars`
shortest-roundtrip formatting, so folds like `2 * Exp(0.4)` now
print `exponential:0.2` instead of `0.20000000000000001`.

`provsql.simplify_on_load` (default `on`) runs the universal
peephole pass at load time, so every downstream consumer
(semiring evaluators, MC, `view_circuit`, PROV-XML export,
Studio) sees the simplified form.

## Conditional inference

The polymorphic moment dispatchers `expected` / `variance` /
`moment` / `central_moment` / `support` all accept an optional
`prov uuid DEFAULT gate_one()` argument; passing `provenance()`
from inside a tracked query conditions on the row's filter
event automatically. New companion C entry points
`rv_sample(token, n, prov)` (SRF over `float8`),
`rv_histogram(token, bins, prov)` (returning `jsonb`), and
`rv_analytical_curves(token, prov, n_points)` (SRF returning
`(x, pdf, cdf)` rows; mass-stems for discrete arms) expose
conditional samples, histograms, and closed-form PDF/CDF curves
for inspection and downstream analytics.

Closed-form truncated distributions cover Normal (Mills ratio),
Uniform (intersected support), Exponential (memorylessness on a
lower bound, finite-interval truncation via the lower incomplete
gamma), and Erlang (via the same regularised incomplete-gamma
machinery). The truncation pipeline also handles Bernoulli
mixtures (each arm truncated independently and the surviving mass
renormalised), categoricals (filtered outcomes plus rescaling),
and Diracs (kept or dropped against the conditioning event); a
universally-infeasible truncated subtree short-circuits to a
`NaN`-typed Dirac so downstream evaluators do not fire MC blindly.
On top of the moment fast paths, `rv_sample` and `rv_histogram`
take an inverse-CDF fast path on bare `gate_rv` conditional
events — Uniform / Exponential by memoryless inverse, Normal by
Beasley-Springer-Moro — bypassing MC entirely when the gate is a
single recognised distribution under a closed-form truncation.
Anything outside the closed-form table falls back to MC
rejection sampling at `provsql.rv_mc_samples`; a `NOTICE` (or,
for histograms / moments, an error) fires when fewer than the
requested `n` samples land within the budget.

## Aggregation over random variables

Three RV-returning aggregates: `sum`, `avg`, `product`
(over `random_variable`). They lower to a single `gate_arith`
root over per-row `gate_mixture` children produced by the new
`rv_aggregate_semimod` helper. `aggtype`-based dispatch lets the
planner-hook recognise RV-returning aggregates and wrap the
per-row argument before the SFUNC sees it; the FFUNC pulls the
provenance back out of each mixture's first child to build the
matching denominator (`AVG`) or to patch the multiplicative
identity into the else-branch (`PRODUCT`). The
`INITCOND = '{}'` convention lets each aggregate define its own
empty-group identity (`as_random(0)` for `SUM`, SQL `NULL` for
`AVG`, `as_random(1)` for `PRODUCT`).

`MIN`, `MAX`, `stddev`, `covar_pop`, and percentile aggregates
over `random_variable` are not yet supported and are deferred.

`HAVING` clauses whose outcome collapses to a deterministic
scalar are supported natively, including the natural shape
`HAVING expected(avg(rv)) > 20` (and the analogous
`variance` / `moment` / `central_moment` over an RV
aggregate). The planner skips the HAVING-lift on such quals
and lets PostgreSQL filter the surviving groups directly, while
the per-group `gate_delta` wrapper is still emitted so the
provenance shape is unchanged. Quals that compute on
`agg_token` results (the historical HAVING surface) continue
to route through `having_Expr_to_provenance_cmp`.

## Studio companion release

ProvSQL Studio 1.1.0 ships in parallel on PyPI as
`provsql-studio==1.1.0`; minimum required extension version is
1.5.0. The new Studio features include the distribution-profile
panel (μ/σ², histogram, PDF/CDF toggle, wheel zoom) with a
closed-form analytical overlay drawn on top of the histogram
bars (terracotta SVG path for continuous arms, discs-on-stems
for Bernoulli mixtures / categoricals / Diracs, staircase
overlay in CDF mode), the `Sample` evaluator with conditional-MC
budget hints, the `Condition on` row-prov auto-preset,
simplified-circuit rendering driven by `provsql.simplify_on_load`,
Config-panel rows for `monte_carlo_seed`, `rv_mc_samples`,
`simplify_on_load`, and a footer that surfaces both the
extension and the Studio package versions (plus a new
`--version` CLI flag). See `studio/RELEASE_NOTES.md` (staging)
for details.

## Internal

- Unified `migrate_probabilistic_quals` classifier in
  `src/provsql.c` replaces the historical pair
  `migrate_aggtoken_quals_to_having` + `extract_rv_cmps_from_quals`;
  routes every qual through a `qual_class` enum (pure-RV,
  pure-agg, deterministic, plus mixed-error classes).
- `gate_agg` arm in `monteCarloRV::evalScalar` unlocks HAVING+RV
  under Monte Carlo.
- `gate_delta` is transparent to the rv_* event walker in
  `Sampler::evalBool` and `walkAndConjunctIntervals` so the
  δ-semiring algebra and the random-variable algebra compose
  cleanly.
- `getJointCircuit` in `MMappedCircuit.cpp` builds a multi-rooted
  BFS so shared `gate_rv` leaves between `input` and `prov` are
  loaded into a single `GenericCircuit` and consequently couple
  correctly under MC rejection sampling.
- `random_variable` is now a thin wrapper over `pg_uuid_t` with
  bare-UUID text I/O and a binary-coercible `WITHOUT FUNCTION`
  cast to / from `uuid`; the planner hook emits a `RelabelType`
  instead of a `FuncExpr`. The historical cached-scalar field
  has been removed.
- `runConstantFold` in the load-time simplifier pass folds any
  deterministic `gate_arith` subtree to a single `gate_value`
  (so e.g. `arith(NEG, value:c)` collapses to `value:-c` before
  `asRvVsConstCmp` looks at the cmp).
- `matchTruncatedSingleRv` (in `RangeCheck.h`) factors the
  closed-form single-RV shape detection used by
  `try_truncated_closed_form` / `try_truncated_closed_form_sample`
  / `rv_analytical_curves`, keeping the supported-shape set in
  sync across moments, sampling, and PDF/CDF curves.
- `HybridEvaluator::double_to_text` uses `std::to_chars` for
  shortest-roundtrip formatting of folded scalar coefficients.

## Bug fixes

- Backend segfault at `verbose_level >= 20` when deparsing an
  `EXCEPT`-rewritten tree. `transform_except_into_join` was
  leaving the synthesised `RTE_JOIN` with `NULL` `eref` /
  `joinaliasvars` / `joinleftcols` / `joinrightcols`; execution
  was fine (outer `Var`s reference the inputs directly) but the
  ruleutils deparser walks the rtable and crashed. All four
  fields are now populated on supported PostgreSQL versions
  (`joinleftcols` / `joinrightcols` / `joinmergedcols` are
  guarded for PG &lt; 13). New regression `verbose_setops`
  covers `EXCEPT` and non-`ALL` `UNION`.
- `READM` / `READB` in `provsql_mmap.c` now compare `read()`
  against `(ssize_t)sizeof(...)` so the size check no longer
  promotes to unsigned and masks short-read errors. File-local
  globals are marked `static` and the `-Wmissing-variable-declarations`
  / `-Wunused-result` warnings are clean.
- Tree-mutator / tree-walker callbacks in `src/provsql.c` now
  take `void *` (PostgreSQL's idiom) so clang's
  `-Wcast-function-type-strict` no longer fires at every
  `expression_tree_mutator` / `expression_tree_walker`
  call site; the dead `collect_rv_footprint` helper in
  `HybridEvaluator.cpp` is dropped (its job is done by
  `FootprintCache` in `Expectation.cpp`) and a bare `move()`
  in `where_provenance.cpp` is qualified as `std::move()`.
  Both gcc and clang now build clean.

## GUCs (user-facing)

- `provsql.monte_carlo_seed` (default `-1`): pinning seeds the
  MC sampler for reproducibility across runs and across the
  Bernoulli and continuous sampling paths.
- `provsql.rv_mc_samples` (default `10000`): sample budget for
  the analytical-evaluator MC fallback. Set to `0` to require
  analytical answers (the fallback then raises).
- `provsql.simplify_on_load` (default `on`): runs the universal
  peephole simplifier when circuits are loaded into memory.

`provsql.hybrid_evaluation` is debug-only (`GUC_NO_SHOW_ALL`);
end users have no reason to flip it.

## New documentation

- `doc/source/user/continuous-distributions.rst`: full user
  surface.
- `doc/source/user/casestudy6.rst`: *The City Air-Quality Sensor
  Network*, the first Studio-driven case study.
- `doc/source/dev/continuous-distributions.rst`: architecture
  companion.

## ABI / compatibility

- `gate_type` enum extended (`gate_rv`, `gate_arith`,
  `gate_mixture` appended; no renumbering).
- mmap format compatible with 1.4.0.
- `random_variable` text I/O is bare UUID; the type is binary-
  coercible with `uuid` (cast declared `WITHOUT FUNCTION`), so
  the on-disk and on-wire representations are identical to
  `uuid`. The struct is `pg_uuid_t`.
- `ALTER EXTENSION provsql UPDATE` is sufficient.

## Out of scope / open follow-ups

- `EXCEPT` and `SELECT DISTINCT` on `random_variable`-bearing
  relations.
- `MIN`, `MAX`, percentile aggregates over `random_variable`;
  the broader `covar_pop` / `stddev` family.
- Where-provenance crossed with random variables.
- An in-Studio distribution editor.
