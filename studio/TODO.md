# ProvSQL Studio – TODO

A Python-backed web UI for ProvSQL that replaces the unmaintained `where_panel/` PHP demo with a branded, distributable tool. It is a single app with two complementary modes that share most of the chrome (top nav, query textarea, result rendering) and let the user flip between them while keeping the current query and result.

* **Where mode** (`/where`): the inspection surface for where-provenance. `provsql.where_provenance` is enabled on the connection, every user query is wrapped to carry its where-provenance, and a sidebar lists the provenance-tagged relations with their content. Hovering a result cell highlights the source cells that contributed to it.
* **Circuit mode** (`/circuit`): the inspection surface for the provenance DAG. `provsql.where_provenance` is off by default (the user can toggle it). No source-table sidebar. The query runs unwrapped, so `provenance()` / `agg_token` columns appear as raw values; clicking a UUID or `agg_token` cell opens the circuit DAG for that token in the sidebar (× / + gates, hover-highlight subtree, click to pin and inspect, lazy expansion past a depth cutoff).

Both modes share: the textarea, "Send query" button, multi-statement support (writes allowed), result table rendering, classification pills, error / status display. A mode switcher in the top nav swaps the sidebar and re-runs the current query (because the wrapping differs between modes).

Stack: Python ≥ 3.10, Flask, psycopg 3, Graphviz (`dot -Tjson`) for layout. Static front-end (no bundler) using HTML/CSS/JS from the Claude Design "Provence" handoff bundle.

Replaces: `where_panel/` (to be removed once Studio ships).

Estimated effort: stages 0-5 sum to about 6.5 days of focused work; documentation and CI add 1-2 days on top. Roughly two weeks elapsed for v1, depending on how much is parallelisable.

---

## Stage 0: Bring in design assets

The relevant Claude Design bundle files are already vendored under `studio/design/` (see `studio/README.md` for the layout). This stage merges them into a single flat static tree.

Target layout:

```
provsql_studio/static/
├── index.html              shared shell, served at both /where and /circuit
├── colors_and_type.css     brand tokens (from studio/design/)
├── app.css                 merged: panel.css + circuit.css
├── app.js                  entry: chrome, mode switch, exec, result render
├── circuit.js              DAG render + interactions (imported by app.js)
├── fonts/                  symlink to branding/fonts
└── img/                    symlink to branding/{logo.png, favicon.ico}
```

* [x] Build `static/index.html` from `studio/design/ui_kits/where_panel/index.html`. Strip the hardcoded `personnel` markup (rendered into `#sidebar-body` by `app.js` in where mode); make the sidebar a generic `<aside id="sidebar">` whose content is rendered by `app.js` per mode; add the mode switcher to the top nav.
* [x] Merge `panel.css` + `circuit.css` into `static/app.css`. Both are scoped enough (`wp-*` and `cv-*`/gate-class prefixes) that they coexist without conflict; the only overlap is the global base block (`* { box-sizing }`, `html, body { … }`), unified at the top of `app.css` with a `body.mode-circuit` override for the 100vh+overflow:hidden circuit canvas.
* [x] Merge `panel.js` (where-mode sidebar + hover-highlight + mock query runner) into `static/app.js`; copy `circuit.js` (DAG render) verbatim. Lazy-loading from `app.js` is left to Stage 2/3 since circuit.js still ships the bundle's mock data and queries DOM that isn't yet built into the shared shell.
* [x] Copy `studio/design/colors_and_type.css` into `static/`.
* [x] Replace the bundle's local `@font-face` block (Latin only) by loading the upstream `branding/fonts-face.css` (Greek + Latin-extended), symlinked as `static/fonts-face.css` and loaded directly from `index.html`. Also added `--font-body` / `--font-display` / `--font-ui` aliases since panel.css and circuit.css reference those names but `colors_and_type.css` only defined `--font-serif` / `--font-sans` / `--font-mono`.
* [x] Symlink `static/fonts/` → `branding/fonts/`, `static/img/logo.png` → `branding/logo.png`, `static/img/favicon.ico` → `branding/favicon.ico`. Symlinks are tracked in git as mode 120000.
* [x] Update relative paths in the merged HTML / CSS so fonts, logo, and stylesheets resolve from `static/`.

---

## Stage 1: SQL helpers in the extension (1 day)

These two functions are useful to anyone building a custom UI on top of ProvSQL, not just to Studio. Ship them as part of the next extension release independently of Studio.

### `provsql.circuit_subgraph(root UUID, max_depth INT DEFAULT 8)`

Returns one row per (parent, child) edge in the BFS expansion of the circuit, with child ordinality, gate type, and `info1` / `info2` from `get_infos`.

```sql
CREATE OR REPLACE FUNCTION provsql.circuit_subgraph(
  root UUID, max_depth INT DEFAULT 8
) RETURNS TABLE(
  node UUID, parent UUID, child_pos INT,
  gate_type TEXT, info1 TEXT, info2 TEXT, depth INT
);
```

* [x] Implement as a recursive CTE in `sql/provsql.common.sql`. Model on existing `sub_circuit_with_desc` (line 888) and `sub_circuit_for_where` (line 956), adding ordinality + infos + depth.
* [x] Cap recursion at `max_depth`; record `depth` per node (BFS-shortest distance from the root) so the front-end can identify frontier nodes for lazy expansion.
* [x] Emit one row per ``(parent, child)`` edge — the circuit is a DAG, so a node with k parents within the bound contributes k rows. Callers needing a one-row-per-node view dedup on `node`. (Earlier versions deduplicated to one row per node and silently dropped multi-parent edges, which broke DAGs with shared subgraphs.)
* [x] Add Doxygen-style comment block; the existing `sql/postprocess-sql-html.py` will pick it up.
* [x] No `mulinput` rejection added: a `mulinput` gate has a single `input` child (the row key), which BFS traverses naturally. `sub_circuit_with_desc` / `sub_circuit_for_where` likewise don't reject.

### `provsql.resolve_input(uuid UUID) RETURNS TABLE(relation regclass, row_data JSONB)`

Given an `input` gate UUID, find which provenance-tagged relation contains a row with that `provsql` column value, and return the row as JSONB. Centralizes the leaf-resolution logic the old PHP did inline.

