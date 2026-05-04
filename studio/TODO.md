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
* [x] Replace the bundle's local `@font-face` block (Latin only) by loading the upstream `branding/fonts-face.css` (Greek + Latin-extended) — symlinked as `static/fonts-face.css` and loaded directly from `index.html`. Also added `--font-body` / `--font-display` / `--font-ui` aliases since panel.css and circuit.css reference those names but `colors_and_type.css` only defined `--font-serif` / `--font-sans` / `--font-mono`.
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
* [x] Cap recursion at `max_depth`; record `depth` per node so the front-end can identify frontier nodes for lazy expansion.
* [x] Dedup: each node appears once even if reachable via multiple paths (DAG, not tree). Pick a deterministic parent (lowest BFS depth, then lowest `child_pos`).
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

* [ ] `pyproject.toml`: declare deps (`flask`, `psycopg[binary]>=3.1`, `sqlparse`, `pydot` or just shell out to `dot`); optional-dependencies `test = ["pytest", "pytest-flask"]` and `dev = ["ruff", "mypy"]`; license matching the upstream extension; project URLs (homepage, source, docs). Console script: `provsql-studio = provsql_studio.cli:main`.
* [ ] `cli.py`: argparse, default `--host 127.0.0.1 --port 8000`, builds DSN from `--dsn` or `PGHOST` / `PGUSER` / etc. env vars (psycopg's default behaviour).
* [ ] `db.py`: `psycopg_pool.ConnectionPool` (or simple `psycopg.connect` per request for v1). Helper that runs a SQL string with `SET LOCAL statement_timeout` and yields per-statement results via `cur.nextset()`.
* [ ] `app.py`: Flask app with the routes below.

### Routes

The two routes share the same shell HTML and JS bundle; they differ in which sidebar component is rendered, in the default value of `provsql.where_provenance` for the request, and in whether `/api/exec` wraps the user query.

* [ ] `GET /` – redirect to `/where` (the more guided of the two modes).
* [ ] `GET /where` – serves the shared shell with `mode=where`. Sidebar = source-relations.
* [ ] `GET /circuit` – serves the shared shell with `mode=circuit`. Sidebar = circuit DAG (empty until the user clicks a UUID / `agg_token` cell in the result).
* [ ] `GET /api/relations` – list provenance-tagged relations with column names + rows. Port the `pg_attribute` query from `where_panel/index.php:100-145`. Used by Where mode only.
* [ ] `POST /api/exec` – body `{sql: string, mode: "where"|"circuit"}`. The user's SQL may contain multiple statements (DDL / DML / SELECT) separated by `;`. **Only the last statement's result is displayed.** Server uses `sqlparse` to split (handles dollar-quoting, comments, string literals); all but the last statement run verbatim with results discarded (errors halt the batch). The last statement is potentially wrapped:

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
* [ ] `GET /api/config` – return current GUC values (`provsql.active`, `provsql.where_provenance`, `provsql.update_provenance`, `provsql.verbose_level`).
* [ ] `POST /api/config` – body `{key, value}`. Whitelist the four GUCs, run `SET` (per-request; client re-sends, simpler than pinning a connection).

### Front-end

The shared shell from Stage 0 (one `index.html`, `app.css`, `app.js`, `circuit.js`) is the starting point. This stage wires up the chrome (top nav, mode switcher, query form, result rendering) and the Where-mode sidebar. The Circuit-mode sidebar is stubbed here and filled in in Stage 3.

* [ ] Mode switcher in the top nav: two-segment toggle ("Where" / "Circuit"). Switching mode changes the sidebar component, then re-runs the current query (the wrapping differs).
* [ ] Shared query form + result renderer:
  * On submit: `POST /api/exec` with `{sql, mode}`, render each result block.
  * Result rendering helper: takes the `[{kind, ...}]` array and produces a sequence of blocks (table for rows, status banner for DDL / DML, error banner for failures). Match the design's `wp-table--result` styling.
  * After every successful exec: re-fetch relations (where mode only) so `add_provenance` results show up live.
  * ⌘ / Ctrl+Enter to submit. Loading skeleton. Empty-result state ("0 rows").
* [ ] **Where mode** (sidebar = source relations):
  * On mode entry: `GET /api/relations`, render each tagged relation into `#sidebar` as a table with `<td id="<relation>:<provsql_uuid>:<col>">`. Replace the bundle's hardcoded `personnel` markup with a generic loop.
  * Hover on result cell: walk the cell's `data-sources` list, toggle `is-source` on the matching `<td>` in the relation tables.
  * `data-sources` is built from the wrapped query's `where_provenance(provenance())` column. The wrapping happens server-side in `/api/exec`; client parses the array and attaches per-cell ids.
* [ ] **Circuit mode** (sidebar = circuit DAG):
  * Result table: cells whose column type is `uuid` or `agg_token` get a clickable affordance.
  * Click on such a cell: lazy-import `circuit.js`, `GET /api/circuit/<value>`, render the DAG into `#sidebar`.
  * No source-relation panel is fetched.
  * Stage 3 covers the actual DAG rendering and lazy expansion.

### Tests

* [ ] `tests/conftest.py` spins up an isolated PG database (or relies on `PGDATABASE` set by CI), installs the extension, runs `test/sql/{setup,add_provenance,security}.sql`.
* [ ] `tests/test_exec.py`: SELECT returns rows; CREATE returns status; syntax error returns error block; multi-statement input returns the LAST statement's result (prior statuses discarded); a syntax error in a non-final statement halts the batch and is surfaced; the where-mode wrapping correctly skips a non-SELECT last statement (e.g. `INSERT INTO ... RETURNING ...` runs raw); `WITH ... SELECT` is recognized as wrappable; dollar-quoted function bodies in earlier statements don't confuse the splitter.
* [ ] `tests/test_relations.py`: `personnel` is listed, columns are right, after a fresh `add_provenance('foo')` the relation appears on the next call.

---

## Stage 3: Circuit mode + cross-mode navigation (2 days)

### Server side

* [ ] `circuit.py`: given a root UUID and depth, call `provsql.circuit_subgraph`, build a Graphviz DOT string, run `dot -Tjson` (subprocess), parse the JSON to extract per-node coordinates, return `{nodes: [{id, type, label, info1, info2, depth, x, y, frontier: bool}], edges: [{from, to, child_pos}]}`.
  * Frontier flag: a node is a frontier if `depth == max_depth` and it has un-fetched children (check `get_children` length > number of children fetched).
  * Cap at `--max-circuit-nodes` (default 500); if exceeded, return a 413 with `{error: "circuit too large", hint: "reduce depth or expand interactively"}`.
* [ ] Cache layouts in-process keyed on `(root_uuid, depth)` since Graphviz is not free.
* [ ] `GET /api/circuit/<uuid>?depth=<N>` – initial render. Accepts both UUID and `agg_token` (the latter is cast to its underlying UUID server-side via `agg_token_uuid`).
* [ ] `POST /api/circuit/<uuid>/expand` – body `{frontier_node_uuid, additional_depth}` – fetches another layer and merges into the previous response. (Or stateless: client sends the set of UUIDs already known and the server returns only the new ones.)
* [ ] `GET /api/leaf/<uuid>` – calls `provsql.resolve_input(uuid)`. Returns `{relation, row_data}` or 404.

### Front-end (circuit-mode sidebar, from `static/circuit.js`)

* [ ] Strip the bundled `QUERIES` mock data; the sidebar starts empty until a UUID / `agg_token` cell is clicked in the result.
* [ ] On click of a typed cell: `GET /api/circuit/<value>`, render the DAG in the sidebar.
* [ ] Replace the bundle's centroid layout with the server-supplied `{x, y}` per node; the local `circuit.js` only paints + handles interactions.
* [ ] Click a frontier node: `POST /api/circuit/.../expand`, merge into the existing scene, re-paint.
* [ ] Inspector panel: when a leaf is pinned, fire `GET /api/leaf/<uuid>` and show the resolved `(relation, row)`.
* [ ] Toolbar: zoom / fit / UUID toggle / formula-strip toggle (already in the design, just keep working).

### Cross-mode navigation

* [ ] Mode switcher in the top nav (Stage 2): clicking it preserves the current SQL textarea content and re-runs in the new mode.
* [ ] In where-mode result, each row gets a small "→ Circuit" button that switches to circuit mode and pre-loads the circuit for that row's provenance UUID. (The wrapped query has the UUID; the button passes it directly to `/api/circuit/<uuid>`.)
* [ ] In circuit-mode result, when no UUID / `agg_token` cells are present, surface a hint: "switch to Where mode to see source-cell highlights for this query".

### Tests

* [ ] `tests/test_circuit.py`: a known query → expected node count and edge structure for a small circuit.
* [ ] Frontier behaviour: depth=1 returns root + 1 layer; expand returns next layer.
* [ ] Leaf resolution: a known input UUID maps back to its `personnel` row.
* [ ] `agg_token` accepted as input to `/api/circuit/`.

---

## Stage 4: Distribution (1 day)

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

## Stage 5: Polish (1 day)

* [ ] Loading state for long queries (spinner + cancel button).
* [ ] Error toast styled with the brand (red border, terracotta-500 accent).
* [ ] Empty-result state with a clear message.
* [ ] Classification pills rendered for `classification` columns when value is one of the known levels (the design already has the styles, just hook them up generically: any enum column with values matching the `_unclassified|_restricted|...` pattern, or a hardcoded list to start).
* [ ] ⌘ / Ctrl+Enter to submit (already in Stage 2; verify on macOS + Linux + Windows browsers).
* [ ] Settings panel for the four GUCs, exposed from the navbar's "Config" link (currently a `#` placeholder in the design).
* [ ] Screenshots for `doc/source/user/studio.rst` (see below).

---

## Documentation

* [ ] New chapter `doc/source/user/studio.rst`. Sections: Installation (pip / Docker), Connecting (DSN / GUCs / read-only-role recipe), Where mode walkthrough, Circuit mode walkthrough, Mode-switching, Limitations.
* [ ] Add to `doc/source/index.rst` `:caption: User Guide` toctree, between `export` and `configuration`.
* [ ] Cross-link from `where-provenance.rst` ("see the Studio for an interactive view") and `export.rst` (next to `view_circuit`).
* [ ] Run `make docs` after every edit to `.rst` (per project convention).
* [ ] Update top-level `README.md` to mention Studio under "Demos" or "Tools".
* [ ] Update `website/_data/demos.yml` if there's a corresponding entry.

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

Ideas raised during design + planning that are out of scope for v1 but worth keeping on the radar.

The first is structural: the where-panel and circuit-visualizer are two **modes** of one tool that share the chrome (textarea, send-query, result rendering, mode switcher). Each mode adds its own sidebar and its own per-cell click affordance. New modes plug into the same pattern.

* **Evaluation mode** (`/eval` or similar): a third mode where the user picks a semiring / probability / Shapley / Banzhaf evaluation and the result table grows extra columns showing the per-row evaluation. Concretely:
  * Sidebar: evaluation picker (semiring registry, probability method, Shapley vs Banzhaf), plus configuration (probability mapping table, semiring witness mapping, Monte-Carlo iterations).
  * Per-cell click on a UUID / `agg_token`: drill in to see the per-gate contribution (e.g. Shapley values per leaf, lit up on the circuit DAG: the heat-map idea).
  * Server side: thin wrappers around `probability_evaluate`, `shapley_all_vars`, `banzhaf_all_vars`, `provenance_evaluate`.
  * Cross-mode: the mode switcher carries the query forward; eval-mode results become a numerical companion to the where / circuit views.
* **What's generic** (so v1 doesn't paint future modes into a corner): keep `/api/exec` mode-agnostic (the wrapping logic lives in a per-mode helper, not hardcoded in the route); keep the result-renderer mode-aware only via the column-type → click-handler mapping; keep the sidebar a swappable component with its own state. Adding a new mode in v2 should mean: one new sidebar template, one new column-type handler, one new endpoint per evaluation kind.
* **Knowledge-compilation view**: render the d-DNNF compiled from a circuit, not just the raw provenance DAG. Would surface what `provenance_evaluate_compiled` actually consumes and make probability evaluation legible. Could ship as part of evaluation mode, or as a separate sub-mode.
* **Shapley / Banzhaf heat-map**: overlay per-gate contribution values on the circuit visualizer. ProvSQL already exposes `shapley_all_vars` / `banzhaf_all_vars`; the visualizer would consume those. Natural fit as evaluation-mode → drill-down on the circuit DAG.
* **Formula simplification**: collapse semantically-equivalent subgraphs (e.g. shared `times` over identical inputs) for readability. Possibly a server-side pass before layout.
* **Tweaks panel** for where mode: theme toggle (light only for v1), table density (comfortable / compact), highlight colour (terracotta / gold / purple), show/hide classification pills. Cheap, demonstrates brand flexibility.
* **Save / load notebooks**: persist the textarea contents + result history as a downloadable `.sql` or JSON file.
* **Lazy expansion sizing**: the chat suggested `depth=4` as a default for very large circuits; TODO currently uses 8 because demo circuits are small. Revisit once we have real data on circuit sizes seen in the wild.
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
