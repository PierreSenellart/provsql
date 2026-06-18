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

## [1.6.0] - 2026-06-18

Companion release for ProvSQL extension 1.10.0. The headline feature
is **Contributions mode**, a per-input Shapley / Banzhaf heat-map over
a query result; the release also renders every circuit construct the
extension 1.10.0 routes produce (absorptive class, d-D certificates,
conditioned gates, Möbius nodes), and brings a series of Playground
and notebook improvements driven by first-user feedback. Requires
extension **>= 1.10.0**.

### Highlights

- **Contributions mode.** A fifth mode alongside Circuit, Where,
  Notebook and the query views: click a result cell to pin it as the
  target and get signed per-input contribution bars (Shapley or
  Banzhaf, backed by `shapley_all_vars`), with measure / method /
  mapping controls; source rows are labelled by their column values
  in table order, and the Method control exposes the extension's
  cost-based (`auto`) vs historical (`ladder`) d-DNNF construction
  routes. Covered in the user guide and a "Seeing it in Studio" coda
  to case study 2. With five modes, the mode switcher becomes a
  dropdown showing the current mode.
- **Absorptive provenance class.** The scheme selector becomes
  four-way (`where` / `semiring` / `absorptive` / `boolean`),
  including per-cell overrides in notebooks; absorptive-assumed
  circuits get an amber **A** badge and narrow the semiring menu to
  what is sound (strictly for cyclic-recursion truncation roots,
  leniently for load-time absorptive folds); a new "Tropical
  (min-plus, nonnegative)" entry in the eval strip makes min-cost
  reachability on cyclic data evaluable in one click.
- **Certificates and the new gates, rendered.** A green **D** badge
  marks gates carrying the persisted d-D certificate (deterministic /
  decomposable tooltips and an inspector row), joining the B / A / IF
  badges; surviving `assume()` wrappers get their own glyph;
  conditioned gates display with labelled evidence / query children;
  and a `gate_mobius` renders as a μ node with each child edge
  carrying its signed integer coefficient and the transparent lineage
  child labelled `lineage`.
- **Notebooks: math and more.** Markdown cells render LaTeX math via
  vendored KaTeX (auto-render, no CDN); notebooks gain a table of
  contents, internal links and a close-all action; the Load button
  also accepts a plain `.sql` file, appended as a ready-to-run cell.
