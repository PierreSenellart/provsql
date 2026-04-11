# Changelog

All notable changes to [ProvSQL](https://provsql.org/) are documented
in this file.  It mirrors the release-notes section of the website
([provsql.org/releases](https://provsql.org/releases/)) and is kept in
sync by the `release.sh` release-automation script.

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
