# Changelog

All notable changes to [ProvSQL Studio](https://provsql.org/docs/user/studio.html)
are documented in this file. Studio's version stream is independent of
the ProvSQL extension's; the
[compatibility matrix](https://provsql.org/docs/user/studio.html#compatibility)
in the user manual records each Studio release's minimum required
extension version.

This file is **maintained by hand at release time** by the Studio
maintainer (mirroring how `release.sh` maintains the extension's
top-level `CHANGELOG.md`); pull requests should not modify it. The
release workflow (`.github/workflows/studio-release.yml`) extracts the
section matching the tag's version and embeds it under "What's
changed" in the GitHub release notes.

## [1.1.1]

Patch release fixing a Circuit-mode crash on databases that contain
temp tables.

### Bug fixes

- **Crash on `/api/circuit` when another session has a temp table.**
  The tracked-input catalog scan was matching every relation with a
  `provsql uuid` column, including `pg_temp_NN.<name>` rows that
  transiently appear in another backend's `pg_class` snapshot before
  autovacuum cleans them up; the subsequent UNION ALL then dispatched
  against an unreachable temp relation and raised `UndefinedTable`.
  The catalog scan now filters `relpersistence <> 't'` and skips
  `pg_temp%` / `pg_toast%` namespaces, so Circuit mode is robust
  against any temp-table activity on the target database.

## [1.1.0]

Companion release for ProvSQL extension 1.5.0. Adds first-class
rendering and inspection for the continuous-distribution gate
family, conditional-inference workflow, simplified-circuit
rendering, and a new evaluator strip group for distribution
profile / sample / moment / support.

### Highlights

- **New gate-type renderers**: `gate_rv` leaves with
  distribution-kind glyphs (*N* / *U* / *E* / *Γ*); `gate_arith`
  with the operator glyph drawn from `info1`; `gate_mixture`
  with three labelled edges (`p` / `x` / `y` for Bernoulli
  mixtures, `key` plus one mulinput edge per outcome for
  categorical blocks) and the Bernoulli probability rendered
  inline in the parent circle.
- **Distribution profile evaluator**: new entry under the
  *Distribution* group of the eval strip. Header stats
  (`μ`, `σ²`), inline-SVG histogram, PDF/CDF toggle, per-bar
  tooltip with `σ`-markers, wheel zoom. Backed server-side by
  the new C entry point `rv_histogram`. When the gate has a
  closed-form, the panel also draws the analytical PDF (or CDF)
  on top of the histogram as a terracotta SVG path; Bernoulli
  mixtures, categoricals, and Diracs render as discs on stems
  in PDF mode and as a staircase in CDF mode. Universally-
  infeasible truncations short-circuit with an inline message
  instead of returning empty bars. Backed by the new C entry
  point `rv_analytical_curves`.
- **Sample evaluator**: second *Distribution* entry, drawing
  conditional samples via `rv_sample(token, n, prov)`. Renders
  as a `<details>` panel with a six-value inline preview and a
  "show full list" expander; when the MC acceptance rate
  truncates the run below the requested `n`, surfaces an
  actionable hint pointing at `provsql.rv_mc_samples`.
- **Moment evaluator**: third *Distribution* entry exposing
  `moment` / `central_moment` with `k` and raw/central
  selectors.
- **Support evaluator**: fourth *Distribution* entry showing the
  closed-form `support` interval (or its conservative
  all-real fallback).
- **Conditioning**: `Condition on` text input on the eval strip
  with row-provenance auto-preset (clicking a result-table cell
  stamps the row's provenance UUID). Toggleable
  `Conditioned by:` badge underneath; clicking the active badge
  clears the conditioning and reverts to the unconditional
  answer, clicking the muted badge restores the row prov.
  Manual edits stick within a row and reset on row navigation.
- **Simplified-circuit rendering**: Circuit mode honours the new
  `provsql.simplify_on_load` extension GUC and renders the
  in-memory simplified graph via the
  `simplified_circuit_subgraph` SRF; the Config-panel toggle
  switches between the raw, gate-creation view and the
  simplified, evaluation-time view.
- **Config-panel rows**: new sliders/toggles for
  `monte_carlo_seed`, `rv_mc_samples`, and `simplify_on_load`,
  matching the existing Provenance-section pattern.
- **Anonymous-input rendering**: anonymous `gate_input`
  probabilities render as a percentage in the node circle;
  tracked-table input gates continue to render `ι` per the
  existing iota convention.
- **Categorical and mixture inspector**: node-inspector entries
  for the two new gate shapes; single-outcome categoricals
  collapse to `as_random` in the simplifier.
- **Footer version readout**: the footer now displays the
  loaded ProvSQL extension version (read from `pg_extension`)
  and the Studio package version (`provsql_studio.__version__`)
  on the right edge, served by `/api/conn`. A new
  `provsql-studio --version` CLI flag prints the package
  version and exits.
- **Schema-panel column pills**: columns whose type is one of
  ProvSQL's circuit-bearing types now carry a terracotta
  column-level pill (`rv` for `random_variable`, `agg` for
  `agg_token`) next to the column name, mirroring the
  relation-level `prov` / `mapping` pills. The
  `create_provenance_mapping` click affordance is suppressed
  on these columns (their values are circuit references, not
  scalar tags).
- **Result-table column-type indicators**: each result-table
  `<th>` carries the column's SQL type name as a tooltip and,
  for ProvSQL-significant columns, the same `rv` / `agg`
  pills as the schema panel plus a purple `prov` pill on the
  row-provenance `provsql` column itself, so the affordance
  follows the data into the result without a round-trip to
  the schema panel.

### Demo and tests

- `studio/scripts/demo_continuous.{sql,py}` fixture loader for
  the sensors / air-quality narrative used in
  `doc/source/user/casestudy6.rst`; a standalone copy is also
  shipped as `doc/casestudy6/setup.sql` for the rendered docs.
- Playwright e2e at `studio/tests/e2e/test_continuous.py`
  covering `gate_rv` / `gate_arith` rendering, the
  *Distribution profile* evaluator (including the closed-form
  PDF/CDF overlay over the histogram bars), the *Sample* panel,
  the conditioning auto-preset, and a Monte-Carlo
  `p ∈ (0, 1)` smoke test on the sensors fixture.

### Compatibility

Minimum required extension version: **1.5.0**. See the
[compatibility matrix](https://provsql.org/docs/user/studio.html#compatibility)
in the user guide for the full table.

## [1.0.0]

First public release. Requires the ProvSQL extension at version 1.4.0
or later.

### Highlights

- **Circuit mode** renders the provenance DAG behind any result UUID or
  `agg_token` cell. Drag-to-reposition, wheel-to-zoom, frontier
  expansion via gold-`+` badges, plus fullscreen, fit-to-screen and
  reset-positions toolbar buttons. Pinning a node opens an inspector
  with gate-specific metadata; `input` and `update` gates expose the
  stored probability as click-to-edit (sends `set_prob` server-side).

- **Semiring evaluation strip** runs any compiled semiring against the
  pinned node (or the root by default). Optgroups for Boolean, Lineage
  (`formula` / `how` / `why` / `which`), Numeric (`counting` /
  `tropical` / `viterbi` / `lukasiewicz`), Intervals (`interval-union`),
  User-enum (`minmax` / `maxmin`), plus probability methods, PROV-XML
  export, and any user-defined wrappers over `provenance_evaluate`. The
  mapping picker filters by the selected semiring's expected value
  type.

- **Where mode** turns on `provsql.where_provenance` and wraps every
  `SELECT` so each output cell becomes hover-aware: the source rows
  that contributed to it light up in the per-relation sidebar. Each
  result row carries a `→ Circuit` button to switch modes with the
  same provenance UUID.

- **Schema panel** lists every selectable relation with `PROV` /
  `MAPPING` pills. Click-to-prefill `add_provenance` /
  `remove_provenance` / `create_provenance_mapping` calls into the
  query box.

- **Config panel** with Provenance, Session and Display-Limits
  sections. Values persist to `$XDG_CONFIG_HOME/provsql-studio/`
  (Linux), `~/Library/Application Support/provsql-studio/` (macOS) or
  `%APPDATA%\provsql-studio\` (Windows); same options exposed as CLI
  flags.

- **In-page connection layer**: DSN editor probes new endpoints with
  `SELECT 1` before swapping pools; database switcher; the per-batch
  `search_path` always pins `provsql` at the end (lock chip in the
  header).

- **Query history**: in-session, with `Alt+↑` / `Alt+↓` to step in
  place and a `History` listbox. Mode switching carries the current
  SQL forward via `sessionStorage`; auto-replay only after an explicit
  run.
