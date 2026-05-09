# Documentation TODO: continuous probability distributions

A plan for adding **probabilistic c-tables over continuous random
variables** (Gaussian, uniform, exponential, ...) to ProvSQL. Anchored
on a 2022 BSc thesis by Timothy Leong (NUS), supervised by Pierre
Senellart, Stéphane Bressan, and Frank Stephan, plus a companion
implementation-notes document by the same author. The thesis lays the
measure-theoretic foundations (lifting Borel sets in ℝᵏ to instances
via a bijection), defines a relational query language (σ, π, ×, ∪, −,
distinct, p̄) on pc-tables with selection predicates that are arbitrary
∨/∧ of inequalities over arithmetic expressions of random variables and
constants, proves intensional / possible-world equivalence, and
specifies four core algorithms: **RangeCheck** (interval support of an
RV via DAG recursion), **BoundCheck** (MILP-based satisfiability of a
tuple's condition), **SampleOne** (memoised rejection sampling),
**EvaluateTupleProbability** (Monte Carlo). A prototype Postgres
extension was written but never integrated; no trace of it remains in
ProvSQL today.

The intended outcome of this plan: a research prototype on a feature
branch that brings continuous-distribution support back, integrated
cleanly into the existing circuit/gate model rather than as a parallel
system.

## Out of scope

The following are deliberately deferred to a follow-up plan once the
core lands:

- **EXCEPT and DISTINCT.** The thesis rewrites these into Cartesian
  product + GROUP BY + AGG_AND. Doable but invasive in the planner
  hook; defer.
- **Aggregation over RVs.** Open theoretical question: how does
  `SUM` over a column of normals interact with the existing
  aggregation semirings? Out of scope here.
- **Studio UI for RV inspection.** Useful but not needed for the
  prototype: leave as a follow-up under `studio/`.
- **Where-provenance + RVs.** The `where_provenance` mode already adds
  `eq` / `project` gates; verify with a regression test that adding
  RV-children to `gate_cmp` does not interfere, but no design work is
  scheduled.

## Plan

### Data model and new gate types

Append to `gate_type` (src/provsql_utils.h:55-73) and to
`provenance_gate` (sql/provsql.common.sql:27-44). On-disk ABI requires
that new gate types are added at the end, before `gate_invalid`.

| Gate              | Children       | Stored data                                | Purpose                       |
|-------------------|----------------|--------------------------------------------|-------------------------------|
| `gate_rv_input`   | (none)         | `extra` = `"normal:μ,σ"` / `"uniform:a,b"` | Continuous RV leaf            |
| `gate_rv_const`   | (none)         | `prob` = literal value                     | Zero-variance RV (constant)   |
| `gate_rv_plus`    | n RV children  | none                                       | Sum of RV expressions         |
| `gate_rv_times`   | n RV children  | none                                       | Product of RV expressions     |
| `gate_rv_minus`   | 2 RV children  | none                                       | Subtraction                   |
| `gate_rv_neg`     | 1 RV child     | none                                       | Negation                      |

Six new gates total. All append-only, all using fields that already
exist in `GateInformation` (src/MMappedCircuit.h:59-79).

**No new comparison gate is needed.** Generalise the existing
`gate_cmp` so its children may be RV expressions in addition to the
current `agg` / `value` pair. The Monte Carlo evaluator learns one
new branch: if a child is an RV expression, sample it; otherwise fall
back to the existing aggregate-vs-constant logic. The HAVING-clause
path (src/having_semantics.cpp) keeps working unchanged. The stale
"Currently unused" comment on `gate_cmp` in src/provsql_utils.h:66
and sql/provsql.common.sql:39 has already been refreshed; the matching
line in sql/fixtures/provsql--1.0.0-common.sql:39 is intentionally
left alone (the fixture is a frozen snapshot of 1.0.0, where `cmp`
truly was unused; `provenance_cmp` was added in the 1.1.0 → 1.2.0
upgrade per sql/upgrades/provsql--1.1.0--1.2.0.sql:48).

### SQL surface

Introduce a composite SQL type `random_variable` (UUID-backed,
mirroring how `agg_token` is exposed today). Constructors and
overloaded operators let user queries read like ordinary SQL:

```sql
CREATE TABLE sensors(id text, reading provsql.random_variable);

INSERT INTO sensors VALUES
  ('s1', provsql.normal(2.5, 0.5)),
  ('s2', provsql.uniform(1, 3)),
  ('s3', provsql.exponential(0.7));

-- selection: planner rewrites > into a comparison gate
SELECT id, reading
  FROM sensors
 WHERE reading > 2;

-- the result has a provsql column carrying the Boolean formula
-- "reading_RV > 2" for each surviving tuple
SELECT id, probability_evaluate(provsql, 'monte-carlo', 10000) AS p
  FROM (SELECT id, reading FROM sensors WHERE reading > 2) t;

-- arithmetic on RVs: sum of two readings
SELECT s1.reading + s2.reading AS combined
  FROM sensors s1, sensors s2 WHERE s1.id = 's1' AND s2.id = 's2';
```

Internals:

- A `random_variable` value is a row `(token uuid, value float8)`.
  The UUID indexes a gate in the circuit (input or composite). The
  value field caches a deterministic literal (e.g. for constants
  converted to zero-variance RVs).
- Constructor functions register a fresh `gate_rv_input` with the
  distribution serialised into `extra` (e.g. `"normal:2.5,0.5"`),
  and return a `random_variable` carrying that UUID.
- Operator overloads `+`, `-`, `*`, `/` on `random_variable ×
  random_variable` (and `random_variable × numeric`) build composite
  RV gates and return a new `random_variable`.
- Operators `<`, `<=`, `=`, `>=`, `>`, `<>` between
  `random_variable`s (or `random_variable × numeric`) return a
  `uuid` (a Boolean token), which the planner-hook rewriter conjoins
  into the tuple's provsql column instead of feeding it to
  PostgreSQL's WHERE filter.

This addresses the only awkward bit: a regular `WHERE reading > 2`
would normally be evaluated by PostgreSQL and filter rows out. The
planner hook recognises comparisons against `random_variable`
columns, routes them into the provsql token, and defaults `WHERE` to
`TRUE` so all rows survive (subject to optional BoundCheck pruning).
This is exactly the spirit of ProvSQL's existing rewriting.

### Planner-hook changes (src/provsql.c)

- **Discovery.** Extend `get_provenance_attributes()` (lines
  407-515) to recognise both `provsql UUID` columns and
  `random_variable` columns.
- **Expression building.** When a comparison node is detected
  anywhere in the WHERE/SELECT tree (Var of `random_variable`
  against a constant or another `random_variable`), translate it
  into a `gate_cmp` in the circuit (reusing `provenance_cmp`,
  sql/provsql.common.sql:586-611) and conjoin the resulting Boolean
  token into the tuple's provsql column via `provenance_times`. RV
  arithmetic in SELECT expressions creates `gate_rv_*` gates.
- **Splice.** Unchanged for the common case. WHERE clauses that
  the rewriter consumes are replaced by `TRUE` (or by
  `BoundCheck(provsql)` when the optional solver is enabled).

### Sampler: extend `BooleanCircuit::monteCarlo`

In src/BooleanCircuit.cpp:287-307. Add a per-call
`unordered_map<uuid, double>` cache that:

- on encountering an `rv_*` gate, recursively samples / combines
  children to produce a scalar;
- on encountering a `gate_cmp` whose children are RV expressions,
  samples both operand sub-DAGs once each (memoised by UUID, per the
  thesis's SampleOne) and returns the Boolean comparator value;
- leaves the existing aggregate-vs-constant `gate_cmp` branch
  unchanged;
- keeps `plus` (OR), `times` (AND), `monus` (AND-NOT) for the
  Boolean part of the formula.

Switch from `rand()` to C++ `<random>` (`std::mt19937_64`,
`std::normal_distribution`, `std::uniform_real_distribution`,
`std::exponential_distribution`). The Bernoulli path inherits the
same RNG as a small adjacent improvement. Add a
`provsql.monte_carlo_seed` GUC for deterministic regression tests.

### Optional BoundCheck (`lp_solve`)

Following the convention of `d4` / `c2d` / `weightmc` (resolved
through `provsql.tool_search_path`):

- Detect `lp_solve` / `lpsolve55` at configure time
  (Makefile.internal); guard with `#ifdef HAVE_LPSOLVE`.
- A new C++ helper `boundCheck(token) -> bool` walks the Boolean
  part of the circuit, lowers each `gate_cmp` clause whose children
  are RV expressions into an LP constraint over base RVs, encodes
  disjoint support intervals via binary indicator variables (per the
  thesis), and asks the solver for feasibility.
- Wired in as a planner-hook post-pass on the rewritten WHERE: if
  BoundCheck returns false for a tuple class, skip the whole
  branch.
- Runtime GUC `provsql.use_bound_check = on/off`, defaulting to
  on if `lp_solve` was detected at build time.

When `lp_solve` is unavailable, queries still work; selectivity is
just looser (always-false predicates produce zero-probability rows
that get filtered post-MC by a threshold).

### Files

Existing files to modify:

- `src/provsql_utils.h` (gate_type enum: append 6 new tags before
  `gate_invalid`; bump `gate_type_name[]` accordingly).
- `sql/provsql.common.sql` (new type, constructors, operators,
  helper functions; bump enum).
- `src/provsql.c` (planner hook: recognise `random_variable`
  columns and inequality predicates; splice into provsql).
- `src/BooleanCircuit.cpp`/`.h` (extend monteCarlo;
  per-sample memoisation map; switch to `<random>`).
- `src/CircuitFromMMap.cpp`/`.hpp` (read new gate types into
  the in-memory circuit).
- `src/probability_evaluate.cpp` (no API change; routes through
  the extended sampler).
- `Makefile.internal` (optional `HAVE_LPSOLVE` probe; link
  `-llpsolve55` when found).
- `test/schedule.common` (add new tests; never edit
  `test/schedule` directly: it is generated).
- `doc/source/user/probabilities.rst` and a new
  `doc/source/user/continuous-distributions.rst` (user manual);
  `doc/source/dev/probability-evaluation.rst` (architecture).

New files:

- `src/RandomVariable.cpp`/`.h` (C++ helpers for distribution
  sampling, registry lookup).
- `src/random_variable_type.c` (PostgreSQL type IO functions for
  `random_variable`).
- `src/BoundCheck.cpp`/`.h` (optional, gated by `HAVE_LPSOLVE`).
- `test/sql/continuous_basic.sql`,
  `continuous_arithmetic.sql`, `continuous_selection.sql`,
  `continuous_join.sql`, `continuous_union.sql`, plus
  `expected/*.out`.

## Priorities

Each stage ends with green `make installcheck` and a small demo SQL
script.

1. **Foundations: type and constructors.** Add `random_variable`
   composite type, IO functions, and constructors `normal /
   uniform / exponential / as_random` that create `gate_rv_input` /
   `gate_rv_const` gates with the distribution serialised into
   `extra`. Append the 6 new gate-type enumerators (in C, in SQL,
   in `gate_type_name[]`). No planner-hook changes yet;
   constructors and gates round-trip through `MMappedCircuit` and
   survive a restart.

2. **Sampler: continuous Monte Carlo.** Extend the in-memory
   circuit to load and recognise the new gate types. Wire
   `<random>`-based sampling for `gate_rv_input` per
   distribution; memoise samples by UUID per tuple-MC iteration.
   Extend `gate_cmp` evaluation: when its children are RV
   expressions, compare two scalar samples; the existing
   aggregate-vs-constant branch stays. First regression tests at
   this stage are circuit-level (no SQL rewriting yet).

3. **Operator overloading: arithmetic and inequality.** SQL
   operators `+ - * /` on `random_variable × random_variable` and
   `random_variable × numeric` (build `gate_rv_*` gates, return
   `random_variable`). SQL comparison operators (returning a
   Boolean token UUID). SQL-level tests that hand-write the
   `provsql` column.

4. **Planner hook: SELECT, WHERE, JOIN, UNION on pc-tables.**
   Detect `random_variable` columns in `get_provenance_attributes`.
   Detect comparisons against `random_variable` in WHERE (and
   SELECT list); rewrite them into Boolean tokens conjoined into
   provsql; replace those WHERE clauses with `TRUE`. Cartesian
   product / JOIN: conjoin both tuples' provsql tokens (already what
   `SR_TIMES` does). UNION ALL: project provsql column straight
   through (already what `SR_PLUS` does). The sensors example
   above runs end-to-end.

5. **Optional pruning: BoundCheck.** Behind `--with-lpsolve`
   (autodetected): build `BoundCheck.cpp`; planner hook wraps the
   rewritten WHERE in a BoundCheck call. GUC
   `provsql.use_bound_check`. One regression test that exercises
   an unsatisfiable predicate and verifies the row is pruned.

6. **Polish.** Documentation:
   `doc/source/user/continuous-distributions.rst` (tutorial),
   `doc/source/dev/probability-evaluation.rst` (arch update).
   Run `make docs`. Demo script in `studio/scripts/` if the Studio
   UI handles the new gate types as opaque tokens (likely yes; it
   already labels gate types it does not specially render). Leave
   `EXCEPT`, `DISTINCT`, `GROUP BY`, and HAVING with RV operands
   as a clearly-marked TODO.

## Implementation observations

### What ProvSQL already provides

The thesis's "DAG over UUIDs" *is* a ProvSQL circuit, just with a
wider gate vocabulary. Concretely:

- **Gate vocabulary** (15 gate types in src/provsql_utils.h:55-73):
  `input`, `plus`, `times`, `monus`, `project`, `zero`, `one`,
  `eq`, `agg`, `semimod`, `cmp`, `delta`, `value`, `mulinput`,
  `update`. New gate types must be appended at the end before
  `gate_invalid`.
- **Persistent storage** (src/MMappedCircuit.h:59-79): each
  gate's `GateInformation` carries `type`, `nb_children`, children
  index, `prob` (double, default 1.0), `info1`/`info2`
  (unsigned), and a variable-length `extra` byte string. The
  16-byte file header (magic + version) already exists, so
  distribution parameters can live in `extra` without breaking the
  on-disk layout.
- **Lazy materialisation** (src/MMappedCircuit.cpp:129-139):
  `setProb` / `set_infos` / `setExtra` lazily create an `input`
  gate if the UUID is unknown. The same pattern fits a future
  `set_distribution`.
- **Boolean circuit + Monte Carlo**
  (src/BooleanCircuit.cpp:287-307): traverses the gate DAG and
  samples each `input` independently from Bernoulli with `rand()`;
  `plus`=OR, `times`=AND. Exactly the structure needed for
  sampling-based evaluation; only the leaf-sampling step is too
  narrow.
- **Three-phase planner hook** in src/provsql.c (~3650 lines):
  discovery, expression building (`make_provenance_expression`,
  lines 1101-1392), splice. SR_PLUS=UNION, SR_TIMES=JOIN,
  SR_MONUS=EXCEPT already wire SQL set ops onto circuit gates.
  HAVING is pre-evaluated by `having_semantics`. Per-tuple metadata
  is carried as a single `provsql UUID` column; this contract is
  preserved.
- **`add_provenance(regclass)`** (sql/provsql.common.sql:215-221)
  adds the UUID column and is the natural place to extend for
  continuous RVs.

### What is missing today

- **No continuous RV inputs.** Leaves are Bernoulli only.
- **No arithmetic on RVs.** No gate type representing `x+y`,
  `x-y`, `x*c`, etc., between two scalar RVs.
- **No inequality predicates between RV expressions.** `gate_cmp`
  is used today to compare an aggregate-value gate against a
  constant-value gate (HAVING clauses), not two RV expressions.
- **No solver, no continuous RNG.** No `<random>`, no
  `boost::math`, no LP/MILP solver. WeightMC is the only
  sampling-adjacent external tool.

### Verification

- `make -j$(nproc)` clean (no new warnings).
- `sudo make install && sudo service postgresql restart && make
  installcheck`. Regressions in `test/sql/continuous_*.sql`
  against hand-computed expected output, with Monte Carlo
  determinism via `provsql.monte_carlo_seed` and large `n`.
- Manual end-to-end demo:

  ```sql
  CREATE TABLE sensors(id text, reading provsql.random_variable);
  INSERT INTO sensors VALUES
    ('s1', provsql.normal(2.5, 0.5)),
    ('s2', provsql.uniform(1, 3));
  SELECT add_provenance('sensors');
  SELECT id, probability_evaluate(provsql, 'monte-carlo', 10000)
    FROM (SELECT id, reading FROM sensors WHERE reading > 2) t;
  -- expected: s1 ~ 0.84, s2 ~ 0.50, within sampling error
  ```

- Studio (`provsql-studio`): point at the demo DB; the circuit
  panel renders, even if new gate types show up as plain labels.

### Compatibility floor

Studio's compatibility table (extension >= 1.4.0 today) will need
a bump when the new gate types ship, in coordination with the
1.4.0 / Studio 1.0.0 release window.
