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

A new `random_variable` composite type (UUID + cached scalar)
carries a probability distribution per row.  Constructors live in
the `provsql` schema:

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
i.i.d. exponentials with the same rate fold to Erlang;
`c·X`-style shifts thread through mixtures and categoricals;
single-child arith roots and semiring identities collapse). The
**island decomposer** splits multi-cmp queries into independent
sub-problems on shared base-RV footprints. Whole-circuit Monte
Carlo remains as the safety net for anything not analytically
tractable.

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
`rv_sample(token, n, prov)` (SRF over `float8`) and
`rv_histogram(token, bins, prov)` (returning `jsonb`) expose
conditional samples and histograms for inspection and downstream
analytics.

Closed-form truncated distributions cover Normal (Mills ratio),
Uniform (intersected support), and Exponential (memorylessness on
a lower bound, finite-interval truncation via the lower
incomplete gamma). Anything outside the closed-form table falls
back to MC rejection sampling at `provsql.rv_mc_samples`; a
`NOTICE` (or, for histograms / moments, an error) fires when
fewer than the requested `n` samples land within the budget.

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

## Studio companion release

ProvSQL Studio 1.1.0 ships in parallel on PyPI as
`provsql-studio==1.1.0`; minimum required extension version is
1.5.0. The new Studio features include the distribution-profile
panel (μ/σ², histogram, PDF/CDF toggle, wheel zoom), the
`Sample` evaluator with conditional-MC budget hints, the
`Condition on` row-prov auto-preset, simplified-circuit rendering
driven by `provsql.simplify_on_load`, and Config-panel rows for
`monte_carlo_seed`, `rv_mc_samples`, `simplify_on_load`. See
`studio/RELEASE_NOTES.md` (staging) for details.

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
- `ALTER EXTENSION provsql UPDATE` is sufficient.

## Out of scope / open follow-ups

- `EXCEPT` and `SELECT DISTINCT` on `random_variable`-bearing
  relations.
- `MIN`, `MAX`, percentile aggregates over `random_variable`;
  the broader `covar_pop` / `stddev` family.
- Where-provenance crossed with random variables.
- An in-Studio distribution editor.