* [x] Implement in `sql/provsql.common.sql` using dynamic `EXECUTE` over the same `pg_attribute` query the old `where_panel/index.php:100-145` uses to find tagged relations.
* [x] Return zero rows for a non-input gate or an unmapped UUID (don't raise).
* [x] Add Doxygen comment.

### Tests

* [x] `test/sql/circuit_subgraph.sql` + expected output. Cover: a few `personnel`-based queries (DISTINCT, UNION, GROUP BY, self-join), a depth-limit case, a `where_provenance = on` case (so `project` gates appear), an `input` leaf, plus a BFS-tree invariant check. (`eq` gates only arise from explicit equijoins under `where_provenance = on`; the self-join case already covers the join shape, so no separate `eq` test was added.)
* [x] `test/sql/resolve_input.sql`. Cover: known input gate, all-rows round-trip, unknown UUID, non-input gate.
* [x] Add both to `test/schedule.common` (on separate lines: their `pg_attribute` discovery and CREATE/DROP TABLE conflict if run as a parallel group).

### Docs

* [x] Add a "Subcircuit introspection" subsection to `doc/source/user/export.rst`, mentioning both functions with a short example.
* [x] Run `make docs` after edits (per project convention).

---

## Stage 2: Studio skeleton + shared chrome + Where mode (1.5 days)

### Package layout

```
studio/
├── pyproject.toml
├── README.md
├── TODO.md                            (this file)
├── provsql_studio/
│   ├── __init__.py                    __version__
│   ├── __main__.py                    `python -m provsql_studio`
│   ├── cli.py                         argparse: --dsn, --host, --port,
│                                                --statement-timeout,
│                                                --max-circuit-depth,
│                                                --max-circuit-nodes
│   ├── app.py                         Flask app factory
│   ├── db.py                          psycopg pool, exec helpers
│   ├── circuit.py                     graphviz layout (Stage 3)
│   └── static/                        (vendored from Stage 0)
└── tests/
    ├── conftest.py                    spin up a test DB, install extension
    ├── test_exec.py
    ├── test_relations.py
    └── test_circuit.py                (Stage 3)
```

### Server

* [x] `pyproject.toml`: deps (`flask`, `psycopg[binary]>=3.1`, `psycopg_pool`, `sqlparse`); optional-deps `test = ["pytest", "pytest-flask"]` and `dev = ["ruff", "mypy"]`; MIT license; console script. Skipped `pydot` and `dot` shellout; those land in Stage 3 with `circuit.py`.
* [x] `cli.py`: argparse, defaults `--host 127.0.0.1 --port 8000`. DSN from `--dsn` or psycopg's PG* env vars.
* [x] `db.py`: `psycopg_pool.ConnectionPool`; `exec_batch(...)` runs a list of pre-split statements in one transaction with `SET LOCAL statement_timeout` + per-mode `provsql.where_provenance`. Returns a `StatementResult` for the last statement (rows / status / error). `list_relations` yields per-relation content as `SELECT *` would render it (the trailing `provsql` column is included; the rewriter strips and re-adds it, so we list the user columns first and pull provsql out by name).
* [x] `app.py`: Flask app factory with the routes below.

### Routes

The two routes share the same shell HTML and JS bundle; they differ in which sidebar component is rendered, in the default value of `provsql.where_provenance` for the request, and in whether `/api/exec` wraps the user query.

* [x] `GET /` – redirect to `/where`.
* [x] `GET /where` – serves the shared shell with `mode=where`.
* [x] `GET /circuit` – serves the shared shell with `mode=circuit`.
* [x] `GET /api/relations` – lists provenance-tagged relations with column names + types + rows, exactly as `SELECT *` would render them under the active rewriter (so the trailing `provsql` UUID column is included).
* [x] `POST /api/exec` – body `{sql: string, mode: "where"|"circuit"}`. The user's SQL may contain multiple statements (DDL / DML / SELECT) separated by `;`. **Only the last statement's result is displayed.** Server uses `sqlparse` to split (handles dollar-quoting, comments, string literals); all but the last statement run verbatim with results discarded (errors halt the batch). The last statement is potentially wrapped:

  * In **where mode**, if the last statement is a `SELECT` or `WITH ... SELECT`, it is wrapped:

    ```sql
    SELECT *, provsql.provenance() AS __prov,
              provsql.where_provenance(provsql.provenance()) AS __wprov
    FROM (<last-statement>) t
    ```

    The front-end strips `__prov` / `__wprov` from the displayed columns and uses them for hover-highlighting and the "→ Circuit" button.
  * In where mode, also sets `provsql.where_provenance = on` for the request.
  * In **circuit mode**, the last statement runs unmodified.
  * For non-SELECT last statements (DDL, DML, `EXPLAIN`, `CREATE TABLE AS`, ...), no wrapping; status / returned rows shown as-is.

  Returns `[{kind, ...}]` with one element corresponding to the last statement (plus error blocks for any earlier failures):
  * `{kind: "rows", columns: [{name, type_oid, type_name}], rows, row_provenance: [uuid, ...] | null, where_sources: [[id, ...], ...] | null}` for SELECTs.
    * `row_provenance`: per-row provenance UUID when the query carries one (where mode wraps for it; circuit mode only when the user wrote `provenance()` themselves).
    * `where_sources`: filled in where mode, null in circuit mode.
    * `columns[].type_name`: the front-end uses this to flag UUID and `agg_token` cells as clickable in circuit mode.
  * `{kind: "status", message, rowcount}` for DDL / DML.
  * `{kind: "error", message, hint, sqlstate}` if a statement fails. Halt the batch on error (psycopg already rolls back).
  * Per-request: `SET LOCAL statement_timeout = '30s'` (from CLI flag).
* [x] `GET /api/config` – returns the four whitelisted GUCs.
* [x] `POST /api/config` – body `{key, value}`. Whitelist enforced server-side.

### Front-end

The shared shell from Stage 0 (one `index.html`, `app.css`, `app.js`, `circuit.js`) is the starting point. This stage wires up the chrome (top nav, mode switcher, query form, result rendering) and the Where-mode sidebar. The Circuit-mode sidebar is stubbed here and filled in in Stage 3.

* [x] Mode switcher in the top nav: two-segment toggle, anchors to `/where` and `/circuit` (Flask serves the same shell with a different body class). Stage 0 wired the visual; Stage 2 just hooks the active marker.
* [x] Shared query form + result renderer: `POST /api/exec`, renders rows / status / error blocks. Re-fetches `/api/relations` after each successful exec in where mode. ⌘ / Ctrl+Enter submits. Loading state shown while a query is in flight. Empty-result handling shows "0 rows".
* [x] **Where mode** (sidebar = source relations): on mode entry, `GET /api/relations` renders each tagged relation. Hovering a result cell toggles `is-source` on the matching `<td>` in the sidebar via the cell's `data-sources` list. `data-sources` comes from the wrapped query's `__wprov` column, parsed client-side from the `{[table:uuid:col;...],[...]}` text format produced by `where_provenance.cpp`.
* [x] **Circuit mode** (sidebar = circuit DAG): placeholder only in Stage 2. Stage 3 covers cell-click → lazy-import circuit.js → fetch `/api/circuit/<uuid>` → render the DAG.

### Tests

* [x] `tests/conftest.py` spins up a one-off DB keyed on a random suffix and runs `test/sql/{setup,add_provenance}.sql` against it (security.sql skipped: the existing test suite covers the planner-level security checks; Studio just relays the user's role). `PROVSQL_STUDIO_TEST_DSN` overrides for CI.
* [x] `tests/test_exec.py` (9 tests): SELECT → rows, CREATE → status, syntax error → error block, multi-statement returns only the last, syntax error in a non-final statement halts the batch, where-mode wrapping skips non-SELECT last statements (DML), `WITH ... SELECT` is wrappable, dollar-quoted function bodies in earlier statements don't break the splitter, circuit mode does not wrap.
* [x] `tests/test_relations.py` (3 tests): `personnel` listed with the right columns + types in `SELECT *` order, every row carries a 36-char provenance UUID matching the provsql cell, fresh `add_provenance('foo')` shows up on the next `/api/relations` call.

---

## Stage 3: Circuit mode + cross-mode navigation (2 days)

### Server side

* [x] `circuit.py`: given a root UUID and depth, call `provsql.circuit_subgraph`, build a Graphviz DOT string, run `dot -Tjson` (subprocess), parse the JSON to extract per-node coordinates, return `{nodes: [{id, type, label, info1, info2, depth, x, y, frontier: bool}], edges: [{from, to, child_pos}]}`.
  * Frontier detection: we ask `circuit_subgraph` for `depth + 1`, keep nodes through depth, and use the existence of any depth+1 child to mark depth-`depth` parents as frontier. A separate post-BFS `get_children` probe doesn't work: the provsql backend cache populated by `get_gate_type` (called at the tail of `circuit_subgraph`) masks subsequent direct `get_children` calls in the same backend.
  * Cap at `--max-circuit-nodes` (default 500); if exceeded, return a 413 with `{error: "circuit too large", hint: "reduce depth or expand interactively"}`.
* [x] Cache layouts in-process keyed on `(root_uuid, depth)` since Graphviz is not free. Bounded LRU at 32 entries.
* [x] `GET /api/circuit/<uuid>?depth=<N>` – initial render. The path token is parsed as a UUID; in studio sessions the `provsql.aggtoken_text_as_uuid` GUC makes `agg_token` cells already arrive as bare UUIDs, so the click handler sets `data-circuit-uuid` straight from the cell value (no SQL-level cast needed).
* [x] `POST /api/circuit/<uuid>/expand` – body `{frontier_node_uuid, additional_depth}` – computes a fresh subgraph rooted at the frontier; the front-end translates it to the anchor's `(x, y)` and merges. Stateless on the server.
* [x] `GET /api/leaf/<uuid>` – calls `provsql.resolve_input(uuid)`. Returns `{matches: [{relation, row}, ...]}` or 404.

### Front-end (circuit-mode sidebar, from `static/circuit.js`)

* [x] Strip the bundled `QUERIES` mock data; the sidebar starts empty until a UUID cell is clicked in the result. Exposed as `window.ProvsqlCircuit = { init, renderCircuit, setStatus, showLoading, showError }` so `app.js` lazy-loads it on demand.
* [x] On click of a typed cell: `GET /api/circuit/<value>`, render the DAG in the sidebar.
* [x] Replace the bundle's centroid layout with the server-supplied `{x, y}` per node; the local `circuit.js` only paints + handles interactions.
* [x] Click a frontier node: `POST /api/circuit/.../expand`, merge into the existing scene, re-paint. Frontier nodes get a small gold "+" badge so users know which gates expand.
* [x] Inspector panel: when an `input` / `mulinput` gate is pinned, fire `GET /api/leaf/<uuid>` and show the resolved `(relation, row)`.
* [x] Toolbar: zoom / fit / UUID toggle / formula-strip toggle wired into the new state machine.

### Cross-mode navigation

* [x] Mode switcher in the top nav: stashes the SQL textarea into `sessionStorage.ps.sql`; the destination page restores it before running. The active marker swaps server-side via the body class set by Flask.
* [x] In where-mode result, each row gets a small "→ Circuit" button that switches to circuit mode and pre-loads the circuit for that row's provenance UUID via `sessionStorage.ps.preloadCircuit`.
* [x] In circuit-mode result, when no UUID / `agg_token` cells are present, the legend surfaces a hint: "No UUID columns in this result: switch to Where mode … or add `provsql.provenance()` to your SELECT."
* [x] `agg_token` cells are clickable in circuit mode. Backed by a new `provsql.aggtoken_text_as_uuid` GUC (PGC_USERSET, default off) that the studio sets `on` per session: `agg_token`'s output function then emits the underlying provenance UUID instead of `"value (*)"`. The `/api/exec` response carries an `agg_display` map resolved server-side via `provsql.agg_token_value_text(uuid)` so the front-end renders the friendly form while keeping the UUID in `data-circuit-uuid` for the existing click flow.

### Tests

* [x] `tests/test_circuit.py` (9 cases): DISTINCT root → + over 3 inputs; layout always populates x/y; invalid UUID → 400.
* [x] Frontier behaviour: depth-1 envelope is enforced; self-join × gates are flagged as frontier at depth=1; expand returns the new sub-DAG.
* [x] Leaf resolution: a known input UUID maps back to its personnel row; unknown UUID → 404.
* [x] `agg_token` underlying UUID accepted by `/api/circuit/`; the agg root is correctly typed `agg`.

---

## Stage 4: Polish (1 day)

* [x] Loading state for long queries (spinner via swap-after-100ms; Cancel button hidden by default, surfaced once the query is in flight; firing POST `/api/cancel/<id>` resolves the request id to a backend pid and runs `pg_cancel_backend` on a separate connection).
* [x] Error toast styled with the brand: terracotta-tinted `wp-error` block with crimson left rule; `wp-warning` (orange / terracotta) and `wp-notice` (gold) variants. ProvSQL-emitted messages get a "ProvSQL" pill so the source is obvious.
* [x] Empty-result state with a clear message ("(no statements)" / "0 rows" footer).
* [x] Classification pills rendered for `classification` columns matching the known levels (`formatCell` recognises the column name and emits `wp-pill wp-pill--<value>`).
* [x] ⌘ / Ctrl+Enter to submit. Alt+↑ / Alt+↓ steps through the persistent query history.
* [x] Config panel for the panel-managed GUCs (`provsql.active`, `provsql.verbose_level`) plus the studio-level options (`max_circuit_depth`, `statement_timeout`, `search_path`). Persisted to `~/.config/provsql-studio/config.json` so settings survive a restart.

---

## Additional work shipped (still pre-v0.1)

Polish + feature work that landed after the original Stage-5 plan, on the road to the first release. Each bullet links to the user-visible behaviour, not the patch internals.

### Top-nav chrome

* [x] Top-nav DB switcher: clicking the database name lists every accessible DB on the current server; pick one and the page reloads onto it. The querybox is wiped on switch (the previous query rarely makes sense against a different database) with a `pushHistory` first so Alt+↑ recovers it; `ps.sql` / `ps.sql.ran` / `ps.lastRunSql` are cleared so the new mode does NOT auto-replay.
* [x] **Free-form DSN editor under the green/red status dot**: a plug icon + status dot opens a popover where the user pastes a libpq connection string (`host=… port=… user=… password=… dbname=…`). The studio probes the DSN with `SELECT 1` before swapping pools, so a bad host / wrong password leaves the existing connection up and surfaces the PG error in the popover.
* [x] **Launch fallback**: when neither `--dsn` nor `PGDATABASE` / `PGSERVICE` / `DATABASE_URL` is set, the studio defaults to `dbname=postgres` and shows a gold "No database picked at launch" banner with a "Pick a database…" button that opens the DB switcher. Same fallback fires when the user posts a DSN without `dbname`.
* [x] Live connection-status dot (green / terracotta), polled every 5s, with the actual `user@host:port` endpoint in the tooltip.
* [x] Database menu items aligned regardless of which one is current (the checkmark slot is always reserved).
* [x] Mode switcher highlight driven by `<body class="mode-X">` instead of a JS toggle, so it lands on the right tab from the very first paint (no Where→Circuit flash on `/circuit`).

### Query box

* [x] Persistent query history: Alt+↑ / Alt+↓ steps through saved queries (cap 50, deduped, persisted in `localStorage['ps.history']`); the History dropdown lists the same set with a "Clear history" entry. Cursor-skip rule: if the current textarea content already equals the entry the cursor would land on, keep stepping in the same direction so Alt+↑ always produces a visible change (handles both consecutive duplicates and the "draft matches arr[0]" case).
* [x] Mode-switch / DB-switch carry rule: a `runQuery` writes `ps.lastRunSql`; when the user clicks a mode tab or the DB switcher, `carryQueryForSwitch()` writes `ps.sql` (always, to preserve the draft) and `ps.sql.ran=1` only if `lastRunSql === current text`. The new mode restores the textarea unconditionally but auto-replays only when `carriedRan` is true. Plain reloads, history navigation, and unrun drafts therefore never auto-execute (critical for side-effecting queries like `add_provenance`).
* [x] Clear-query button in the SQL editor gutter (eraser icon, under the `SQL` label): wipes the textarea but pushes the current text to history first, so an accidental clear is one Alt+↑ away from recovery.

### Where mode

* [x] Schema panel: a navbar button opens a searchable popover listing every SELECT-able table / view / matview / foreign table, with column lists. Provenance-tracked relations get a brand-purple `prov` pill and a left accent stripe; the `provsql` bookkeeping column is hidden from the column list (its presence is what the pill signals).
* [x] Schema panel: provenance-mapping relations (any table or view shaped `(value <T>, provenance uuid)`, including the view variant from `create_provenance_mapping_view`) get a gold `mapping` pill. The two pills are mutually exclusive — a mapping view that also carries a planner-injected `provsql` column shows only `mapping` (the more specific classification).
* [x] Schema panel: per-relation `+ prov` / `– prov` action chips on plain provenance-eligible tables prefill `SELECT add_provenance(...)` / `SELECT remove_provenance(...)` into the querybox (replacing its content, since these are complete standalone calls). Hidden on views/matviews/foreign tables (the underlying ALTER TABLE rejects them) and on mappings.
* [x] Schema panel: per-column hot-spot on provenance-tracked tables prefills `SELECT create_provenance_mapping('<table>_<col>_mapping', '<schema>.<table>', '<col>');` so the user can mint a mapping in two clicks. Hover tints columns; the comma-separated column list scrolls horizontally on overflow instead of being truncated with an ellipsis.
* [x] Where-mode sidebar: "Input gates only" toggle (default on, persisted in `sessionStorage`) filters provenance-tagged relations to those whose first row's provsql token is an `input` gate — derived materializations (plus / times / agg) are hidden as noise. One extra `provsql.get_gate_type(...)` round-trip per relation, wrapped in a savepoint so probe failures are non-fatal. Empty relations get `first_gate_type=null` and are hidden under the filter; an in-panel hint counts hidden relations and offers an explicit "untick" path when everything is filtered out.
* [x] Search_path locked + displayed: the studio always pins `provsql` at the end of the per-batch search_path (rendered with a lock icon in the header), and surfaces an override field in the Config panel.
* [x] Wrap-fallback notice: a where-mode query that touches no provenance-tracked relation drops the `__prov` / `__wprov` wrap with an INFO banner ("Source relation is not provenance-tracked; …"), instead of erroring out.

### Circuit mode

* [x] Glyph polish on circuit gates: `⊕ ⊗ ⊖ Π 𝟙 𝟘 ⋆ δ ι υ ⋈` for plus / times / monus / project / one / zero / semimod / delta / input / update / eq; agg renders the actual SQL function name (uppercased, with `Σ` shorthand for `SUM`); cmp renders the operator name with `≥ ≤ ≠` substitutions for `>= <= <>`; value renders the actual literal. All non-italic, centred via empirical y-offset tuning.
* [x] Edge child-position labels for non-commutative gates (`cmp`, `monus`, ordered `agg`s), 8px digits drawn just outside the child node along the edge direction. Plus, times, eq, and commutative aggs (sum/count/min/max/avg) stay clean.
* [x] Show UUIDs toggle synced across the result-table UUID cells, the eval-target indicator, the inspector's UUID row, and the "root abc1…" status line. Internal-gate UUIDs stay collapsed (the meta line under each circle is reserved for leaves only).
* [x] Inspector panel rewrite: drops the redundant `type` row (already in the title), translates `info1` / `info2` to gate-specific labels (`function` + `result type` for agg, `operator` for cmp, `left attr` / `right attr` for eq, `value` for mulinput, `relation id` + `columns` for input/update), parses project's `extra` from `{{1,1},{2,3}}` into a bullet list of `input col → output col` lines, and shows a children count.
* [x] Wheel-to-zoom on the canvas (clamped 0.4..2.5).
* [x] viewBox sized to match the canvas aspect ratio (no more letterbox bands inside the bordered rectangle).
* [x] Canvas height bumped to 720px; status line moved into the toolbar row to free vertical space.
* [x] Fullscreen circuit toggle: a button in the toolbar pins the canvas to the viewport, Esc exits. The eval strip pins to the top-left so semiring evaluation works without leaving fullscreen.
* [x] Reset zoom + pan on every new circuit so the whole graph fits at first paint.
* [x] Inspector close button clears the pin (so the Show UUIDs toggle no longer reopens the panel after dismiss).
* [x] DAG-correct `circuit_subgraph`: emits one row per `(parent, node)` edge, so circuits with shared subgraphs no longer drop edges silently. Studio's `_layout` dedups by node id; `evaluate_circuit` works against the full DAG.
* [x] New circuit-mode query wipes the previous circuit (canvas + inspector + drag offsets + status text) and the previous evaluation result, so stale state never sits next to a fresh result table. Exposed as `window.ProvsqlCircuit.clearScene()` for app.js to call from `runQuery`.
* [x] Result-table cells with embedded newlines (typically `provsql.view_circuit`'s ASCII tree dump) render in a scrollable `<pre class="wp-cell-pre">` block — monospaced, `white-space: pre`, slim scrollbars, capped at 80ch × 24rem so wide / tall trees stay legible without blowing the table layout.

### Semiring evaluation (new feature)

* [x] Per-circuit-node Run button supporting `boolexpr`, `boolean`, `counting`, `why`, `formula`, `probability`. Mapping select populated lazily from `/api/provenance_mappings` (discovered as any table / view / matview with `(value, provenance uuid)` columns; default-namespace tables shown unqualified). Method picker for probability with per-method args input (number for `monte-carlo`, dropdown of compilers for `compilation`, free-form text default `0.8;0.2` for `weightmc`).
* [x] Result chip + runtime measurement (similar to the result-table footer) + Hoeffding 95% absolute-error bound for Monte-Carlo runs (`ε = sqrt(ln(40) / 2N)`).
* [x] Clear button next to Run wipes the result so a verbose Why / Formula output doesn't obscure the canvas (especially in fullscreen). Sits in a `cv-eval__btnpair` cluster with the Copy button, immediately adjacent.
* [x] Copy-result button: stashes the just-rendered payload (and for probability, always the FULL precision regardless of how it's displayed) on `result.dataset.copy`; click writes it to the clipboard via `navigator.clipboard.writeText` (with a hidden-textarea + `execCommand` fallback for non-HTTPS dev origins) and flips the icon to a green check for ~1s. Both Clear and Copy use the sphinx-copybutton visual (1.7em framed icon button, 1px border, light-grey background, 0.4em radius, `fa-eraser` and `fa-clipboard` icons; `is-copied` success state uses sphinx's `#22863a` green) so the affordance reads identically across the user docs and Studio.
* [x] Eval-strip layout: dropdowns + hint on row 1; Run + Clear/Copy buttons + result chip + bound + time on row 2 (`.cv-eval__action-row`). Run is always on its own line so the result trails it consistently regardless of how wide the dropdowns get.
* [x] Eval target indicator: `→ root abc1…` (or `→ selected node abc1…` when a node is pinned), tracking whichever the next Run will hit.
* [x] Mapping dropdown filtered by value-type compatibility for custom semirings: each `/api/provenance_mappings` entry now carries the `value` column's `format_type`; for a custom semiring the dropdown shows only mappings whose `value_type` matches the wrapper's `return_type`, with `(no compatible mappings : value type ≠ <T>)` placeholder when none match. Compiled `sr_boolean` / `sr_counting` semirings get an italic hint instead of a hard filter (`Expects boolean values.` / `Expects numeric values.`) since their kernels accept any value type polymorphically. Every option label is type-tagged (`personnel_level (classification_level)`) so the type is visible even when the filter is off.
* [x] PROV-XML export option (under "Other" in the semiring dropdown): calls `provsql.to_provxml(token[, mapping])`. Mapping is optional and the dropdown prepends a `(no mapping : unlabeled tokens)` sentinel; the multi-line XML result renders inside the same scrollable `<pre>` as `view_circuit` cells, with the inline chip background suppressed for `data-kind="xml"`.
* [x] Probability decimals configurable in the Config panel (0–15, default 4, persisted under `localStorage['ps.probDecimals']`); clicking the result chip flips between rounded and full double-precision (`dataset.rounded` / `dataset.full`); cursor + tooltip indicate the affordance. Copy always carries the full-precision form, regardless of which is currently displayed.

### Diagnostics + correctness

* [x] Collapsible NOTICE messages with full `DETAIL` / `HINT` capture: `elog_node_display` parse-tree dumps from `provsql.verbose_level >= 50` are no longer truncated and collapse behind a `<details>` element.
* [x] `statement_timeout` surfaced as a clear "Query canceled: statement timeout reached" message instead of leaking the raw 57014.
* [x] Auto-prepare disabled on the connection pool: `prepare_threshold = None` so `SET LOCAL provsql.where_provenance = on/off` actually reaches the planner across multiple runs of the same query (psycopg3 was caching the first plan after 5 executions, freezing whatever wp setting was active at prepare time).

### Tests

* [x] `tests/test_evaluate.py` (10 tests): `/api/provenance_mappings` discovery + filter, `/api/evaluate` dispatch for each semiring (boolexpr / formula / counting / boolean) and the validation paths (missing mapping, unknown semiring, unknown probability method, invalid UUID).
* [x] `tests/test_relations.py`: schema-panel `has_provenance` flag (positive on personnel, negative on a freshly-created untagged table).
* [x] `tests/test_exec.py`: auto-prepare regression — same SELECT run 16+ times alternating wp on/off, asserting the UUIDs swing between two values (would hang at the wp=on UUID without `prepare_threshold = None`).

### Studio CLI / packaging
* [x] `provsql-studio` console script wired through `pyproject.toml`.
* [x] CLI banner on startup when `--dsn` and PG env are both unset.

---

## Open before v0.1

Coverage gaps and UX work that should land (or at least be triaged) before tagging the first PyPI release.

* [ ] **Online help inside the studio**: `<?>`-style tooltips and per-section "?" links pointing at the relevant chapter of the online doc. Concretely: hover help on the Config panel rows (each GUC), the where-mode wrap notice, the circuit toolbar buttons, the semiring select / methods / args inputs, and the connection editor. The links resolve to `provsql.org/docs/...` anchors rather than re-explaining things in cramped tooltips.
* [x] **Tutorial + case-study coverage audit**: walked tutorial.rst + casestudy{1..5}.rst against the studio. Findings synthesised below as the "ProvSQL feature gaps" punch list and the doc-pacing notes for the studio chapter.
* [ ] **ProvSQL feature gaps** (from audit): triage of extension features that have no studio UI surface. Each row: feature; doc step that wants it; suggested surface.
  * [x] **Custom semirings via `provenance_evaluate(token, mapping, '𝟙', '⊕', '⊗')`** : Tutorial §10 wrappers, CS1 §4 (`security_clearance`), CS2 §6 (`evidence_grade`), CS4 §2 (`union_tstzintervals` is itself a custom semiring built into provsql). Implemented via auto-discovery: `GET /api/custom_semirings` lists `(*, regclass) RETURNS T` SQL/PL functions whose body invokes the bare `provenance_evaluate` (POSIX word-boundary regex `\mprovenance_evaluate\M` in `prosrc` excludes the `sr_*` wrappers, which call `provenance_evaluate_compiled`). Eval-strip dropdown reorganised into three optgroups (Compiled Semirings / Custom Semirings / Other); custom entries encode as `custom:<schema>.<name>`. Server re-runs the discovery filter to validate every payload, so a crafted call can't reach an arbitrary `(uuid, regclass)` user function. Type-aware result rendering (numeric → 4-sig-fig, boolean → pill, otherwise `str()`). Studio coverage: 8 new tests in `tests/test_evaluate.py`.
  * **`shapley` / `banzhaf` / `*_all_vars`**: CS2 §13–15. Add `shapley` and `banzhaf` semirings to the eval strip with a "variable token" picker; the bulk variants belong in a small "per-input contributions" panel (bar chart over variable tokens, mapping-resolved labels).
  * **Temporal SRFs (`timeslice`, `history`, `timetravel`)**: CS4 §3–5. Verbose `... AS (cols ...)` call shape. Suggested: a "Time travel" mini-panel that builds the SRF call from a view picker + date / window / column-filter, or document the recipe and provide a copy-paste snippet.
  * **`undo`**: CS4 §7. Optional one-button "undo last DML" affordance on a result that comes from `update_provenance`.
  * [x] **`to_provxml` export**: CS1 §12. Implemented as a `PROV-XML export` option under the eval strip's "Other" optgroup; mapping is optional (with a `(no mapping : unlabeled tokens)` sentinel); the multi-line XML result renders inside the same scrollable `<pre>` as `view_circuit` cells, with a Copy button next to Clear.
  * **`set_prob`**: every probability case study has a `set_prob(provenance(), col)` setup step. Either document as SQL, or add a row-level "set probability" action when both a `provsql` and a numeric column are visible.
  * [x] **`add_provenance` / `remove_provenance`** (CS5 §5): per-relation `+ prov` / `– prov` action chips in the schema panel prefill the corresponding SELECT call into the querybox. Hidden on views/matviews/foreign tables and on mappings. `repair_key` is still a doc-only recipe.
  * **`expected(COUNT/SUM)`** (CS5 §10): runs as plain SQL today : document only.
  * **Multi-relation provenance mappings + manual `(value, provenance)` tables** (CS5 §2, §5): already discovered by `list_provenance_mappings`; no UI work needed, just call it out in the chapter.
* [ ] **Big-table strategy**: result tables and the relations sidebar currently render every row. For real datasets that breaks both rendering speed and the UI. Pick + implement an approach: server-side `LIMIT` with a "show more" affordance; virtual scrolling; explicit "preview N rows" with a row-count estimate; or refuse to render past a threshold with a guidance message. Same question for the schema-panel relations sidebar in Where mode (currently lists every row of every tagged relation). Audit confirms the urgency: CS3 (Île-de-France GTFS) has ~30k stops + ~250k stop_times, and the Where-mode source-relations sidebar would try to render every row.
* [ ] **Studio-chapter doc-pacing notes** (drop-in for Stage 5's `studio.rst`):
  * Studio pins `provsql` on the search_path automatically: every case study's `SET search_path TO public, provsql;` opener is unnecessary.
  * Setup steps (`psql -d mydb -f setup.sql`) still need psql; the studio has no SQL-file loader. Call this out at the top of the chapter.
  * `view_circuit(...)` ASCII output (Tutorial §11, CS1 §11, CS3 §5) is superseded by the circuit visualizer; redirect explicitly.
  * Programmatic walk via `get_gate_type` / `get_children` / `identify_token` / `get_nb_gates` (CS1 §15) is replaced by clicking around the DAG and reading the inspector panel.
  * `SET provsql.update_provenance = on` (CS4 §6) is the nav toggle, not a SQL command.
  * `where_provenance(provenance())` step-throughs (CS1 §6, CS2 §7–8) become "switch to Where mode and hover".
  * `probability_evaluate` runtime comparisons (CS1 §13) read off the runtime chip on each result and on each eval; no `\timing` needed.
* [x] **`agg_token` cells clickable in circuit mode** (was deferred at the Stage-3 close). Implemented via a new `provsql.aggtoken_text_as_uuid` GUC (PGC_USERSET; default off; flipped on by every studio session) and a companion SQL helper `provsql.agg_token_value_text(uuid)` that recovers the user-facing `"value (*)"` form. The studio's `/api/exec` resolves all distinct agg-cell UUIDs in one batch and ships the map as `agg_display` next to the rows; the front-end renders the friendly form while keeping the UUID in `data-circuit-uuid` for the existing circuit-load handler. Regression test in `test/sql/agg_token_text_as_uuid.sql`; studio coverage in `tests/test_exec.py`.

---

## Stage 5: Documentation (1 day)

Documentation lands BEFORE the v0.1 PyPI / Docker release: the studio chapter, the cross-links, and the screenshots all need to ship in the same window so the public release has a place to send users.

* [ ] New chapter `doc/source/user/studio.rst`. Sections: Installation (pip / Docker), Connecting (DSN / GUCs / read-only-role recipe), Where mode walkthrough, Circuit mode walkthrough (including semiring evaluation), Mode-switching, Limitations.
* [ ] Screenshots for `doc/source/user/studio.rst` (ported from the original Stage-5 polish list, since they belong with the docs chapter rather than the polish work).
* [ ] Add to `doc/source/index.rst` `:caption: User Guide` toctree, between `export` and `configuration`.
* [ ] Cross-link from `where-provenance.rst` ("see the Studio for an interactive view") and `export.rst` (next to `view_circuit`).
* [ ] Run `make docs` after every edit to `.rst` (per project convention).
* [ ] Update top-level `README.md` to mention Studio under "Demos" or "Tools".
* [ ] Update `website/_data/demos.yml` if there's a corresponding entry.

---

## Stage 6: Distribution (1 day)

Last step: the v0.1 release itself. Gated on Stage 5 being done so the published artifact actually has docs to point at.

### PyPI

* [ ] Confirm the `provsql-studio` name is available on PyPI before the first release.
* [ ] Finalize `pyproject.toml`: name `provsql-studio`, version `0.1.0`, MIT license (matches the upstream `LICENSE`), classifiers, project URLs.
* [ ] `README.md`: install, run, screenshots, "this is a UI for ProvSQL, you still need the extension installed".
* [ ] `studio/CHANGELOG.md`: separate from the top-level extension `CHANGELOG.md` since version streams are independent.
* [ ] Build with `python -m build`, smoke-test in a fresh venv against a local PG with ProvSQL.
* [ ] First release: tag `studio-v0.1.0`, upload to PyPI.

### Docker

* [ ] Add Python + studio install to `docker/Dockerfile`. Keep `apache2 / php-pgsql / graphviz / libgraph-easy-perl` for now; remove PHP only after Studio is stable.
* [ ] Update `docker/demo.sh`: launch `provsql-studio --host 0.0.0.0 --port 8000` in addition to (or in place of) Apache. Print the studio URL alongside the psql info.
* [ ] Local smoke test: `docker build -t provsql-demo docker/ && docker run --rm -p 8000:8000 provsql-demo` and confirm `/where` and `/circuit` load against the test database.

### Source tree integration

* [ ] Top-level `Makefile` target: `make studio` → `python -m provsql_studio` (assumes user has activated a venv with the package installed; document this).
* [ ] `make studio-install` → `pip install -e ./studio` for contributors.

### Cleanup

* [ ] Remove `where_panel/` once the Docker image and docs are switched over to Studio.
* [ ] Update `docker/demo.sh` to drop the `cp -r /opt/provsql/where_panel/* /var/www/html/` block.
* [ ] Search the docs for references to `where_panel` and update.
* [x] `studio/` added to `.gitattributes` `export-ignore` so PGXN / `git archive` extension release tarballs don't carry it.

---

## Continuous integration

Studio shares the repo with the extension and the docs, each of which already has its own GitHub Actions workflow (`.github/workflows/{build_and_test, codeql, docs, macos, pgxn, wsl}.yml`). Studio adds two CI concerns: the SQL helpers ride on the existing extension test pipeline, and the Python package needs its own pipeline.

### Existing workflows: what to update

A studio-only commit triggers only the new `studio.yml`, never the extension / docs / macOS / WSL builds. Each `paths-ignore` block also lists `.github/workflows/studio.yml` so editing that file in isolation doesn't cascade.

* [x] **`build_and_test.yml`**: `studio/**` and `.github/workflows/studio.yml` added to `paths-ignore` in both `push` and `pull_request` blocks. SQL-helper changes in `sql/provsql.common.sql` and `test/sql/` still trigger the regression suite.
* [x] **`docs.yml`**: same. `doc/source/user/studio.rst` still triggers the docs build (only studio code edits are ignored).
* [x] **`macos.yml`**: same.
* [x] **`wsl.yml`**: same.
* [x] **`codeql.yml`**: uses an allowlist (`paths: ['src/*', '.github/workflows/codeql.yml']`); studio is excluded by construction. No change made.
* [x] **`pgxn.yml`**: triggered only on `v[0-9]+.[0-9]+.[0-9]+` tags (extension versions); studio releases use `studio-v*` tags. No overlap, no change made.
* [ ] When `where_panel/` is deleted (Stage 4 cleanup), remove `where_panel/**` from `docs.yml`'s `paths-ignore`.

### New workflow: `.github/workflows/studio.yml`

* [ ] **Trigger**: `push` / `pull_request` on changes under `studio/**`, `sql/provsql.common.sql`, `sql/provsql.14.sql`, or this workflow file. Also `workflow_dispatch`.
* [ ] **Matrix**: Python 3.10 / 3.11 / 3.12 / 3.13. PostgreSQL 14 / 15 / 16 (mirror what `build_and_test.yml` covers).
* [ ] **Job: build extension + run studio tests**:
  * Service: `postgres:<version>` container with `provsql` installed (re-use the install steps from `build_and_test.yml`, factoring into a composite action if it gets repetitive).
  * Set up Python, `pip install -e ./studio[test]`.
  * Run `test/sql/{setup,add_provenance,security}.sql` against the test database (so `personnel` exists for the studio tests).
  * `pytest studio/tests`.
* [ ] **Job: lint + format**:
  * `ruff check studio/`, `ruff format --check studio/`.
  * Optional: `mypy studio/provsql_studio/` once the codebase is non-trivial.
* [ ] **Job: package build smoke test**:
  * `python -m build studio/`.
  * `pip install studio/dist/*.whl` in a fresh venv; `provsql-studio --help` should exit 0.

### Release workflow: `.github/workflows/studio-release.yml`

* [ ] Trigger on tags matching `studio-v*`.
* [ ] Run the full studio test job first.
* [ ] Build sdist + wheel.
* [ ] Publish to PyPI via `pypa/gh-action-pypi-publish` using a Trusted Publisher (no API token in repo secrets).
* [ ] Create a GitHub release attaching the artifacts.

### Docker workflow

* [ ] Decide: extend the existing Docker workflow (if any: check whether one exists; if not, the demo image is built ad-hoc) to publish a `provsql-demo:latest` image to GHCR on each release. Out of scope for v1 if the image is currently built by hand.

---

## Future work (v2 and beyond)

Ideas raised during design + planning that are out of scope for v1 but worth keeping on the radar. Triage notes (after the v0.1 development push) split the list into "already shipped (in part)", "could land earlier", and "genuinely v2-shaped".

The first is structural: the where-panel and circuit-visualizer are two **modes** of one tool that share the chrome (textarea, send-query, result rendering, mode switcher). Each mode adds its own sidebar and its own per-cell click affordance. New modes plug into the same pattern.

### Already shipped (move out of v2)

* [x] **Per-circuit-node semiring evaluation** (the heart of "evaluation mode"): the circuit-mode sidebar exposes a Run button supporting `boolexpr`, `boolean`, `counting`, `why`, `formula`, and `probability` (with method picker, per-method args, runtime, and a Hoeffding 95% bound for Monte-Carlo). It targets the pinned node or the root, and shares the same / `api/evaluate` endpoint that a future `/eval` mode would reuse. What remains v2 (see below) is the per-row column extension and the heat-map drill-down.
* [x] **Mode-agnostic API design** ("what's generic"): `/api/exec` takes a `mode` field instead of having mode-specific routes; the wrapping logic lives in `db.exec_batch` behind a `wrap_last` flag rather than inside the route; the result renderer keys off `column.type_name` for click handlers (uuid + agg_token). Adding a new mode means a new sidebar template + a new endpoint, as planned.

### Could land earlier (candidates for v0.2)

* [ ] **Save / load notebooks** — partial: query history is already persisted (sessionStorage `ps.sql` carry-over + the history dropdown). The remaining work is small: a "Download .sql" button next to the history dropdown that exports the recent buffer, and a file-picker that imports back into the textarea. ~30 lines.
* [ ] **Lazy expansion sizing**: a one-line GUC default tweak (`max_circuit_depth`). Current default 8 was sized for tutorial circuits; once we collect real-world depth data (Stage 5 documentation walkthroughs would surface this), drop to 4 with a "Show more" affordance via the existing frontier-expand path.
* [x] **Drag-to-move circuit nodes**: each `.node-group` is a `<g transform="translate(x,y)">`; a `mousedown` handler on the group seeds a window-level move/up gesture, client deltas convert to SVG coords via `svg.getScreenCTM().inverse()`, and `state.dragOffsets[uuid]` accumulates per-node offsets that `paintEdges()` and the node transform read on every paint. Click-vs-drag uses a 4px movement threshold (the post-mouseup click event is suppressed via a per-group flag once the threshold is crossed). Offsets are keyed by uuid and reapplied on every paint, so frontier-expansion preserves manual nudges; they reset on `renderCircuit()` (new circuit). A "Reset layout" toolbar button (↶) wipes offsets in one click.

### Genuinely v2-shaped

* **Result-table evaluation extension**: the missing half of "evaluation mode": run the chosen semiring across every row of the current result and add a column with the per-row value. Today the eval strip evaluates one node at a time; this would batch-evaluate all UUIDs in the displayed `provsql` column.
* **Shapley / Banzhaf heat-map**: overlay per-gate contribution values on the circuit visualizer. ProvSQL already exposes `shapley_all_vars` / `banzhaf_all_vars`; the visualizer would consume those and colour-tint each input gate. Natural fit as a drill-down click on a result row from the result-table evaluation extension.
* **Knowledge-compilation view**: render the d-DNNF compiled from a circuit, not just the raw provenance DAG. Would surface what `provenance_evaluate_compiled` actually consumes and make probability evaluation legible. Could ship as a sub-mode toggled from the circuit-mode toolbar (`Π`-shaped circuit ↔ d-DNNF view).
* **Formula simplification**: collapse semantically-equivalent subgraphs (e.g. shared `times` over identical inputs) for readability. Possibly a server-side pass before layout.
* **Tweaks panel** for where mode: theme toggle (light only for v1), table density (comfortable / compact), highlight colour (terracotta / gold / purple), show/hide classification pills. Cheap, demonstrates brand flexibility.
* **Multi-user demo deployment**: per-browser-session isolation in a single Docker container, so a conference audience can each `localhost:8000` against a hosted instance.

---

## Decisions

(All open questions raised during planning have been resolved. Future deliberations get appended here as they come up.)


* **Versioning**: **independent stream**. Studio releases as `studio-v0.1.0`, `studio-v0.2.0`, ... on PyPI; extension releases stay on `v1.x`. Compatibility surfaced via a startup check (`SELECT extversion FROM pg_extension WHERE extname = 'provsql'`) that warns or refuses to start if the installed extension is older than studio's minimum requirement, plus a compatibility matrix in `doc/source/user/studio.rst`.
* **Docker swap-over**: **replace `where_panel/` immediately** when Studio ships. The Docker image stops bundling Apache + PHP, `docker/demo.sh` drops the `cp -r .../where_panel/* /var/www/html/` block, the demo URL changes from `http://<container-ip>/` to `http://<container-ip>:8000/where`. The old PHP version's "SQL injection" framing was a red herring: the user is the SQL author by design, and the demo container is the user's own sandbox, not a privileged service.
* **Studio version in Docker**: **hybrid via build arg**. Dockerfile defaults to `ARG STUDIO_VERSION=<latest-released>` and `pip install provsql-studio==${STUDIO_VERSION}`; a `--build-arg STUDIO_SOURCE=/opt/provsql/studio` override switches to `pip install -e ${STUDIO_SOURCE}` for contributors hacking locally. Released images carry a known-good studio release; first release of either component is bootstrapped by a one-time source-tree build.
* **Multi-statement handling**: **only the last statement's result is displayed**. The user's input is split with `sqlparse` (handles dollar-quoting, comments, string literals); all but the last statement are sent verbatim and their results discarded (errors halt the batch). Only the last statement is potentially wrapped: in where mode, if it is a `SELECT` (or a `WITH ... SELECT`), it is wrapped in `select *, provsql.provenance() AS __prov, provsql.where_provenance(provsql.provenance()) AS __wprov from (<last>) t`; otherwise (DDL, DML, `EXPLAIN`, `CREATE TABLE AS`, ...) it runs unmodified and its status / returned rows are shown verbatim. Edge cases like `CREATE TABLE AS SELECT` are simply not wrapped; the user re-runs `SELECT * FROM <new_table>` to inspect with where-provenance.

---

## References

* Design bundle: vendored under `studio/design/`.
* Original Where Panel: `where_panel/index.php` (to be removed once Studio ships).
* Brand assets: `branding/{logo.png, favicon.ico, fonts/*.woff2, fonts-face.css}`.
* Existing circuit-introspection: `sub_circuit_with_desc` (sql/provsql.common.sql:888), `sub_circuit_for_where` (line 956), `view_circuit` (line 1464).
* Demo data: `test/sql/{setup,add_provenance,security}.sql` (`personnel` table with classifications).
* Existing Docker integration: `docker/Dockerfile`, `docker/demo.sh`.