- **Playground onboarding and data loading.** The landing page gains
  a call-to-action to the interactive tutorial plus per-case-study
  notebook links, and a first visit opens the tutorial notebook
  directly; boot, database switch and Reset show a modal busy overlay
  instead of a passive status bar. The in-browser psycopg shim
  implements `cursor.copy()`, so dump-style `COPY ... FROM stdin`
  loads (notably the bundled notebooks' data cells) actually populate
  tables; `?nb=` deep links open the notebook's bound database.

### Improvements

- SQL arriving via paste, drop, `?q=` links or a loaded file is
  cleaned of invisible Unicode (non-breaking spaces, zero-width and
  bidi characters, BOM) that PostgreSQL rejects with cryptic syntax
  errors, in both the query box and notebook cells.
- TID / BID pills are shared across views, approximation guarantees
  render uniformly, and aggregate moments display exactly where the
  extension computes them exactly.
- The absorptive badge placement is harmonised with the Boolean badge
  and the certificate frontier tag.

### Bug fixes

- Playground: the application `search_path` puts `public` first, so
  unqualified `CREATE TABLE` (notably the notebooks' setup cells) no
  longer lands in the `provsql` schema, which left the Schema panel
  empty after Run all.
- Playground: the case-study-8 deep link works (Markdown/KaTeX assets
  resolve under a sub-path, and the database is pre-created).
- The mode menu no longer renders white-on-white items.

### Demo and tests

- End-to-end suites walk case studies 2 (Contributions), 7 and 8
  through the live UI; the per-cell scheme cycle covers absorptive;
  navigation timeouts are sized for loaded CI runners.

## [1.5.0] - 2026-06-05

Companion release for ProvSQL extension 1.9.0. Two headline features:
**Notebook mode**, a Jupyter-style notebook over a ProvSQL database,
and the **ProvSQL Playground**, the full Studio running entirely in
the browser over PostgreSQL+ProvSQL compiled to WebAssembly. The
evaluation strip also gains the extension's guarantee-first
probability surface. Requires extension **>= 1.9.0**.

### Highlights

- **Notebook mode.** A third mode alongside Circuit and Where: an
  ordered list of cells (SQL, Markdown, circuit snapshots, semiring /
  probability evaluations) executed against a pinned kernel session
  with Jupyter state semantics: temp tables and `SET`s persist across
  cells, each cell runs in its own transaction, restart gives a clean
  slate. Includes the Jupyter two-mode keymap (Esc / Enter, a/b, dd/z,
  m/y, Shift/Ctrl/Alt+Enter), Markdown rendering with `sql`-fenced
  highlighting (vendored marked + DOMPurify), per-cell
  provenance-scheme overrides, an outline + compact-relations sidebar,
  and round-trips to Circuit mode that restore selection and scroll.
- **Tabs as database bindings.** Each notebook tab is bound to the
  database it was authored against (stamped in the `.ipynb`
  metadata); a binding mismatch shows a banner offering switch /
  create / rebind, never switching silently. `POST /api/databases`
  backs the create action, and a nav-bar action empties the connected
  database (drop user schemas, reinstall provsql).
- **`.ipynb` save / load.** Notebooks are nbformat-v4 files: outputs
  carry standard MIME fallbacks (HTML tables, self-sufficient SVG
  circuit snapshots, plain-text eval results) so GitHub and nbviewer
  render them directly, while Studio re-renders from richer
  `application/vnd.provsql.*` payloads. Tabs autosave to browser
  storage between explicit saves.
- **Bundled example notebooks.** The user-guide tutorial and case
  studies ship as self-establishing notebooks (idempotent setup cells:
  create a fresh database, Run all, get the whole study), generated
  from the documentation sources by `studio/scripts/rst2nb.py` with a
  CI drift check, opened from the `Open example...` menu or a
  `/notebook?nb=<name>` deep link.
- **ProvSQL Playground.** The unmodified Studio Python (Flask +
  psycopg + sqlparse) runs client-side in Pyodide over PGlite
  (PostgreSQL+ProvSQL in WebAssembly), published as static files at
  [provsql.org/playground](https://provsql.org/playground/): no
  install, fully self-hosted (no CDN at run time), with the tutorial
  and case-study databases pre-seeded, a Reset button, shareable
  `?mode=&db=&q=&nb=` deep links, and notebook mode included (kernel
  restart maps to `DISCARD ALL` on the single shared session). A
  shell-page/iframe split keeps the WASM backend warm across mode and
  database switches. Browser support is gated on WebAssembly JSPI.
- **Guarantee-first probability evaluation.** The eval strip leads
  with the extension 1.9.0 exact / relative / additive guarantee
  paths, adds the `karp-luby` and `d-tree` methods (with an
  (eps,delta) vs samples toggle for the approximate methods), renders
  the returned guarantee as a bound or value interval with the actual
  sample count, surfaces the resolved method (down to
  `compilation:d4` / `wmc:ganak`), and includes karp-luby in the
  probability benchmark.

### Improvements

- Tool-backed compiler / counter pickers and the fallback-compiler
  config row hide tools that are not available on the backend instead
  of listing dead options; the probability-method dropdown has hover
  tips for every entry.
- A folder icon under the eraser loads a `.sql` file into the query
  box; dump-style `COPY ... FROM stdin` blocks run through the COPY
  sub-protocol, so `pg_dump` outputs paste and run as-is.
- Numeric `value` / `mulinput` gate labels compact to 4 significant
  figures; tuple counts pluralize (`1 tuple`).
- Result cells holding `agg_token` arithmetic results display
  `value (*)` like plain aggregate tokens (requires extension 1.9.0).

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
