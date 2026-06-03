# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ProvSQL is a PostgreSQL extension that adds **(m-)semiring provenance** and **uncertainty management** to PostgreSQL. It transparently rewrites SQL queries to track provenance as circuit tokens (UUIDs), enabling computation of probabilities, Shapley/Banzhaf values, and a wide range of semiring evaluations (Boolean, counting, why/how/which-provenance, formula, tropical, Viterbi, Łukasiewicz, min-max/max-min, interval-union, …).

PostgreSQL integration requires `shared_preload_libraries = 'provsql'` in `postgresql.conf` because the extension installs a **planner hook** for transparent query rewriting and launches a background worker for the persistent circuit store. (The single-process WASM/browser build — see *WASM / Browser Build* below — is the exception: with no background workers it installs the hook at `CREATE EXTENSION` and needs no preload.)

The repository contains several components beyond the extension itself:

- `src/`, `sql/`, `test/`, `doc/` — the PostgreSQL extension and its docs.
- `studio/` — **ProvSQL Studio**: a Python/Flask web UI for provenance inspection, circuit visualisation, and on-the-fly semiring evaluation. Distributed on PyPI as `provsql-studio`. `studio/web/` is the **ProvSQL Playground**: the same Studio Python running entirely in the browser over PostgreSQL+ProvSQL compiled to WASM (PGlite + Pyodide).
- `wasm/` — reproducible Emscripten build of ProvSQL as a PGlite-loadable WASM extension (build scripts, libc++ patch, headless Node smoke test) — see *WASM / Browser Build* below.
- `where_panel/` — older PHP web app for where-provenance display (legacy, kept for compatibility).
- `website/` — Jekyll source for `https://provsql.org/` (consumes generated docs + branding).
- `branding/` — logo, favicon, and font assets shared by the docs and website.
- `docker/` — Dockerfile that builds an image with provsql plus optional external tools; `docker/README.md` is the Docker Hub overview, mirrored to the registry on every image push.

## Build Commands

```bash
# Build the extension (LLVM JIT is force-disabled via with_llvm=no).
# Always pass `-j$(nproc)` for parallel compilation -- ProvSQL's PGXS
# build is parallel-safe and the local round-trip is noticeably faster.
make -j$(nproc)

# Build with debug symbols
make DEBUG=1

# Build the standalone knowledge compiler tool
make tdkc

# Build the standalone mmap migration tool (requires libpq-dev)
make provsql_migrate_mmap

# Install to PostgreSQL extensions directory (needs root)
sudo make install

# Build the Sphinx user/dev documentation (HTML in doc/source/_build/html)
make docs

# Build the full Jekyll website (also runs `make docs` first)
make website

# Build the Docker image
make docker-build

# Build the WASM artifacts (PGlite core + ProvSQL extension) against the
# Emscripten builder image and assemble the Playground -- reproduces the
# `wasm` CI job locally (needs podman/docker + Node/corepack + Boost headers;
# heavy). Backed by wasm/build-wasm.sh.
make wasm
# Assemble the in-browser build (ProvSQL Playground) doc-root from the
# canonical Studio sources + the wasm/ artifacts; reuses in-place
# pglite/ and provsql.tar.gz unless PGLITE_DIST=/PROVSQL_TARGZ= are given
make playground
# Browser e2e for the assembled Playground (headless Chromium / Playwright)
make playground-test
# Build then rsync the Playground to provsql.org/playground/
make deploy-playground

# Force the single-process in-memory store on a native build (the
# configuration the WASM target uses; for testing the in-process path)
make CPPFLAGS=-DPROVSQL_INPROCESS_STORE
```

The build uses PostgreSQL's PGXS system via `Makefile.internal`. LLVM JIT is explicitly disabled (`with_llvm=no`) due to known PostgreSQL bugs. Requires C++17 and Boost (`libboost-dev`, `libboost-serialization-dev`). The `docs` target requires Sphinx + the project theme; the `website` target additionally requires Doxygen, Ruby, and Bundler/Jekyll. The actual WASM artifacts are produced by the scripts under `wasm/` against an Emscripten toolchain (podman/Docker-gated); `make wasm` (→ `wasm/build-wasm.sh`) reproduces the `wasm` CI job's build locally, and `make playground-test` runs its browser e2e.

## Testing

```bash
# Run full regression test suite (requires PostgreSQL superuser)
make test
# or equivalently:
make installcheck

# Run with a specific PostgreSQL port
make installcheck EXTRA_REGRESS_OPTS="--port=5434"
```

