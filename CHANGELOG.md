# Changelog

All notable changes to [ProvSQL](https://provsql.org/) are documented
in this file.  It mirrors the release-notes section of the website
([provsql.org/releases](https://provsql.org/releases/)) and is kept in
sync by the `release.sh` release-automation script.

## [1.4.0] - 2026-05-09

Major release headlining the **ProvSQL Studio** companion (released
in parallel as `provsql-studio` 1.0.0 on PyPI) and a substantial
expansion of the compiled-semiring family.  The mmap circuit format
is unchanged from 1.3.0; an `ALTER EXTENSION provsql UPDATE` is
enough.

## ProvSQL Studio companion release

[**ProvSQL Studio**](https://provsql.org/docs/user/studio.html), a
self-contained Flask/JS web UI for provenance inspection, ships in
parallel as `pip install provsql-studio==1.0.0`.  Studio renders the
provenance DAG behind any UUID or `agg_token` cell, runs any
compiled semiring (or probability method or PROV-XML export) against
a pinned node, lights up the source rows of a Where-mode result via
hover, and prefils `add_provenance` / `create_provenance_mapping`
calls from the schema panel.  Studio's version stream is independent
of the extension's; the
[compatibility matrix](https://provsql.org/docs/user/studio.html#compatibility)
in the user guide records each Studio release's minimum required
extension version (1.0.0 ↔ extension 1.4.0+).  The Docker image
`inriavalda/provsql:1.4.0` bundles both, exposes Studio on port 8000,
and replaces the legacy Apache + `where_panel` PHP UI.

## New compiled semirings

Ten new `sr_*` evaluators land alongside the existing
`sr_formula` / `sr_counting` / `sr_why` / `sr_boolexpr` /
`sr_boolean` family.  All are dispatched through the
`provenance_evaluate_compiled` C++ path, so they evaluate in a
single circuit traversal and respect circuit caching.

- **`sr_how(token, mapping)`**: canonical `N[X]` polynomial
  provenance (how-provenance), the universal commutative-semiring
  carrier.

- **`sr_which(token, mapping)`**: which-provenance / lineage: the
  set of input labels that influence the result.

- **`sr_tropical(token, mapping)`**: tropical (min-plus) semiring
  on `float8`, returning the cost of the cheapest derivation.

- **`sr_viterbi(token, mapping)`**: Viterbi (max-times) semiring
  on `float8` `∈ [0, 1]`, returning the probability of the most
  likely derivation.

- **`sr_lukasiewicz(token, mapping)`**: Łukasiewicz fuzzy
  semiring: `+ = max`, `× = max(a + b − 1, 0)` on `float8`
  `∈ [0, 1]`, preserving crisp truth and avoiding the near-zero
  collapse of long product chains.

- **`sr_minmax(token, mapping, element_one)`** and
  **`sr_maxmin(token, mapping, element_one)`**: min-max / max-min
  m-semirings over a user-defined enum carrier (security-classification
  shape and trust/availability shape, respectively).  The third
  argument is a sample value of the carrier enum, used only for type
  inference.

- **PG14+: `sr_temporal(token, mapping)`,
  `sr_interval_num(token, mapping)`,
  `sr_interval_int(token, mapping)`**: interval-union m-semiring
  carriers over `tstzmultirange`,
  `nummultirange`, and `int4multirange`.  `sr_temporal` subsumes
  the old `union_tstzintervals_*` helpers (state functions,
  aggregates, monus) which have been removed; the user-facing
  `union_tstzintervals(token, mapping)` wrapper is now a thin
  `SELECT sr_temporal(...)` call retained for backward compatibility,
  and `timetravel`, `timeslice`, `history`, and `get_valid_time` now
  call `sr_temporal` directly.

- **`sr_boolexpr` signature change**: the provenance-mapping
  argument is now optional (`token2value regclass = NULL`).  When
  omitted, leaves are rendered as bare `x<id>` placeholders.
  Existing one-argument callers continue to work unchanged.

- **Paren elision in `sr_boolexpr` and `sr_formula` output**:
  redundant outer parentheses, parentheses around single-child
  subtrees, and parentheses around same-op nested subtrees are
  dropped at rendering time, so long expressions stay readable.
  The parsed expression is unchanged; only the textual form is
  shorter.  Callers that grep `sr_boolexpr` output for exact
  paren counts will need to adjust.

## Circuit introspection helpers

Two new SQL-level helpers expose the gate DAG so external tools can
walk a bounded slice without copying the entire circuit; Studio
uses them to render Circuit mode:

- **`circuit_subgraph(root UUID, max_depth INT DEFAULT 8)`**:
  returns `(node, parent, child_pos, gate_type, info1, info2, depth)`
  for the BFS-bounded subgraph rooted at `root`, joining
  `get_gate_type` / `get_children` / `get_infos` in a single
  recursive CTE and keeping every distinct DAG edge (a child reached
  from `k` parents within the bound contributes `k` rows; self-joins
  contribute one row per child position).

- **`resolve_input(uuid UUID)`**: returns
  `(relation regclass, row_data jsonb)` for the source row whose
  `provsql` column equals `uuid`, by enumerating every
  provenance-tracked relation.  Returns zero rows for non-input gates
  (`plus`, `times`, `agg`, …).

## `agg_token` rendering and the `aggtoken_text_as_uuid` GUC

`agg_token` cells now have two render modes, controlled by a new
`provsql.aggtoken_text_as_uuid` GUC:

- **Off (default, unchanged)**: cells render as `value (*)`.

- **On (typical for UI layers like Studio)**: cells render as the
  underlying UUID, so callers can click through to the provenance
  circuit; the `value (*)` side is recovered via the new
  `agg_token_value_text(uuid)` helper, which returns
  `get_extra(token) || ' (*)'` when `token` resolves to an `agg`
  gate.

`agg_token_out` is consequently `STABLE` rather than `IMMUTABLE`
(the chosen output now depends on a session GUC).

## `provsql.tool_search_path` and external-tool robustness

A new `provsql.tool_search_path` GUC (colon-separated directories
prepended to `PATH` when invoking external tools) replaces the
previous "tool must be on the postmaster's `PATH`" assumption.  The
external-tool dispatch layer also gains:

- **Pre-flight tool lookup with structured error decoding**: calls
  fail fast with a clear message when a required tool is missing,
  instead of waiting for an opaque downstream error.

- **`statement_timeout` translation**: a `statement_timeout` that
  fires during d4 / c2d / dsharp / weightmc compilation now becomes
  a proper SQLSTATE 57014 (`query_canceled`) cancel rather than a
  raw subprocess kill.

- **SIGINT translation**: Ctrl-C during `find_external_tool`
  pre-flight is translated into a proper PostgreSQL cancel.

- **Private mkdtemp dir**: external tools now run in a per-call
  `mkdtemp` directory, closing a `/tmp` race on shared hosts.

## Bug fixes

- **`provenance_aggregate` UUID collision under concurrent
  aggregation.** `SUM(id)` and `AVG(id)` over the same children
  could collapse to a single agg gate, after which their
  concurrent `set_infos` calls would overwrite each other's
  aggregation operator (and `provsql_having` would read the wrong
  `agg_kind` under cross-backend contention).  The aggregate
  function OID is now folded into the gate UUID so the two queries
  produce distinct gates.

- **`CircuitCache` poisoning under concurrent gate creation.** A
  rare lost-write between two backends creating the same gate
  could leave the cache pointing at the wrong type / children
  pair.  Fixed; the cache's return-value contract is now aligned
  with what the callers expect (`src/CircuitCache.cpp`).

- **`CircuitCache` poisoning when calling `get_gate_type` before
  `get_children`.** A separate cache-coherence bug along the
  `get_gate_type` → `get_children` ordering has been fixed.

- **Where-provenance column position for multi-table joins.**
  PROJECT-gate column positions were computed against the wrong
  RTE for multi-table joins, causing empty locator sets on some
  query shapes.  Fixed.

- **1.2.3 → 1.3.0 upgrade WARNING text.** The recovery-instructions
  block raised by the storage-layout check used `%s` placeholders
  inside a `RAISE WARNING`, where the substitution marker is `%`
  (no type letter); the data-directory path was rendered with a
  stray `s` appended (`<datadir>s` instead of `<datadir>`). Fixed.
  The behaviour of the upgrade itself was unaffected.

## Documentation

- **Studio user-guide chapter** (`doc/source/user/studio.rst`): a
  full walkthrough of Where mode, Circuit mode, the eval strip, and
  the Config panel, plus a compatibility matrix and screenshots.
  Cross-links from the introduction, semiring, and probability
  pages.

- **Semirings chapter expansion**
  (`doc/source/user/semirings.rst`, +300 lines): new sections
  documenting `sr_how`, `sr_which`, `sr_tropical`, `sr_viterbi`,
  `sr_lukasiewicz`, `sr_minmax` / `sr_maxmin`, and the PG14+
  interval-union family, with a capability matrix summarising
  each semiring's identities and δ-handling.

- **Expanded case studies**: Case Study 1 gains a
  *Minimum Security Clearance* step that walks `sr_minmax` over a
  `classification_level` enum mapping; case study 4 has its
  temporal-DB code samples migrated from `union_tstzintervals` to
  `sr_temporal`; case studies 3 and 5 are realigned with the new
  paren-elided `sr_boolexpr` / `sr_formula` output.

- **Developer guide**: new Studio architecture chapter
  (`doc/source/dev/studio.rst`) covering the Flask app layout, the
  `/api/*` surface, the auto-prepare and `search_path` pinning
  strategy, and how Circuit mode walks `circuit_subgraph`.

- **Build-system chapter**: new "Studio releases" section in
  `doc/source/dev/build-system.rst` documenting Studio's
  independent version stream, Trusted Publishing on PyPI, the
  `studio-v*` tag workflow, and the hand-edited
  `studio/CHANGELOG.md` discipline.

## Infrastructure

- **Studio CI workflow** (`.github/workflows/studio.yml`): Python
  3.10 / 3.11 / 3.12 / 3.13 × PostgreSQL 14 / 15 / 16 matrix
  covering pytest, Playwright e2e, ruff, and a wheel-install smoke.

- **Studio release pipeline**
  (`.github/workflows/studio-release.yml`): tag-triggered
  (`studio-v*`), publishes to PyPI via Trusted Publishing,
  attaches sdist + wheel to a GitHub release, embeds
  the matching `studio/CHANGELOG.md` section in the release notes,
  and aborts loudly if the section is missing.

- **Docker image rework**: bundles Studio (PyPI install at image
  build time, contributor `STUDIO_SOURCE=` override for editable
  installs); adds the PGDG apt source so any `PSQL_VERSION` resolves
  (Debian bookworm only ships 15); collapses the apt layer; replaces
  Apache + `where_panel/` with `provsql-studio` on port 8000;
  parallelises `make` in the build.

- **`where_panel/` removed.**  The legacy PHP where-provenance UI is
  superseded by Studio's Where mode.

## Upgrade procedure

```sh
make install
```

In each database that uses ProvSQL:

```sql
ALTER EXTENSION provsql UPDATE;
```

The mmap circuit format is unchanged from 1.3.0; for users already
on 1.3.x no migration is required.  Users still on 1.2.x must run
`provsql_migrate_mmap` first to move the flat
`$PGDATA/provsql_*.mmap` files into the per-database layout
introduced in 1.3.0; see the [1.3.0 release notes](https://provsql.org/releases/v1.3.0/)
for the full procedure.

## [1.3.1] - 2026-05-04

A bug-fix release focused on `repair_key` / `mulinput` correctness,
plus a corrected upgrade path from 1.2.3 and documentation additions
(a fifth case study and expanded material in case studies 1 and 2).
No on-disk format change relative to 1.3.0; an
`ALTER EXTENSION provsql UPDATE` is enough.

## Upgrade-script corrections

- **`sql/upgrades/provsql--1.2.3--1.3.0.sql`** shipped with 1.3.0 only
  carried the per-database mmap migration warning and missed two
  groups of SQL-surface changes that had landed in
  `provsql.common.sql` / `provsql.14.sql` during the 1.3.0 dev cycle:
  the lazy-input-gate refactor of `add_provenance` / `repair_key`
  (commit `f670b7f`) and the schema-qualified
  `provsql.time_validity_view` references in `timetravel`,
  `timeslice`, `history`, and `get_valid_time` (commit `1f59032`).
  Users on 1.2.3 who ran `ALTER EXTENSION provsql UPDATE TO '1.3.0'`
  ended up with a stale set of function bodies. The script in 1.3.1
  has been corrected and now applies all the missing changes; users
  still on 1.2.3 reach a clean 1.3.0-equivalent SQL surface when they
  upgrade after 1.3.1.

- **`sql/upgrades/provsql--1.3.0--1.3.1.sql`** applies the same
  catch-up changes on the 1.3.0 → 1.3.1 path so that users **already
  on 1.3.0** (who came through the broken upgrade) are brought back
  in sync. Fresh installs of 1.3.0 also run this script, but the
  CREATE OR REPLACE statements match the source already on disk, so
  it is a no-op for them.

## Bug fixes

- **`probability_evaluate(..., 'tree-decomposition')` on circuits
  containing `mulinput` gates.** Input gates produced by `repair_key`
  could share an internal id when their UUIDs were never materialised
  in the d-DNNF builder, causing the probability to be wrong and to
  vary from one session to the next on identical data. The aliasing
  has been removed (`src/BooleanCircuit.cpp`,
  `src/dDNNFTreeDecompositionBuilder.cpp`); a regression test
  (`test/sql/treedec_mulinput.sql`) covers the affected query shapes.

- **Off-by-one in `BooleanCircuit::rewriteMultivaluedGates`.** The
  splitter that turns a `mulinput` into a chain of independent
  Bernoulli inputs produced non-deterministic probabilities under
  self-join + `GROUP BY` queries. Fixed; the four built-in evaluation
  methods (default, `'possible-worlds'`, `'tree-decomposition'`,
  `'monte-carlo'`) now agree on `mulinput`-bearing circuits.

- **Shapley and Banzhaf computation on `mulinput` circuits.**
  `shapley()`, `shapley_all_vars()`, `banzhaf()`, and
  `banzhaf_all_vars()` previously walked through `mulinput` gates and
  returned meaningless values. They now raise a clear error
  identifying the unsupported gate type.

## Documentation

- **New Case Study 5: The Wildlife Photo Archive.** A 30-photo /
  13-species / 63-detection synthetic dataset demonstrates the
  `VALUES` clause, `repair_key` and the `mulinput` gate (with the
  numerical effect of mutual exclusion made explicit via
  `sr_boolexpr` and `probability_evaluate`), probabilistic ranking
  versus naive confidence thresholding, `EXCEPT` with monus, common
  table expressions, and `expected()` aggregates. The case study is
  bundled (no external data download) and is part of the regression
  suite.

- **Case Study 1** gains three steps in the circuit-inspection
  section: a tree-decomposition probability variant in the benchmark
  step, an `sr_boolexpr` step on the Nairobi monus token, and a
  programmatic circuit-inspection step using `get_nb_gates`,
  `get_gate_type`, `get_children`, and `identify_token`.

- **Case Study 2** gains two steps: a bulk Shapley/Banzhaf step using
  `shapley_all_vars` / `banzhaf_all_vars` (contrasted with the
  per-variable cross-join from the existing Steps 13 and 14), and a
  step on arithmetic over aggregate results illustrating the
  `agg_token` cast warning.

- **Copy-to-clipboard buttons** on every documentation code block
  (`sphinx-copybutton`). A small JS shim
  (`doc/source/_static/copybutton-shim.js`) papers over an
  incompatibility between `sphinx-copybutton` 0.4.0 (the version in
  Ubuntu Noble's apt) and Sphinx 9.

## Infrastructure

- Release tarballs and CI workflows exclude the `studio/`
  subdirectory for future developments.

## Upgrade procedure

```sh
make install
```

In each database that uses ProvSQL:

```sql
ALTER EXTENSION provsql UPDATE;
```

The mmap circuit format is unchanged from 1.3.0; no migration is
required.

## [1.3.0] - 2026-05-04

### Breaking change: per-database circuit storage

Prior to 1.3.0, the provenance circuit was stored in four flat files at
the root of the PostgreSQL data directory (`$PGDATA/provsql_gates.mmap`,
`provsql_wires.mmap`, `provsql_mapping.mmap`, `provsql_extra.mmap`),
shared across all databases in the cluster. Starting with 1.3.0, each
database gets its own isolated set of files under
`$PGDATA/base/<db_oid>/`.

**Users upgrading from 1.2.x must migrate their circuit data before
upgrading.** The new `provsql_migrate_mmap` tool handles this. If the
migration is skipped, existing circuit data becomes inaccessible (new
provenance queries still work, but provenance computed under the old
version is lost). The upgrade script detects old flat files and raises a
WARNING with recovery instructions if they are still present.

#### Upgrade procedure

1. Install the new ProvSQL binaries:
   ```
   make install
   ```

2. Run the migration tool as the `postgres` user:
   ```
   provsql_migrate_mmap -D $PGDATA -c <connstr>
   ```
   The tool reads the old flat files, collects root UUIDs from each
   database's provenance-tracked tables, writes per-database files under
   `$PGDATA/base/<db_oid>/`, and deletes the old flat files on success.

3. Restart PostgreSQL.

4. In each database that uses ProvSQL:
   ```sql
   ALTER EXTENSION provsql UPDATE;
   ```

#### If you forgot step 2

If PostgreSQL has already been restarted with the new binaries before
migrating, some empty per-database files may have been created. To
recover:

1. Delete the empty per-database files:
   ```
   rm -f $PGDATA/base/*/provsql_*.mmap
   ```
2. Restart PostgreSQL.
3. Immediately run `provsql_migrate_mmap` before executing any
   provenance query.

### Lazy input gate creation

`add_provenance()` no longer eagerly writes an input gate to the circuit
for every existing row in the table at the time it is called. Gates are
now created on first reference during a query, at the cost of a small
overhead on the first query that touches each row. This significantly
reduces the overhead of provisioning large tables.

### Four case studies

Four worked examples have been added to the documentation and are
included as regression tests:

- **Case Study 1: The Intelligence Agency**: simple introductory
  example with Boolean and why-provenance.
- **Case Study 2: The Open Science Database**: comprehensive example
  covering why-provenance, where-provenance, custom semirings,
  probabilities, Shapley and Banzhaf values.
- **Case Study 3: Île-de-France Public Transit**: Boolean provenance
  and formula inspection over GTFS transit data.
- **Case Study 4: Government Ministers Over Time**: temporal provenance
  with `union_tstzintervals` and time-validity views.

### Bug fixes

- Fix GROUP BY provenance aggregation silently dropped when ORDER BY
  referenced the semiring result column.
- Fix d-DNNF tree decomposition: deduplicate OR gate children to prevent
  double-counting in probability evaluation.
- Fix NULL dereference and out-of-bounds crashes in where-provenance on
  views.
- Fix temporal functions (`time_filter`, `time_range`, `in_interval`) to
  use schema-qualified `provsql.time_validity_view`, preventing failures
  when `search_path` does not include the `provsql` schema.
- Fix `sr_boolean` evaluation when the provenance mapping uses integer
  values.
- Fix where-provenance PROJECT gate positions for provenance tables that
  are not the first RTE in a query, causing empty locator sets on some
  PostgreSQL versions.

## [1.2.3] - 2026-04-12

### PGXN improvements

- Prevent indexing of secondary documentation directories
  (`doc/source/`, `doc/tutorial/`, `doc/demo/`, `doc/aggregation/`,
  `doc/temporal_demo/`, `where_panel/`) on the PGXN distribution page
  via `no_index` in `META.json`.

- Document PGXN as an installation channel in the user guide, with
  a note that `pgxn install` does not configure
  `shared_preload_libraries`.

- Add a GitHub Actions workflow (`.github/workflows/pgxn.yml`) that
  automatically publishes releases to PGXN on version-tag pushes.

### Documentation and repository housekeeping

- Add `CODE_OF_CONDUCT.md`.

- Add architecture dataflow diagram to the website overview page.

- Replace `sudo` with generic "as a user with write access to the
  PostgreSQL directories" wording across installation and contribution
  documentation.

## [1.2.2] - 2026-04-11

### In-place extension upgrades

`ALTER EXTENSION provsql UPDATE` is now supported, starting with this
release.  A committed chain of upgrade scripts under `sql/upgrades/`
covers every previous release (1.0.0 → 1.1.0 → 1.2.0 → 1.2.1 → 1.2.2),
so users on any historical version can upgrade in place without
dropping and recreating the extension.  The persistent provenance
circuit (memory-mapped files) is preserved across the upgrade: the
on-disk format has been binary-stable since 1.0.0, and the relevant
headers (`src/MMappedCircuit.h`, `src/provsql_utils.h`) now carry
explicit warnings so future contributors don't break that guarantee
by accident.

A pg_regress regression test (`test/sql/extension_upgrade.sql`)
exercises the full chain end-to-end on every PostgreSQL version in
the CI matrix, installing the extension at 1.0.0 from a frozen
install-script fixture and walking it up to the current
`default_version`.  See the new "Extension Upgrades" section of the
developer guide for the workflow contributors should follow when
making SQL changes.

### Repository housekeeping and discoverability

- **`CHANGELOG.md`** at the repository root, mirroring the release
  notes published at
  [provsql.org/releases](https://provsql.org/releases/).  It is
  automatically kept in sync by `release.sh`.

- **GitHub issue and pull-request templates** under `.github/`.
  The bug-report form prompts for PostgreSQL version, ProvSQL
  version, OS, a minimal SQL reproducer, and optional verbose-mode
  output; the PR template carries a contributor checklist and links
  to the developer guide.

- **DockerHub** image-version badge added to the README
  ([`inriavalda/provsql`](https://hub.docker.com/r/inriavalda/provsql))
  and a prose pointer on the website overview page.

- **PGXN `META.json`** at the repository root, making ProvSQL ready
  for submission to the [PostgreSQL Extension Network](https://pgxn.org).
  Submission will happen once upstream approval lands; no change to
  the build or install flow in the meantime.

- **`CITATION.cff`** now carries the Zenodo concept DOI
  ([`10.5281/zenodo.19512786`](https://doi.org/10.5281/zenodo.19512786))
  and a Software Heritage archive URL in its `identifiers` block.

### Infrastructure

- `release.sh` learned to update `CITATION.cff`, `CHANGELOG.md`, and
  `META.json` in sync with `provsql.common.control` and
  `website/_data/releases.yml`, and to enforce the presence of an
  upgrade script (auto-generating a no-op when no SQL sources have
  changed since the previous tag).
- CI workflows now fetch git tags so `git describe` works inside
  the pgxn-tools containers, which unblocks the Makefile's
  dev-cycle upgrade-script generation.
- The four build / docs workflows' `paths-ignore` lists exclude
  `META.json`, `.github/ISSUE_TEMPLATE/**`, and
  `.github/pull_request_template.md`, so metadata-only edits do not
  trigger the full CI matrix any more.

### No SQL-level changes

There are **no changes to `sql/provsql.common.sql` or
`sql/provsql.14.sql`** in this release.  The SQL API, query
rewriter, semiring evaluators, and probability machinery are
unchanged from 1.2.1.  The upgrade script `1.2.1 → 1.2.2` is
accordingly an empty placeholder.

## [1.2.1] - 2026-04-11

Maintenance release headlining the new **developer guide** and laying
the groundwork for long-term archival and citation.

### Highlights

- **Developer guide** (14 chapters, ~3500 lines): PostgreSQL extension
  primer, architecture, query rewriting pipeline, memory management,
  where-provenance, data-modification tracking, aggregation semantics,
  semiring and probability evaluation (including the block-independent
  database model and the expected Shapley / Banzhaf algorithm from
  Karmakar et al., PODS 2024), coding conventions, testing, debugging,
  and the build system.  Cross-references to Lean 4 machine-checked
  proofs of the positive-fragment rewriting rules and the m-semiring
  axioms.  See the new Developer Guide tab in the documentation.

- **User guide updates**: expanded coverage of `expected()`, the
  `choose` aggregate, custom semiring evaluation, and diagnostic
  functions.  "Formula semiring" has been renamed to "symbolic
  representation" throughout.

- **`CITATION.cff`**: standard citation metadata at the repo root.
  GitHub now shows a **Cite this repository** button that emits
  BibTeX and APA for the ICDE 2026 paper.

- **Software Heritage** archival is active — the full repository
  history is continuously preserved at archive.softwareheritage.org.

- **Zenodo integration** enabled: starting with this release, every
  tagged version receives a persistent DOI
  ([10.5281/zenodo.19512786](https://doi.org/10.5281/zenodo.19512786)).

### Fixes

- `create_provenance_mapping_view` is now available on all supported
  PostgreSQL versions, not only PG 14+.

- External-tool tests (`c2d`, `d4`, `dsharp`, `minic2d`, `weightmc`,
  `view_circuit_multiple`) now skip cleanly when the tool is not
  installed, instead of being removed from the test schedule.

### Infrastructure

- Automated documentation coherence check runs in CI (validates every
  `:sqlfunc:` / `:cfunc:` / `:cfile:` / `:sqlfile:` reference resolves
  to a live Doxygen anchor).
- Mobile-friendly Doxygen and Sphinx output.
- CI speedups: concurrency groups, skip-on-tags, macOS `pg_isready`
  race fix.
- In-place extension upgrades via `ALTER EXTENSION provsql UPDATE` are
  supported starting with this release; upgrade scripts live under
  `sql/upgrades/` and the path is exercised by an automated CI test.

## [1.2.0] - 2026-04-10

This release focuses on providing broader and more consistent support
for SQL language features within provenance tracking.  Systematic
testing across a wide range of query patterns led to numerous bug
fixes, new feature support, and clearer error messages for unsupported
constructs.

### New Features

- **CTE support**: Non-recursive `WITH` clauses now fully track
  provenance.  Nested CTEs (CTE referencing another CTE) and CTEs
  inside `UNION`/`EXCEPT` branches are supported.  Recursive CTEs
  produce a clear error message.

- **`INSERT ... SELECT` provenance propagation**: When both source
  and target tables are provenance-tracked, `INSERT ... SELECT` now
  propagates source provenance to the inserted rows instead of
  assigning fresh tokens.  A warning is emitted when the target table
  lacks a `provsql` column.

- **Correct arithmetic and expressions on aggregate results from
  subqueries**: Explicit casts (`cnt::numeric`), arithmetic
  (`cnt + 1`), window functions (`SUM(cnt) OVER()`), and expressions
  (`COALESCE`, `GREATEST`, etc.) on aggregate results from subqueries
  now produce correct values with a warning, using the original
  aggregate return type from the provenance circuit.

- **UNION ALL with aggregate columns**: `UNION ALL` of queries
  returning aggregate results now works correctly.

### Bug Fixes

- Fixed crash when mixing `COUNT(DISTINCT ...)` with `provenance()` or
  `sr_formula(provenance(), ...)` in the same query.

- Fixed `COUNT(*)` returning NULL instead of `0 (*)` on empty results
  without `GROUP BY`.

- Fixed `provenance_cmp` function failing with "function
  uuid_ns_provsql() does not exist" when `provsql` was not in
  `search_path`.

### Improved Error Messages

- `provenance_evaluate` on unsupported gate types now reports the
  specific gate type and suggests using compiled semirings.

- Subquery errors now read "Subqueries (EXISTS, IN, scalar subquery)
  not supported" instead of the misleading "Subqueries in WHERE
  clause".

- Clear error messages for unsupported operations on aggregate
  results: `DISTINCT` on aggregates, `UNION`/`EXCEPT` (non-ALL) with
  aggregates, `ORDER BY`/`GROUP BY` on aggregate results from
  subqueries.

- Dropped redundant "by provsql" suffix from all error messages (the
  "ProvSQL:" prefix is already present).

### Documentation

- Updated supported/unsupported SQL features list with accurate
  coverage based on systematic testing.

- Added documentation for `INSERT ... SELECT` provenance propagation.

- Expanded aggregation documentation with examples of casts, window
  functions, `COALESCE`, and `GREATEST` on aggregate results.

- Added workaround guidance for unsupported features (use `LATERAL`
  for correlated subqueries, explicit cast for comparison on
  aggregates).

## [1.1.0] - 2026-04-09

### Support for arithmetic on aggregate results

Queries performing arithmetic on aggregate results (e.g.,
`SELECT COUNT(*)+1` or `SUM(id)*10`) are now supported.  Previously,
these queries produced incorrect results because the planner hook
replaced aggregate references with `agg_token` values without
adjusting surrounding operator type expectations.  This is handled by
adding implicit and assignment casts from `agg_token` to standard SQL
types (`numeric`, `double precision`, `integer`, `bigint`, `text`),
and by inserting appropriate type casts during query rewriting when
aggregate results are used inside operators or functions.  A warning
is emitted when provenance information is lost during such
conversions.

### Infrastructure improvements

- Versioned Docker image tagging (images are now tagged with the
  release version in addition to `latest`).
- Improved release process: post-release version bump is now
  automated, and release tarballs exclude non-essential files (CI
  workflows, release script, branding, Docker, and website assets).
- CI fixes for macOS and documentation builds.

## [1.0.0] - 2026-04-05

Initial official release of ProvSQL after 10 years of development.
ProvSQL is now fully documented and usable in production.
