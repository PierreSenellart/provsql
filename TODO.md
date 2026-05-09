# Operation plan: continuous probability distributions

## Context

The branch `continuous_distributions` was opened to bring back continuous-distribution support that was prototyped in Timothy Leong's 2022 NUS BSc thesis but never integrated. The design document at `doc/TODO/continuous_distributions.md` is the authoritative source for *what* to build and *why*; this operation plan is the executable form — sequencing, verification gates, commit points, and corrections to a few line-number references in the TODO.

The TODO already commits to: two new gate types (`gate_rv`, `gate_rv_arith`) appended to the on-disk gate-type ABI; reuse of pre-existing `gate_value`, `gate_cmp`, `gate_input`; a `random_variable` SQL composite type mirroring `agg_token`; planner-hook rewriting of inequalities against RV columns; a continuous Monte Carlo extension to `BooleanCircuit::monteCarlo`; an optional `lp_solve` BoundCheck pruner; an `Expectation` compiled semiring with structural independence detection; and Studio renderer/inspector enhancements that stay inside the existing Circuit mode (no new top-level mode).

Out of scope per the TODO and confirmed: EXCEPT, DISTINCT, aggregation over RVs, where-provenance × RV design work, a "Continuous mode" in Studio, and an in-Studio distribution editor. Each remains a clearly-marked follow-up.

Per user direction (2026-05-09): all eight priorities land on the `continuous_distributions` branch. No PRs needed; commit at the end of each priority once `make installcheck` is green. Priorities 5 (BoundCheck) and 6 (expected value & moments) are firmly in scope.

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
- **External tools** are resolved through `provsql.tool_search_path` GUC and `src/external_tool.h:42-80`'s `run_external_tool` / `find_external_tool` — but this is for *runtime* invocation. `lp_solve` for BoundCheck is a *link-time* dependency, so it needs a new `HAVE_LPSOLVE` autoprobe in `Makefile.internal` (no existing precedent in that file; introduce one).
- **Studio gate rendering** is class-based (`node--<type>` from `studio/provsql_studio/static/circuit.js:361`, with CSS in `studio/provsql_studio/static/app.css:1772-1783`); server-side serialiser is `studio/provsql_studio/circuit.py:165-239`. The new `gate_rv` / `gate_rv_arith` types need both client and server entries.
- **Test schedule**: append new `test: continuous_*` lines to `test/schedule.common`; never edit `test/schedule` (generated).

## Execution sequence

Each priority ends with a green `make installcheck`, a one-liner commit, and (where useful) a small demo SQL script committed alongside the tests. The user runs `sudo make install && sudo service postgresql restart` between code changes (per CLAUDE memory).

### Priority 1 — Foundations: type, constructors, value parsing

- Append `gate_rv`, `gate_rv_arith` to `gate_type` in `src/provsql_utils.h:55-73` (before `gate_invalid`); update `gate_type_name[]`. Declare `provsql_arith_op` enum (PLUS=0, TIMES=1, MINUS=2, DIV=3, NEG=4) with the same "do not renumber" warning.
- Mirror the gate names in `sql/provsql.common.sql:27-44` (`provenance_gate` enum).
- New file `src/random_variable_type.c`: PG IO functions `random_variable_in` / `_out` / `_cast` patterned on `src/agg_token.c:27-120`.
- New file `src/RandomVariable.{h,cpp}`: helpers for parsing the `extra` blob (`"normal:μ,σ"` / `"uniform:a,b"` / `"exponential:λ"`), for analytical moments per distribution, and for sampling (used in priority 2).
- In `sql/provsql.common.sql` after the `agg_token` block (~line 1149): forward decl `random_variable`, IO functions, full type definition. SQL constructors `provsql.normal(numeric, numeric)`, `provsql.uniform(numeric, numeric)`, `provsql.exponential(numeric)`, `provsql.as_random(numeric)` — first three create `gate_rv`; `as_random` creates `gate_value`. All return `random_variable`.
- Extend `src/having_semantics.cpp:100` (`extract_constant_C`): add a sibling `extract_constant_double` that parses `gate_value`'s `extra` as `float8`. Existing int path stays.
- Verify gates round-trip through `MMappedCircuit` and survive a Postgres restart.

**Tests**: `test/sql/continuous_basic.sql` — create distributions, dump them, restart-survival is implicit because pg_regress runs in a live cluster. Add to `test/schedule.common`.