Tests are **integration tests** against a live PostgreSQL instance using `pg_regress`. There are ~106 SQL test files in `test/sql/` with expected output in `test/expected/`. Tests skip optional external tools (c2d, d4, dsharp, minic2d, weightmc, graph-easy) if unavailable. Test diffs appear in `/tmp/tmp.provsqlXXXX/regression.diffs`; `make test` will pipe them through `$PAGER` automatically on failure.

`test/schedule` is **generated** from `test/schedule.common` (plus version-specific fragments) — add new pg_regress tests to `schedule.common`, never to `schedule`.

To run a single test, you can invoke pg_regress manually with the specific test name from the schedule.

`.github/workflows/wasm.yml` is a single cheap per-PR **in-process-store smoke** (extension built with `-DPROVSQL_INPROCESS_STORE`, one psql session exercising provenance rewriting + probability — *not* the full multi-backend `pg_regress`, which needs preload and is incompatible with the single-process store). The full matched PGlite+extension build, the headless Node smoke test (`wasm/test-node.mjs`), and the browser e2e (`studio/tests/web/`, pytest-playwright on headless Chromium) are reproduced **locally** with `make wasm` / `make playground-test` (`wasm/build-wasm.sh`), not in CI — that build is heavy and was only ever opt-in (`workflow_dispatch`, never auto-triggered), so the local script is the single source of truth. The web e2e is deliberately excluded from `make studio-test`; run it with `cd studio && pytest tests/web`.

## Architecture

### Query Rewriting (Core)

`src/provsql.c` (~3650 lines) is the main entry point. It installs a PostgreSQL planner hook (`prev_planner`) that performs three-phase transparent rewriting:

1. **Discovery** — identify relations with provenance columns
2. **Expression building** — combine provenance tokens using semiring operations
3. **Splice** — append the provenance expression to the SELECT list

Each table with provenance tracking has a special `provsql` UUID column. Query results gain a UUID representing the provenance circuit for that tuple.

