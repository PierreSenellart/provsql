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

## [1.4.0] - 2026-05-27

Companion release for ProvSQL extension 1.8.0. Brings the extension's
two new surfaces into Studio: the **external-tool registry** (a panel
to manage compilers / counters / KCMCP servers, with every tool picker
now driven by the registry) and **inversion-free** d-DNNF compilation
(a certificate badge, per-input order keys, and a new compilation /
benchmark target). Requires extension **>= 1.8.0**.

### Highlights

- **Tools panel.** A new panel manages the extension's external-tool
  registry (`provsql.tools`): tools are grouped by operation, and can be
  enabled / disabled, re-prioritised, edited, registered and
  unregistered inline, covering both CLI tools (executable + command
  template) and KCMCP servers (endpoint or managed). Nav popovers are
  mutually exclusive, and a registry change refreshes the Evaluate
  strip's compiler dropdowns immediately.
- **Registry-driven tool selection.** The compiler / counter pickers and
  the probability benchmark are now populated from `provsql.tools`
  rather than hardcoded lists, and only tools that currently resolve on
  the backend are selectable.
- **Inversion-free integration.** Studio renders the inversion-free
  `gate_annotation` as an **IF** badge on the certificate root
  (coexisting with the safe-query **B** badge); the inspector shows the
  certificate order and each input's per-input order key and rank.
  When the root carries a certificate, inversion-free is offered both as
  a d-DNNF compilation target in the compiler picker and as a method in
  the probability benchmark (routed through the elided root's original
  token).
- **Compiled d-DNNF timing.** Compiling a circuit to a d-DNNF (`kc-ddnnf`)
  runs under the session's `statement_timeout`, and the canvas subtitle
  reports the server-side compile time.

## [1.3.0] - 2026-05-24

Companion release for ProvSQL extension 1.7.0. Brings the extension's
new **knowledge-compilation surface** into Studio: a dedicated
inspector for compiled d-DNNFs, CNF, and tree decompositions, a
multi-compiler / multi-counter probability benchmark, and
tool-availability awareness throughout the UI. Requires extension
**>= 1.7.0**.

### Highlights

- **Knowledge-compilation inspector.** New `/api/kc/*` endpoints back
  a KC strip folded into the Evaluate strip: compile a circuit's
  Boolean provenance with any available compiler and inspect the
  result. Compiled d-DNNF circuits render directly in the circuit
  canvas (root at the top), with a result modal and a d-DNNF
  structural-statistics summary (the `ddnnf_stats` size column /
  canvas size summary). A `kc-nnf` panel shows the `.nnf` text export,
  and the CNF view is annotated with the `tseytin_cnf`
  variable-to-input mapping. The compiled circuit is labelled with its
  KC class, and the KC compilers are credited in the bibliography.
- **Tree-decomposition view.** Bags are coloured, clickable to
  inspect, and resolve back to their source rows; pinning a TD bag or
  a d-DNNF internal node hides the (inapplicable) evaluation strip.
- **Multi-backend probability benchmark.** The benchmark table runs
  every method the extension exposes, gains a compiled-size column,
  enforces a per-method `statement_timeout`, and runs under the
  session's `provsql.boolean_provenance` so its numbers match the
  marginal-probability path.
- **Tool-availability awareness.** Compiler / counter dropdowns and
  the benchmark are filtered to tools actually resolvable on the
  backend; `/api/kc/{ddnnf,td}` return `501 Not Implemented` (rather
  than `500`) when the required tool is absent, and missing-tool
  shortcuts surface as notices. WMC backends move to their own
  option group.
- **`provsql.fallback_compiler` in the Config panel.** A new dropdown
  with validation exposes the extension's fallback-compiler GUC.
- **Aggregated random variables.** The evaluation strip offers the
  distribution profile / moment / sample surface on `agg` and
  `semimod` gate targets, and hides the eval-strip options that do not
  apply to KC / agg / semimod targets.
- **UI polish.** Even 50/50 shell split so the dense left column
  matches the query / result pane; schema-panel key columns underlined
  (solid for primary keys, dotted for `repair_key` / BID grouping
  keys); circuit-canvas maximum zoom raised from 2.5 to 8 for dense
  circuits; result-table column sorting; trimmed Monte-Carlo seed input
  width.

## [1.2.0] - 2026-05-17

Companion release for ProvSQL extension 1.6.0. Surfaces the new
**safe-query rewriter** (Boolean-provenance mode) and the new
**TID / BID / OPAQUE classifier** through dedicated UI affordances:
a third selector value on the provenance-scheme widget, a
session-sticky toggle, per-result prov pills on the result-table
header, and per-relation prov sub-pills on the schema panel.

### Highlights

- **Boolean-provenance scheme integration**. A third value
  (`Boolean`) joins the existing provenance-scheme selector
  (Semiring / Where). When selected, Studio sets
  `provsql.boolean_provenance = on` on the backend for the
  duration of the session and the extension's safe-query
  rewriter fires on every accepted hierarchical-CQ query. The
  toggle is session-sticky: Studio retains the selected scheme
  across query executions, schema-panel clicks, and circuit-mode
  navigation, matching the existing Default / Where behaviour.
- **Result-pane classifier pill**. The `provsql` column header of
  every query result carries a per-query pill reflecting the classifier's
  verdict on the just-executed query, labelled `prov-tid` /
  `prov-bid` (TID and BID share the brand pill: the distinction matters
  for probabilistic evaluation but does not warrant a visual demotion) or
  bare `prov` with a muted tone for OPAQUE. The pill source is the
  `provsql.classify_top_level` NOTICE Studio captures from each query
  execution; the tooltip explains the kind in prose.
- **Schema-panel prov pills**. The per-relation PROV pill on each
  schema-panel row now carries a kind-aware label (`prov-tid` /
  `prov-bid` for tracked relations whose kind the classifier has
  certified, bare `prov` with a muted tone for OPAQUE, bare
  `prov` with the brand pill for relations on an older extension
  that does not surface a `prov_kind`). The kind is populated at
  schema-load time by probing the classifier on a
  `SELECT * FROM <relation>` query inside a SAVEPOINT-wrapped
  transaction; view bodies are probed identically (the classifier
  descends through `RTE_SUBQUERY`).
- **Schema-panel row-click**. Clicking a relation row in the
  schema panel now inserts `SELECT * FROM <rel>` into the
  query box (replacing the previous behaviour of typing the
  relation name without a SELECT). Bare-name click insertion is
  schema-qualified when the bare name does not resolve in the
  current search_path, so `public.foo` and `myschema.foo` both
  work after a single click.
- **Schema-panel column-count tooltip fix**. Views now report
  the actual number of columns of the underlying query in the
  hover tooltip, not the count of `pg_attribute` rows (which
  undercounted views with computed columns).

### Internal

- **Catalog scan filter**: the tracked-input enumeration now
  filters temp / toast schemas, so the schema panel only lists
  user-visible tracked relations.
- **Psycopg pool teardown**: the e2e test fixture closes the
  psycopg connection pool cleanly on Playwright shutdown,
  avoiding a "FATAL: terminating connection due to
  administrator command" trail in the test output.

### Compatibility

Minimum required extension version: **1.6.0**. See the
[compatibility matrix](https://provsql.org/docs/user/studio.html#compatibility)
in the user guide for the full table.

The Boolean-provenance mode selector and the TID / BID schema /
result pills are inactive on extensions older than 1.6.0: the
provenance-scheme selector falls back to the Default / Where
two-way picker, and the result/schema pills omit the per-relation
classifier sub-pill.

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