**Commit gate**: `make installcheck` green, commit "TODO continuous_distributions: random_variable type, gate_rv/gate_rv_arith, float8 value parsing".

### Priority 2 — Sampler: continuous Monte Carlo

- Extend `src/CircuitFromMMap.{cpp,hpp}` to load the new gate types (Boost-deserialise into the in-memory circuit).
- In `src/BooleanCircuit.cpp:287-307` (`monteCarlo`): switch leaf RNG from `rand()` to `std::mt19937_64` seeded from a new GUC `provsql.monte_carlo_seed` (int, registered alongside the existing GUCs in `src/provsql.c:3557-3626`); the Bernoulli path inherits this RNG. Add a per-call `unordered_map<uuid, double>` cache. Implement the four cases from the TODO: `gate_rv` → fresh draw via `<random>` (memoised by UUID per tuple-MC iteration); `gate_rv_arith` → recurse on children, combine per `info1`; `gate_value` reached via an RV path → parse `extra` as `float8` (using `extract_constant_double`); `gate_cmp` over RV children → sample both operand sub-DAGs, return the comparator. Aggregate-vs-constant `gate_cmp` branch unchanged.
- Tests at this stage are circuit-level (no SQL rewriting yet): use `create_gate(...)` to hand-build small circuits and call `probability_evaluate(token, 'monte-carlo', n)` with the seed GUC pinned for determinism.

**Tests**: `test/sql/continuous_sampler.sql` — covers each new gate type, the per-iteration memoisation property (the same RV UUID inside an arith expression must use the same draw), and seed reproducibility.

**Commit gate**: `make installcheck` green, commit "TODO continuous_distributions: monteCarlo over gate_rv/gate_rv_arith with std::mt19937_64".

### Priority 3 — Operator overloading: arithmetic and inequality

- In `sql/provsql.common.sql`: SQL operators `+ - * /` over (`random_variable × random_variable`) and (`random_variable × numeric`), each building a `gate_rv_arith` with the right `info1` op tag and returning a `random_variable`. Comparison operators `< <= = <> >= >` over the same pairs, each building a `gate_cmp` and returning a Boolean token (`uuid`). Mirror `agg_token`'s 12 operator declarations at `sql/provsql.common.sql:1259-1358`.
- Tests at this stage hand-write the `provsql` column to verify that arithmetic and comparisons compose correctly, before the planner hook automates it.

**Tests**: `test/sql/continuous_arithmetic.sql` — `(a+b) > c` shapes, mixed `random_variable × numeric`, commutators, identity laws.

**Commit gate**: `make installcheck` green, commit "TODO continuous_distributions: SQL operators over random_variable".

### Priority 4 — Planner hook: SELECT, WHERE, JOIN, UNION on pc-tables

- `src/provsql.c:407-531` (`get_provenance_attributes`): recognise `random_variable` columns alongside `provsql UUID`.
- `src/provsql.c:1101-1392` (`make_provenance_expression`): when a comparison node anywhere in WHERE/SELECT compares a `random_variable` Var, translate it into a `gate_cmp` (reuse `provenance_cmp`, `sql/provsql.common.sql:586-611`) and conjoin the resulting Boolean token into the tuple's provsql via `provenance_times`. RV arithmetic in SELECT expressions creates `gate_rv_arith`.
- Splice: WHERE clauses consumed by the rewriter are replaced with `TRUE` (BoundCheck wrapping comes in priority 5). JOIN / UNION ALL paths already use `SR_TIMES` / `SR_PLUS` and need no change.

**Tests**: `test/sql/continuous_selection.sql`, `continuous_join.sql`, `continuous_union.sql`. The sensors example from the TODO (s1 normal(2.5, 0.5), s2 uniform(1,3); `WHERE reading > 2`; expect ≈0.84 and ≈0.50 with `monte_carlo_seed` pinned and large `n`) anchors the end-to-end test.

**Commit gate**: `make installcheck` green, commit "TODO continuous_distributions: planner-hook rewriting for random_variable".

### Priority 5 — Optional pruning: BoundCheck

- `Makefile.internal`: add an autoprobe `HAVE_LPSOLVE` (probe for `lpsolve55` library + header) and conditionally link `-llpsolve55`. No precedent in the file for build-time tool autodetection — introduce one and document it in `doc/source/dev/build-system.rst`.
- New file `src/BoundCheck.{h,cpp}`, gated by `#ifdef HAVE_LPSOLVE`: walks the Boolean part of the circuit, lowers each `gate_cmp` over RV children into an LP constraint, encodes disjoint support intervals via binary indicators (per the thesis), asks the solver for feasibility.
- New GUC `provsql.use_bound_check` (bool, default `on` if `HAVE_LPSOLVE`, else hardcoded `off`).
- Planner-hook post-pass on the rewritten WHERE: if `BoundCheck` returns false, the row is pruned.