`_PG_init` registers user-facing GUC variables: `provsql.active`, `provsql.where_provenance`, `provsql.update_provenance`, `provsql.verbose_level`, `provsql.aggtoken_text_as_uuid`, `provsql.boolean_provenance` (umbrella GUC gating every Boolean-only optimisation: the safe-query / read-once rewriter, the recursive-CTE fixpoint lowering, …), `provsql.tool_search_path` (colon-separated directories prepended to `PATH` when invoking external tools; **`PGC_SUSET`/superuser-only** since it dictates where the server OS user looks for executables — a non-superuser setting it would be arbitrary code execution as the server account), `provsql.fallback_compiler` (compiler used by `makeDD`'s final fallback after `interpretAsDD` and tree-decomposition both fail; default `"d4"`), `provsql.kcmcp_server` (launch command, with a `{endpoint}` placeholder, for the managed KCMCP knowledge-compiler server; `PGC_SIGHUP`, empty = none; the `kcmcp_supervisor.c` background worker forks/supervises it and publishes its endpoint in shared memory for the in-extension client), `provsql.monte_carlo_seed` (seed for the `std::mt19937_64` shared by the Bernoulli and continuous sampling paths; `-1` = non-deterministic), `provsql.rv_mc_samples` (sample budget for the analytical-evaluator MC fallback; `0` turns the fallback into an exception), `provsql.simplify_on_load` (universal peephole pass at circuit-load time), and `provsql.classify_top_level` (read-only query-time classifier in `src/classify_query.c`: emits a `NOTICE` labelling each top-level SELECT as TID / BID / OPAQUE with its provenance-tracked source relations; Studio reads the NOTICE to render the schema-panel TID/BID pills). The debug-only `provsql.hybrid_evaluation` is also registered with `GUC_NO_SHOW_ALL`.

### Circuit Representation

Provenance is stored as a DAG of **gates**:

- `input` — variable (leaf) gates
- `plus` / `times` / `monus` — semiring operations
- `project` / `eq` — where-provenance (projection, equijoin)
- `agg` / `semimod` — aggregation
- `zero` / `one` — semiring identity elements
- `value` / `mulinput` — scalar/multivalued constants (`value` has an integer mode used in HAVING and a float8 mode used in the continuous-RV surface)
- `delta` — δ-semiring operator
- `cmp` — comparison gate (HAVING predicates and the planner-hook lift of WHERE comparators on `random_variable` columns)
- `update` — update-provenance gate (data-modification tracking)
- `rv` — continuous random-variable leaf (Normal / Uniform / Exponential / Erlang; distribution encoded in `extra`)
- `arith` — N-ary arithmetic over scalar children (operator tag in `info1`: PLUS=0 / TIMES=1 / MINUS=2 / DIV=3 / NEG=4)
- `mixture` — Bernoulli or categorical mixture of scalar random-variable roots

The gate-type enum is **append-only** (persisted on disk; reordering would invalidate every existing circuit). Same for `provsql_arith_op`.

Key circuit classes:

- `src/Circuit.h` / `src/Circuit.hpp` — template base parameterised by gate type.
- `src/GenericCircuit.{h,hpp,cpp}` — semiring-agnostic in-memory circuit.
- `src/BooleanCircuit.{h,cpp}` — Boolean circuit for knowledge compilation / probability.
- `src/WhereCircuit.{h,cpp}` — where-provenance circuit.
- `src/DotCircuit.{h,cpp}` — GraphViz DOT export.
- `src/MMappedCircuit.{h,cpp}` — mmap-backed persistent store.
- `src/CircuitFromMMap.{h,cpp}` — reads the mmap store into a `GenericCircuit` / `BooleanCircuit`.
- `src/CircuitCache.{h,cpp}` / `src/circuit_cache.h` — per-session gate cache.

### Semiring Implementations

`src/semiring/` contains header-only semiring definitions consumed by the compiled-evaluation path:

- `Semiring.h` (base interface), `Boolean.h`, `BoolExpr.h`, `Counting.h`, `Formula.h`, `How.h`, `IntervalUnion.h`, `Lukasiewicz.h`, `MinMax.h`, `Tropical.h`, `Viterbi.h`, `Which.h`, `Why.h`.

`src/Expectation.{h,cpp}` is the analytical moment evaluator for continuous-RV circuits (carrier `double`). It is *not* a `Semiring` subclass: the `provenance_evaluate_compiled_internal` dispatcher special-cases `semiring == "expectation"` and calls `provsql::compute_expectation` directly on the `GenericCircuit`, bypassing the `evaluate<S>` template path used by the proper semirings.

`src/provenance_evaluate_compiled.{cpp,hpp}` is the dispatcher; `src/having_semantics.{cpp,hpp}` pre-evaluates `HAVING` sub-circuits before the main traversal (and exposes `extract_constant_double` for the float8 mode of `gate_value`). SQL bindings live in `sql/provsql.common.sql` as `sr_*` functions (`sr_boolean`, `sr_boolexpr`, `sr_counting`, `sr_formula`, `sr_how`, `sr_which`, `sr_why`, `sr_tropical`, `sr_viterbi`, `sr_lukasiewicz`, `sr_minmax`, `sr_maxmin`, plus the temporal/interval ones in `provsql.14.sql`).

### Probability, Shapley, Knowledge Compilation

- `src/probability_evaluate.cpp` — probability-method dispatcher.
- `src/TreeDecomposition.{h,cpp}` — min-fill tree decomposition.
- `src/dDNNF.{h,cpp}` — d-DNNF data structure and linear-time probability evaluation.
- `src/dDNNFTreeDecompositionBuilder.{h,cpp}` — d-DNNF construction from a tree decomposition.
- `src/TreeDecompositionKnowledgeCompiler.cpp` — standalone `tdkc` tool.
- `src/shapley.cpp` — Shapley and Banzhaf values.

External tools optionally supported (resolved through `provsql.tool_search_path`): `d4`, `c2d`, `minic2d`, `dsharp`, `weightmc`, `graph-easy`.

A compile tool can instead be a **warm KCMCP server** reached over a socket (registry `kind = 'kcmcp'`; see `doc/source/dev/kc-server-protocol.rst`): `compilation()` routes such a tool to the in-extension client `src/kcmcp_client.cpp` (one connection per backend, keyed by endpoint, with reconnect-once-on-stale and `on_proc_exit` cleanup), which sends a `compile` REQUEST and parses the `ddnnf-nnf` RESULT back through the same `parseDDNNF` as the CLI path, falling back to CLI on any failure. The record's `endpoint` is a fixed `unix:`/`host:port` address (endpoint mode), or the literal `managed`, resolved from shared memory to the server that the `src/kcmcp_supervisor.c` background worker launches/supervises per `provsql.kcmcp_server`. The shared KCMCP wire codec is `src/kcmcp_protocol.{h,cpp}` (compiled into both the extension and `tdkc`); `src/kcmcp_server.cpp` + `src/dimacs_cnf.cpp` are the `tdkc --kcmcp` reference server (tdkc-only). The protocol conformance test is `test/kcmcp/conformance.py` (`make kcmcp-tdkc-test`); the client's pg_regress coverage is `test/sql/kcmcp_client_{endpoint,managed}.sql`, run with a server via `test/kcmcp/with-tdkc.sh` (wired into `make test` and CI).

### Continuous Distributions

`random_variable` columns and the hybrid analytic + Monte Carlo evaluator live in:

- `src/RandomVariable.{h,cpp}` and `src/random_variable_type.c` — thin UUID-wrapper type (binary-coercible with `uuid`) and parsers for `gate_rv`'s `extra` blob.
- `src/MonteCarloSampler.{h,cpp}` — `std::mt19937_64` seeded from `provsql.monte_carlo_seed`, per-iteration RV and Boolean caches, `gate_agg` arm that unlocks HAVING+RV under MC, `gate_delta` transparency in the rv_* event walker.
- `src/RangeCheck.{h,cpp}` — support-interval propagation through `gate_arith` + per-conjunct decision on `gate_cmp` (joint AND-conjunction pass via `walkAndConjunctIntervals`). Backs `provsql.simplify_on_load`.
- `src/AnalyticEvaluator.{h,cpp}` — closed-form CDF for single-distribution `gate_cmp` (`std::erf` for Normal, log1p / expm1 for Exponential, regularised lower incomplete gamma for Erlang).
- `src/Expectation.{h,cpp}` — analytical moments per distribution, structural-independence detection via `FootprintCache`, MC fallback at the `provsql.rv_mc_samples` budget.
- `src/HybridEvaluator.{h,cpp}` — peephole simplifier (normal-family closure, i.i.d. Erlang sum, identity collapse), island decomposition over base-RV footprints, monotone-shared-scalar fast path. Debug-gated by `provsql.hybrid_evaluation`.
- `getJointCircuit` (in `src/MMappedCircuit.{h,cpp}`) — multi-rooted BFS so shared `gate_rv` leaves between an input scalar and its conditioning event couple correctly in the MC sampler's `rv_cache_`.

The planner-hook classifier (`migrate_probabilistic_quals` in `src/provsql.c`, `qual_class` enum: `QUAL_PURE_AGG` / `QUAL_PURE_RV` / `QUAL_DETERMINISTIC` plus mixed-error classes) routes every WHERE/HAVING qual to its rewrite. RV-returning aggregates dispatch on `aggtype` (not `aggfnoid`) through `make_rv_aggregate_expression`, wrapping per-row arguments in `rv_aggregate_semimod` so the FFUNC builds a single `gate_arith` root over per-row `gate_mixture` children.

### Memory Management

- `src/provsql_mmap.{c,h}` — file-backed mmap storage and IPC.
- `src/provsql_shmem.{c,h}` — shared-memory segment setup.
- `src/MMappedUUIDHashTable.{h,cpp}` — UUID-keyed open-addressing hash table over mmap.
- `src/MMappedVector.{h,hpp}` — `std::vector`-like interface over mmap.

**Per-database file layout**: each database's circuit is stored in four files under `$PGDATA/base/<db_oid>/`: `provsql_gates.mmap`, `provsql_wires.mmap`, `provsql_mapping.mmap`, `provsql_extra.mmap`. Files are created lazily on the first IPC message for that database and removed automatically when the database is dropped.

**File format header**: every mmap file begins with a 16-byte header — `uint64_t magic | uint16_t version | uint16_t elem_size | uint32_t _reserved` — validated on open to detect type mismatches and format version changes.

**IPC protocol**: every message from a backend to the background worker is prefixed with the opcode byte followed by `MyDatabaseId` (`Oid`), so the worker dispatches to the correct per-database `MMappedCircuit` instance. The worker maintains a `std::map<Oid, MMappedCircuit*>` opened lazily.

**Migration tool**: `src/provsql_migrate_mmap.cpp` (built as `provsql_migrate_mmap`) migrates old flat `$PGDATA/provsql_*.mmap` files (pre-1.3.0 format, no header) to the new per-database layout. Run as the `postgres` user before upgrading to 1.3.0; the tool deletes the old flat files on success.

**Storage abstraction**: `src/MappedRegion.h` abstracts the backing of each mmap region. In the default multi-process build it is a `MAP_SHARED` mmap kept coherent across backends and the worker by the kernel; under `PROVSQL_INPROCESS_STORE` it is a heap buffer loaded on `map()` and written back explicitly on `sync()`/`close()` (Emscripten has no usable shared mmap). See *WASM / Browser Build* for the build flags.

### WASM / Browser Build

ProvSQL compiles to WebAssembly to run client-side inside PGlite (PostgreSQL→WASM), powering the ProvSQL Playground. Two build-configuration switches in `src/provsql_config.h` gate this, both auto-enabled under `__EMSCRIPTEN__`:

- **`PROVSQL_INPROCESS_STORE`** — single-process circuit store: the background worker, shared-memory segment, LWLock, and inter-process pipes are replaced by an in-memory request/response FIFO with synchronous in-process dispatch (`src/MappedRegion.h` heap-backs the regions; the KCMCP supervisor `src/kcmcp_supervisor.c` and its registration compile out; the Boost-serialized circuit round-trip is dropped in favour of building the `GenericCircuit` directly from this process's store, removing the libboost-serialization dependency; the planner hook installs at `CREATE EXTENSION`). Can be forced on a native build (`make CPPFLAGS=-DPROVSQL_INPROCESS_STORE`) to exercise the in-process path; all native tests pass serially under it.
- **`PROVSQL_NO_SUBPROCESS`** — no fork/exec and no sockets: the external knowledge-compiler CLIs, the KCMCP socket client (`src/kcmcp_client.cpp`), and `run_in_own_pgroup` in `src/external_tool.cpp` compile out; probability falls back to the in-process tree-decomposition compiler and Monte Carlo. Tied to the platform (`__EMSCRIPTEN__`) rather than to `PROVSQL_INPROCESS_STORE`, so a native in-process-store test build keeps the subprocess/socket paths and stays a faithful regression baseline.

The reproducible build lives in `wasm/`: the upstream `postgres-pglite/build-pglite.sh` builds the pinned PostgreSQL-pglite (PG17.5) WASM core, `build-extension.sh` compiles the extension against it, and `relink-pglite.sh` (deliberately *not* named `build-pglite.sh`, so copying it into the tree does not clobber the upstream core builder) re-links `pglite.wasm` to export ProvSQL's symbols (`MAIN_MODULE=2`), `patches/0001-cxx-inline-libcxx.patch` fixes a libc++ inlining bug in the PGlite Postgres tree that otherwise blocks any C++ extension, and `test-node.mjs` is the headless smoke test. The matched `pglite.wasm`/`.data`/`.js` + `provsql.tar.gz` pair is what the browser loads. See `wasm/README.md` and `doc/source/dev/playground.rst`.

### SQL API

`sql/provsql.common.sql` defines the SQL-level API:

- `add_provenance(regclass)` — add provenance tracking to a table.
- `create_gate(token, type, children)` — manually create circuit gates.
- `get_gate_type(token)`, `get_children(token)` — query gate structure.
- `set_prob(token, p)`, `get_prob(token)` — probability management.
- `create_provenance_mapping(name, table, column)` — gate labeling.
- `provenance_evaluate(...)` (SQL-level UDF dispatch) and
  `provenance_evaluate_compiled(...)` (C++ dispatcher) — semiring evaluation.
- `sr_*` family — compiled semiring evaluators (see above).
- `where_provenance(...)`, `view_circuit(...)`, `to_provxml(...)` — visualisation/export.
- Probability methods: `probability_evaluate`, `monte_carlo`, `independent`, `possible_worlds`, `repair_key`, …
- Shapley/Banzhaf: `shapley`, `banzhaf`.
- Continuous random variables: composite type `random_variable`; constructors `provsql.normal`, `provsql.uniform`, `provsql.exponential`, `provsql.erlang`, `provsql.categorical`, `provsql.mixture` (two overloads), `provsql.as_random` (with implicit casts from `integer` / `numeric` / `double precision`). Operators `+ - * / -` and `< <= = <> >= >` on `(random_variable, random_variable)` (the comparison operators are intercepted at planning time and rewritten into `gate_cmp`). Aggregates `sum` / `avg` / `product` over `random_variable` lower to `gate_arith` of `gate_mixture` children via `rv_aggregate_semimod` (each with an `INITCOND = '{}'` and a per-aggregate empty-group identity).
- Polymorphic moment / support / sample / histogram surface: `expected`, `variance`, `moment`, `central_moment`, `support` (dispatchers over `random_variable` / `agg_token` / plain numeric, with an optional `prov uuid DEFAULT gate_one()` conditioning argument); `rv_sample(token, n, prov)` SRF over `float8`; `rv_histogram(token, bins, prov)` returning `jsonb`. Circuit introspection helper `simplified_circuit_subgraph` exposes the in-memory peephole-folded subgraph.

Version-specific additions in `sql/provsql.14.sql` (PostgreSQL 14+: data-modification tracking, temporal validity ranges via `sr_temporal`, `sr_interval_int`, `sr_interval_num`).

Cross-version upgrade scripts live under `sql/upgrades/` (`provsql--1.X.Y--1.X.Z.sql`).

### Aggregation & Where-Provenance

- `src/Aggregation.{h,cpp}` and `src/aggregation_evaluate.c` handle GROUP BY provenance.
- `src/agg_token.{c,h}` — the `agg_token` composite type (UUID + running value).
- `src/WhereCircuit.{h,cpp}` and `src/where_provenance.cpp` — where-provenance (column-level tracking).
- `src/DotCircuit.{h,cpp}` and `src/view_circuit.cpp` — DOT visualisation through `graph-easy`.
- `src/to_prov.cpp` — PROV-XML export.

## Code Organization Conventions

**Mixed C/C++**: PostgreSQL interface code uses C (`*.c`); complex data structures (circuits, graph algorithms) use C++ (`*.cpp`); generic algorithms use header-only templates (`*.hpp`). Cross-language shims live in `src/c_cpp_compatibility.h`, `src/provsql_utils.{h,c}` (C side), and `src/provsql_utils_cpp.{h,cpp}` (C++ side).

**Version compatibility**: PostgreSQL version guards via `#if PG_VERSION_NUM < XXXXXX`. Version-specific SQL in separate files (`provsql.common.sql` + `provsql.14.sql`). Centralized compatibility shims in `src/compatibility.{h,c}`.

**Generated files**: `sql/provsql--1.5.0-dev.sql` is generated from `provsql.common.sql` and `provsql.14.sql` by the Makefile — do not edit it directly. The default version is set in `provsql.common.control` (currently `1.5.0-dev`).

**Errors / logging**: use the `provsql_error` / `_warning` / `_notice` / `_log` macros defined in `src/provsql_error.h`.

## Supported SQL Features

The query rewriter handles: SELECT-FROM-WHERE, JOIN (not outer/semi/anti), nested subqueries, GROUP BY, SELECT DISTINCT, UNION/UNION ALL, EXCEPT, VALUES, HAVING (with dedicated `having_semantics` pre-evaluation), aggregates with a `FILTER (WHERE …)` clause (routed through the normal aggregation-provenance path, yielding an `agg_token`), UPDATE/INSERT/DELETE (when `provsql.update_provenance` is enabled), CTEs, and WHERE / JOIN / UNION on `random_variable` columns (the comparator is lifted into per-row provenance as a `gate_cmp`; see the continuous-distribution chapter). Window functions are **not supported**: they execute and carry *tuple* provenance through (each output row gets its input row's token, walked via the `WindowFunc` arm of `insert_agg_token_casts_mutator`), but the windowed computation itself gets no aggregate-provenance semantics: `sum() OVER ()` stays a plain scalar, not an `agg_token`. `process_query` emits a `provsql_warning` (once per rewritten query level whose `q->hasWindowFuncs` is set and that actually involves provenance-tracked relations); top-level window functions also force the `classify_top_level` verdict to OPAQUE.

## Releasing the extension

The extension uses the `./release.sh X.Y.Z` script (preflight, version-string propagation across five files, signed tag, GitHub release, prompt-driven `1.X+1.0-dev` post-bump). **Before** you invoke `release.sh`, verify the upgrade-script discipline:

1. **`sql/upgrades/provsql--<prev>--<new>.sql` MUST contain every new SQL object added since the prior release**, not only the obvious enum / type additions. Inspection pattern:
   ```bash
   git diff v<prev>..HEAD -- sql/provsql.common.sql sql/provsql.14.sql
   ```
   Every `CREATE TYPE`, `CREATE FUNCTION`, `CREATE OPERATOR`, `CREATE CAST`, `CREATE AGGREGATE` whose name is new (or whose body / signature changed) must be replicated in the upgrade script. Use `CREATE OR REPLACE FUNCTION` freely. For DDL that does not support `OR REPLACE` (`TYPE`, `OPERATOR`, `CAST`, `AGGREGATE`), wrap each in a `DO` block that checks the appropriate `pg_catalog` table and skips on duplicate, so the script is idempotent. Signature changes need `DROP FUNCTION IF EXISTS <old sig>;` before the new `CREATE FUNCTION`. Mind dependency order: TYPE → in/out functions → CAST → constructors → arithmetic / comparison functions → OPERATOR → aggregate sfunc/ffunc → AGGREGATE → polymorphic dispatchers. **This has been forgotten in past releases**; if `provsql--<prev>--<new>.sql` is still a small stub when the rest of the SQL surface has grown, that is the bug.

2. **If the release appends values to the `provenance_gate` enum**, the script must end with `SELECT reset_constants_cache();`. The C side caches the OID of each gate-type enum value in a per-session, per-database table populated lazily by `get_constants()`. A backend warmed under the previous version retains `InvalidOid` for the new values; the first `create_gate(_, '<new>')` then raises `ProvSQL: Invalid gate type`. `reset_constants_cache()` (in `src/provsql_utils.c`) forces a fresh lookup on the next `get_constants()` call.

3. **`test/sql/extension_upgrade.sql` is the canary**: it runs `DROP EXTENSION provsql CASCADE`, `CREATE EXTENSION provsql VERSION '1.0.0'`, `ALTER EXTENSION provsql UPDATE`, then exercises one function from the latest version's surface. Every new release should add a deterministic smoke call for one feature it introduced (e.g. `expected(provsql.normal(2, 1))` for 1.5.0). Update `test/expected/extension_upgrade.out` accordingly. This file pays for itself when a future release forgets step 1.

4. The `RELEASE_NOTES.md` and `studio/RELEASE_NOTES.md` are **uncommitted staging drafts** maintained between releases and consumed by `release.sh`'s `$EDITOR` (extension) and by the hand-edit of `studio/CHANGELOG.md` (Studio). If they exist as tracked files at release time, remove them before merging the release branch so they do not land in `master`; preserve their content out-of-tree (e.g. `/tmp/`) for the release step.

5. `release.sh`'s green-CI check covers Linux, macOS, WSL workflows by SHA; it does not run `extension_upgrade` against an actually-installed previous-version package, so step 1 above is the only line of defence against shipping an incomplete upgrade script. CI then re-runs `extension_upgrade.sql` on every PostgreSQL major (PG12+, since `ALTER TYPE ADD VALUE` is not in-transaction-usable on older), which is where the canary fires if you missed something.

After `release.sh` finishes: `make deploy` rsyncs `website/_site/` to `provsql.org`. Tag-driven workflows (`pgxn.yml`, `build_and_test.yml` docker job) publish to PGXN and push `inriavalda/provsql:<version>` + `:latest` to Docker Hub. The Docker image inherits whatever `STUDIO_VERSION` is pinned in `docker/Dockerfile` at tag time, so a fresh extension release built before a Studio bump will carry the older Studio (acceptable per Studio's backwards compatibility floor; can be rebuilt later for a matched pair).

## ProvSQL Studio

`studio/` is a self-contained Python package (`provsql-studio` on PyPI) — a Flask app that points a browser at a ProvSQL-enabled database and provides:

- **Where mode**: highlights source cells contributing to each output value (auto-wraps queries with `provsql.where_provenance`).
- **Circuit mode**: renders the provenance DAG behind a UUID/agg_token, with a pinned-node inspector and an evaluation strip that runs any `sr_*` semiring, probability method, or PROV-XML export inline.

Layout: `studio/provsql_studio/` (Python source: `app.py`, `cli.py`, `db.py`, `circuit.py`, `static/`), `studio/tests/` (unit + Playwright e2e under `tests/e2e/`), `studio/scripts/` (developer-facing demo loaders), `studio/CHANGELOG.md`, `studio/pyproject.toml`. CI: `.github/workflows/studio.yml` (lint + matrix tests + package smoke), `.github/workflows/studio-release.yml` (tag-driven PyPI publish). User documentation: `doc/source/user/studio.rst`. Developer documentation: `doc/source/dev/build-system.rst` (see "Studio Releases" section).

**ProvSQL Playground** (`studio/web/`): the *unmodified* Studio Python (Flask + psycopg + sqlparse) runs in the browser, with no parallel JS/TS port — features flow through the real source untouched. It loads in **Pyodide** (CPython→WASM) over **PGlite** (PostgreSQL+ProvSQL→WASM) reached through a `psycopg`→PGlite shim (`studio/web/psycopg_pglite.py`); the synchronous `db.py` is bridged to async PGlite via **JSPI** (WebAssembly JS Promise Integration; requires a JSPI-capable browser, behind a flag on Firefox). To keep the WASM backend warm across switches, a never-reloaded **shell page** (`app.html`, `shell-boot.js`) owns the PGlite cluster + Pyodide + WASM Graphviz and hosts the UI in an **iframe** (`ui.html`, `child-boot.js`); the iframe's `fetch('/api/*')` is proxied to the shell over `postMessage`. A mode switch reloads only the iframe; a database switch reopens just PGlite. The tutorial and case studies (cs1, cs2, cs4–cs7; cs3 omitted for its large external GTFS download) are pre-seeded as separate databases, switchable from the connection chip; a **Reset** button drops and re-seeds them all. Deep links (`?mode=&db=&q=`) restore database/mode/auto-run query. The build is **fully self-hosted** — `build.sh` assembles the doc-root and vendors Pyodide, the wheel closure, Graphviz, and Font Awesome (no CDN at run time; e2e asserts zero off-origin requests) and writes a `THIRD-PARTY.html` notices page; it is path-portable (deploys unchanged under `/playground/`, with a `.htaccess` supplying the WASM MIME type). `studio/web/README.md` is the file-by-file reference.

**Releasing Studio**: bump `__version__` in `studio/provsql_studio/__init__.py`, hand-write a `## [X.Y.Z]` entry at the top of `studio/CHANGELOG.md` (PRs do not touch this file: the maintainer assembles the section from merged PR descriptions at release time), push, then tag `studio-vX.Y.Z`. The release workflow extracts the matching changelog section into the GitHub release notes and aborts if the section is missing. Studio's version stream is independent of the extension's: the compatibility floor (extension >= 1.5.0 for Studio 1.1.x; extension >= 1.4.0 for Studio 1.0.x) is in the studio.rst compatibility table.

## Documentation

- `doc/source/user/*.rst` — Sphinx user manual (tutorial, case studies, configuration, semirings, probabilities, shapley, where-/data-modification, temporal, studio, …).
- `doc/source/dev/*.rst` — developer architecture documentation (architecture, query-rewriting, memory, semiring-evaluation, probability-evaluation, aggregation, debugging, testing, build-system, coding-conventions, postgresql-primer, playground, …). **Read these first when you need an authoritative description of internals**; they are kept current. `dev/playground.rst` covers the WASM/browser build and the in-browser Studio architecture.
- `doc/source/c/`, `doc/source/sql/` — Doxygen integration for the C/SQL API references.
- `doc/source/_static/studio/*.png` — Studio screenshots; capture instructions below.
- `doc/source/_static/casestudy6/*.png` — case-study-6 (City Air-Quality Sensor Network) screenshots; captured via the same workflow.

Run `make docs` after editing any `.rst` or `doc/` file (per project memory).

## Studio screenshots for the docs

Doc assets live under `doc/source/_static/studio/*.png` and need to match the live UI when it changes (new Config-panel rows, new toolbar buttons, …). Two capture methods, in order of preference:

1. **OS-level (full fidelity)** — recommended for docs, since native form controls (toggles, range sliders, etc.) render correctly. Requires `xdotool` and `imagemagick` (`import`, `convert`).

   Before capturing, hide the Chrome-MCP overlays or they bake into the
   shot: the bottom indicator `#claude-static-indicator-container` and
   the large red cursor pointer the extension draws. Set them to
   `display:none` via `mcp__claude-in-chrome__javascript_tool` first.

   ```bash
   # 1. Find Chrome window ID
   xdotool search --name "ProvSQL Studio"

   # 2. ACTIVATE the window first, then capture. `import` fails with
   #    "missing an image filename" on this display server unless the
   #    target window is active.
   xdotool windowactivate <id>
   import -window <id> /tmp/chrome-full.png

   # 3. Find the target element's CSS rect via the browser, e.g. via
   #    mcp__claude-in-chrome__javascript_tool:
   #      document.getElementById('config-panel').getBoundingClientRect()

   # 4. Browser chrome (tabs + address bar) offsets the page region.
   #    The offset is `outerHeight - innerHeight` (typically ~143px on
   #    desktop Chrome). Add it to the CSS y-coordinate.

   # 5. Crop. Example for the Config panel at CSS (1508, 50, 320, 692)
   #    with a 143px chrome offset (so the OS y is 50+143 = 193):
   convert /tmp/chrome-full.png -crop 320x692+1508+193 \
           doc/source/_static/studio/config-panel.png
   ```

2. **html2canvas via the page** — works without OS-level tools, dumps a PNG into `~/Downloads/`, but renders custom-styled toggles and range sliders imperfectly. Use as a fallback when `import` is unavailable. Inject via `mcp__claude-in-chrome__javascript_tool`:

   ```js
   const s = document.createElement('script');
   s.src = 'https://cdn.jsdelivr.net/npm/html2canvas@1.4.1/dist/html2canvas.min.js';
   s.onload = async () => {
     const c = await html2canvas(document.getElementById('config-panel'),
                                 { scale: 2, backgroundColor: '#ffffff' });
     c.toBlob(b => {
       const a = document.createElement('a');
       a.href = URL.createObjectURL(b);
       a.download = 'config-panel.png';
       a.click();
     }, 'image/png');
   };
   document.head.appendChild(s);
   ```

   Caveat: `html2canvas` respects the element's `overflow` / `max-height`,
   so on a scrolling panel (e.g. the Config panel, whose content exceeds
   its viewport-capped height) it captures only the visible, clipped box.
   To grab the full content, first lift the caps on the element and its
   scroll ancestors (`el.style.maxHeight='none'; el.style.overflow='visible'`),
   then restore them after.

Do **not** rely on the `mcp__claude-in-chrome__computer` screenshot tool
to produce doc assets: it downscales to a fixed max width (~1568 px, so a
1920-wide window is captured at ~0.82×) and its `save_to_disk` file is
not reachable on the local filesystem. It is fine for *previewing* the UI,
but use OS-level `import` for the actual full-resolution crops.

## Website

`website/` is a Jekyll site published at `https://provsql.org/`. `make website` copies branding assets, rebuilds the Sphinx docs, and stitches Doxygen output into `website/docs/`, `website/doxygen-c/`, `website/doxygen-sql/` before running `bundle exec jekyll build`. `make deploy` rsyncs `website/_site/` to `provsql:/var/www/provsql/`.