**Tests**: `test/sql/continuous_boundcheck.sql` — one tuple with an unsatisfiable predicate (e.g. `normal(0, 1) > 100 AND normal(0, 1) < -100` with the same RV UUID), verify the row is pruned. Skip the test gracefully when `lp_solve` is not available, following the existing conditional-skip pattern for `c2d` / `d4` / `dsharp`.

**Commit gate**: `make installcheck` green (with and without `lp_solve` available, both paths verified), commit "TODO continuous_distributions: optional BoundCheck pruning behind HAVE_LPSOLVE".

### Priority 6 — Expected value and moments

- New file `src/semiring/Expectation.h`: compiled semiring computing `E[·]` over a circuit. `gate_rv` returns the analytical expectation per distribution (`μ` for normal, `(a+b)/2` for uniform, `1/λ` for exponential). `gate_value` returns the literal. `gate_rv_arith(PLUS)` sums child expectations. `gate_rv_arith(TIMES)` returns `∏ E[X_i]` iff children's base-RV footprints are disjoint; otherwise falls back to MC on the offending sub-circuit.
- A small precomputation: per gate, a Bloom filter (or `std::set<uuid>`) of base `gate_rv` UUIDs reachable below it, used for the structural independence test on TIMES.
- Wire `expectation` into the dispatch table at `src/provenance_evaluate_compiled.cpp:290-335` (FLOAT block).
- SQL functions in `sql/provsql.common.sql`: `expected_value(rv random_variable) -> float8`, `variance(rv random_variable) -> float8`, `moment(rv random_variable, k integer) -> float8`. Aggregate forms come for free from the existing aggregation pipeline.
- Higher moments via the same descent with the appropriate algebraic identities (variance via `Var(X+Y) = Var(X) + Var(Y) + 2 Cov(X,Y)`, with covariance zero when independent; closed form for the basic distributions, MC otherwise).

**Tests**: `test/sql/continuous_expectation.sql` — analytical case (sum of normals), structurally-independent case (product of disjoint normals), and MC fallback (product of two expressions sharing a base RV).

**Commit gate**: `make installcheck` green, commit "TODO continuous_distributions: Expectation semiring and moment SQL functions".

### Priority 7 — Studio

All inside the existing Circuit mode. No new top-level mode.

- **Frontend renderer**: add CSS rules `.node--rv` / `.node--rv_arith` in `studio/provsql_studio/static/app.css` (mirroring `.node--leaf` at lines 1772-1783) and conditional label branches in `studio/provsql_studio/static/circuit.js` (mirroring the `'project'` / `'value'` / `'agg'` branches at lines 689-703). For `gate_rv`: distribution name and parameters with a small inline analytical-PDF thumbnail (no sampling at the leaf — closed form for normal / uniform / exponential). For `gate_rv_arith`: operator glyph from `info1` (`+`, `-`, `×`, `÷`, `−x`).
- **Server-side serialiser**: extend the SQL CASE block in `studio/provsql_studio/circuit.py:165-175` if special `info1`/`info2` extraction is needed for the new types.
- **Node inspector**: distribution preview — analytical density for `gate_rv` leaves, empirical histogram from a quick MC pull for `gate_rv_arith` and `gate_cmp` over RVs.
- **Side-by-side comparison**: pin two `rv` (or `rv_arith`) nodes and overlay their densities, with `P(X > Y)` annotated.
- **Hover info on `rv` nodes**: distribution parameters, plus computed support interval when `BoundCheck` is on. RangeCheck surfaces here.
- **Config-panel rows** for the new GUCs: `provsql.monte_carlo_seed`, `provsql.use_bound_check`. Recapture `doc/source/_static/studio/config-panel.png` per the OS-level `import + convert` flow in CLAUDE.md.
- **Demo script**: `studio/scripts/demo_continuous.{sh,py}` loads the sensors / two-distribution example.
- **Playwright e2e**: `studio/tests/e2e/test_continuous.py` — load demo, run `WHERE rv > 2`, click into the resulting circuit, verify the strip shows `p ∈ (0, 1)` and the new gate types render with their specific labels.
- **User documentation**: new "Continuous distributions" subsection in `doc/source/user/studio.rst`, with one circuit-panel screenshot showing a `gate_rv` node with its preview, and one of the side-by-side comparison.

**Commit gate**: Studio CI green (lint + matrix + Playwright); commit "TODO continuous_distributions: Studio renderer, inspector preview, demo, e2e".

### Priority 8 — Polish

- **`doc/source/user/continuous-distributions.rst`** (new): tutorial covering the sensors example, RV arithmetic, expected_value / variance / moment, and the `monte_carlo_seed` / `use_bound_check` GUCs.
- **`doc/source/user/probabilities.rst`**: cross-reference the new continuous-distributions page.
- **`doc/source/user/studio.rst`**: bump the compatibility table (extension floor) in lockstep with the extension version that ships these gate types.
- **`doc/source/dev/probability-evaluation.rst`**: architecture section on continuous Monte Carlo, the Expectation semiring, and structural independence detection.
- Run `make docs` (per CLAUDE memory).
- Leave `EXCEPT`, `DISTINCT`, `GROUP BY` / HAVING with RV operands as a clearly-marked TODO at the bottom of `doc/source/user/continuous-distributions.rst`.

**Commit gate**: docs build clean, commit "TODO continuous_distributions: user manual, dev architecture, compatibility-floor bump".

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
- `Makefile.internal` — `HAVE_LPSOLVE` autoprobe.
- `test/schedule.common` — new `continuous_*` test entries.
- `studio/provsql_studio/static/circuit.js`, `app.css` — new gate-type rendering.
- `studio/provsql_studio/circuit.py` — server-side serialiser tweaks if needed.
- `doc/source/user/probabilities.rst`, `doc/source/user/studio.rst`, `doc/source/dev/probability-evaluation.rst`.

**New**:
- `src/RandomVariable.{cpp,h}` — distribution helpers (parse extra, sample, analytical moments).
- `src/random_variable_type.c` — PG IO functions for `random_variable`.
- `src/semiring/Expectation.h` — Expectation compiled semiring.
- `src/BoundCheck.{cpp,h}` — gated by `HAVE_LPSOLVE`.
- `test/sql/continuous_basic.sql`, `continuous_sampler.sql`, `continuous_arithmetic.sql`, `continuous_selection.sql`, `continuous_join.sql`, `continuous_union.sql`, `continuous_boundcheck.sql`, `continuous_expectation.sql` (+ `expected/*.out`).
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
  SELECT id, probability_evaluate(provsql, 'monte-carlo', 100000)
    FROM (SELECT id, reading FROM sensors WHERE reading > 2) t;
  -- s1 ≈ 0.84, s2 ≈ 0.50
  ```
- After priority 5: same demo with one tuple replaced by an unsatisfiable predicate; verify it is pruned when `provsql.use_bound_check = on`.
- After priority 6: `expected_value(provsql.normal(2.5, 0.5))` returns 2.5 closed-form; `expected_value(s1.reading + s2.reading)` returns 4.5 (linearity); a structurally-dependent product falls back to MC and matches a hand-computed value within sampling error.
- After priority 7: Studio CI green; manual smoke test of the demo fixture in a real browser, capturing the two new screenshots for `doc/source/_static/studio/`.
- After priority 8: `make docs` clean; the new tutorial renders correctly.

## Risks and follow-ups

- **`lp_solve` autoprobe in `Makefile.internal` has no precedent**. Be prepared to spend extra time on the build-system change in priority 5; it touches `doc/source/dev/build-system.rst`.
- **`MMappedCircuit::setInfos` / `setExtra` do not lazy-create gates** (only `setProb` does). Ensure the `gate_rv` constructor goes through a path that *does* create the gate before setting `extra` on it. Likely pattern: emit a `setProb` (or equivalent gate-creation IPC) first, then `setExtra` for the distribution parameters.
- **Monte Carlo seeding contract**. The `provsql.monte_carlo_seed` GUC must be respected by all samplers (Bernoulli + continuous + boundcheck negative tests) so that regression tests are deterministic with large `n`.
- **Compatibility floor bump** on Studio happens in lockstep with the extension version that ships these gate types; CHANGELOG entries are written by the maintainer at release time per CLAUDE memory.
- **Out-of-scope follow-ups** to leave clearly TODO'd at the bottom of `doc/source/user/continuous-distributions.rst`: EXCEPT, DISTINCT, aggregation over RVs, where-provenance × RVs, in-Studio distribution editor.
