# Studio: notebook mode

Plan for a third Studio mode, **Notebook**, à la Jupyter: an ordered
list of editable cells (Markdown prose + SQL) with their results
rendered inline and persisted, executed against a *stateful* database
session, saveable to a file and reloadable. Anchored on the existing
mode architecture (`studio/provsql_studio/app.py` routes,
`db.exec_batch`, the `app.js` mode switcher and result renderer, the
`circuit.js` canvas) and on the Jupyter nbformat v4 specification for
the on-disk format. Supersedes the “Notebooks (small)” entry of
[`studio.md`](studio.md) (the export half folds into this plan).

The pitch: the tutorial and case studies are *narratives* -- prose
interleaved with queries whose outputs feed the next step. Today they
live in Sphinx pages the user copies from, one statement at a time,
into a stateless query box. A notebook mode makes them executable
documents inside Studio itself, and gives users the same vehicle for
their own provenance analyses: an inspectable, re-runnable,
shareable record of a session.

## Out of scope

- **Non-SQL code cells** (Python, JS): Studio is a SQL surface; users
  wanting Python around ProvSQL should use actual Jupyter with
  psycopg. Revisit only if a concrete need appears.
- **Collaborative / multi-user editing** and server-side notebook
  hosting for third parties: single-user tool, same trust model as
  today. (The multi-user demo deployment of `studio.md` is a separate
  concern and composes with this one.)
- **Plotting / charting cells** beyond what already exists
  (`rv_histogram` returns jsonb today and renders as a value; a chart
  renderer is a possible later increment, noted under Priorities, not
  scoped here).
- **Cell-to-cell variable templating** (psql-style `\set` /
  `:variable` interpolation): powerful but orthogonal; the kernel’s
  session state (temp tables, GUCs) already covers most sequencing
  needs. Listed as a possible follow-up.
- **Scheduling / headless execution** (papermill-style): out of
  scope; `psql -f` covers batch needs.

## The mode in one paragraph

A third tab in the mode switcher (`Where | Circuit | Notebook`,
route `/notebook`). The right-hand panel’s query box + result table
is replaced by a vertical cell list; the left sidebar keeps the
provenance-tagged-relations browser (shared with Where mode) topped
by a notebook outline (cell index from Markdown headings). Each cell
is either **Markdown** (rendered in place, double-click to edit) or
**SQL** (the existing highlight-overlay editor, shrunk to cell size,
with the cell’s result blocks rendered underneath exactly as the
shared result renderer draws them today: rows, status, error,
NOTICE pills, provenance UUID cells with their click affordances).
Cells run individually (Ctrl+Enter / Shift+Enter), in sequence (Run
all / Run from here), against a **pinned database session** (the
“kernel”) whose state -- temp tables, session GUCs, prepared
statements -- persists across cells until the user restarts it. The
notebook saves to a Jupyter-nbformat-compatible `.ipynb` file
(download / load, like the query-box `.sql` button) and re-renders
saved outputs on load without re-executing.

## Plan

### 1. Cell model and UI

- **Cell types**: `markdown` and `sql` at minimum. Two
  provenance-native cell types ride on top of `sql` (see §4):
  **circuit cells** (a pinned circuit-canvas snapshot for a token)
  and **evaluation cells** (an eval-strip invocation bound to a
  token). Internally every cell is `{id, type, source, options,
  outputs[], execution_count}`.
- **Cell chrome**: drag handle / move-up / move-down, insert-above /
  insert-below, delete (with undo toast), type selector, collapse
  output, clear output. A thin execution-state gutter:
  `[ ]` never run, `[*]` running, `[n]` execution counter -- the
  Jupyter idiom, so out-of-order execution is visible.
- **Keyboard**: Ctrl+Enter (run cell), Shift+Enter (run + advance /
  create next), Alt+Enter (run + insert below), Esc/Enter for
  command vs edit focus modes only if cheap -- the full Jupyter
  command-mode key map is *not* a goal.
- **Toolbar** (replaces the Send-query button row): Run, Run all,
  Run from here, Interrupt (wired to the existing `/api/cancel`
  machinery), Restart kernel, Add cell, Save (download), Load,
  Export HTML. The provenance-scheme selector moves into per-cell
  options with a notebook-level default (§3).
- **Sidebar**: the relations browser as in Where mode (it is the
  natural companion while writing queries), plus an outline built
  from Markdown headings for long notebooks.
- **Markdown rendering**: vendor a renderer + sanitizer (e.g.
  `marked` + `DOMPurify`, both MIT). Self-hosting is mandatory: the
  Playground build asserts zero off-origin requests, so the
  libraries are vendored into `static/vendor/` and added to
  `build.sh`’s closure and `THIRD-PARTY.html`. All rendered HTML is
  sanitized; raw HTML pass-through stays off.

### 2. Execution model: the kernel

The central design decision. Today every `/api/exec` borrows a pool
connection, runs the batch in one transaction with `SET LOCAL`s, and
returns it; *no session state survives between requests* (that is why
`exec_batch` re-applies every GUC each time). A notebook needs the
opposite: `CREATE TEMP TABLE` in cell 2 must be visible in cell 5,
like a Jupyter kernel’s interpreter state.

- **Pinned connection**: a notebook session pins one dedicated
  psycopg connection outside the pool, created by
  `POST /api/nb/session` (returns `{session_id}`; registers the
  backend pid so Interrupt can `pg_cancel_backend` it via the
  existing inflight registry). `DELETE /api/nb/session/<id>` closes
  it; **Restart kernel** = delete + create (+ clears all execution
  counters). Sessions are tracked in an `app.extensions` registry
  with a lock, like `provsql_inflight`.
- **Per-cell transaction**: each cell executes inside
  `with conn.transaction():`, preserving `exec_batch`’s current
  semantics verbatim within a cell (the `SET LOCAL` prelude, the
  classifier savepoint, the where-wrap, the COPY-block path, the
  multi-statement halt-on-first-error). Across cells, committed
  state and session-scoped objects persist; aborted cells roll back
  cleanly. This is the psql default (autocommit per statement is
  *not* mimicked; one cell = one transaction is easier to reason
  about and matches what `/api/exec` does today for a batch).
- **Refactor**: split `db.exec_batch(pool, …)` into a thin
  pool-acquiring wrapper around `db.exec_batch_on(conn, …)` that
  does the actual work; the notebook path calls `exec_batch_on`
  with the pinned connection. The ACTIVE-connection safety net
  moves into `exec_batch_on`’s caller: for a kernel, a wedged
  connection closes the *kernel* and surfaces a “kernel died,
  restart it” error block instead of silently swapping connections.
- **Lifecycle / limits**: idle timeout (default ~30 min, config
  option) closes abandoned kernels; a small cap on concurrent
  kernels (default ~4) since each holds a PostgreSQL backend.
  Closing the browser tab fires a best-effort
  `navigator.sendBeacon` delete. A dead kernel (server restart, PG
  restart, timeout) is detected on the next cell run and offered a
  one-click restart; outputs are kept.
- **Database switch**: a kernel is bound to one database. Switching
  the connection (connection chip) while a notebook is open
  restarts the kernel after a confirm.
- **GUC panel interplay**: config-panel GUCs keep applying per cell
  via the existing `SET LOCAL` prelude -- unchanged semantics, no
  drift between modes. Anything the user `SET`s manually in a cell
  persists for the kernel’s lifetime, which is exactly the
  Jupyter-like behaviour wanted.

### 3. Cell options and the provenance scheme

- Notebook-level defaults (stored in notebook metadata): provenance
  scheme (`where` / `semiring` / `boolean`), max result rows.
- Per-cell overrides (stored in cell metadata, surfaced as a small
  gear popover): scheme, “render where-provenance highlights”
  (applies the Where-mode wrap to this cell’s final SELECT),
  row cap. The `wrap_last` decision thus becomes per cell; the
  `prov_scheme` plumbing in `api_exec` already supports all three
  values per request, so this is mostly front-end.
- The existing “last statement gets wrapped / rendered” rule applies
  per cell; intermediate statements behave as in `/api/exec` today.

### 4. Provenance-native cells (the differentiator)

Generic SQL notebooks exist; the reason this lives in Studio is that
cell outputs participate in the provenance UI:

- **Result tables render with full affordances**: provsql / agg_token
  UUID cells keep their short/full display, the `agg_display`
  resolution, the TID/BID/OPAQUE classifier pill, NOTICE pills.
- **“→ Circuit” from a result row** inserts (or updates) a **circuit
  cell** below the current cell: a self-contained snapshot of the
  circuit canvas for that token (`/api/circuit/<uuid>` scene,
  rendered by the same `circuit.js` code, with depth/expand
  controls). The cell records `{token, depth}` in metadata and an
  SVG snapshot in outputs, so a *saved* notebook shows the DAG
  without a live database.
- **Evaluation cells**: the eval strip as a cell -- token +
  semiring/probability method + arguments, with the result inline.
  Records the full invocation in metadata, so re-running the
  notebook re-evaluates. This is the natural narrative form of
  “and the probability of this answer is …” in the tutorial.
- Both special cells re-run against the kernel like SQL cells and
  degrade gracefully when their token no longer exists (clear error
  block, keep stale snapshot collapsed).

### 5. On-disk format: nbformat-compatible `.ipynb`

Adopt **Jupyter nbformat v4** rather than inventing a format:

- `nbformat: 4`, `kernelspec: {name: "provsql-studio", language:
  "sql"}`, `language_info: {name: "sql"}`. Markdown cells are plain
  nbformat markdown cells. SQL cells are code cells whose `source`
  is the SQL text; Studio-specific bits (scheme override, circuit /
  evaluation cell descriptors, collapse state) live under
  `metadata.provsql` -- valid nbformat, ignored by other tools.
- **Outputs** are `execute_result` entries with a dual mime bundle:
  `application/vnd.provsql.blocks+json` (the verbatim `/api/exec`
  blocks payload: the single source of truth Studio re-renders
  from) and `text/html` (a static rendering for nbviewer / GitHub /
  JupyterLab display). Circuit cells add `image/svg+xml`.
  On load Studio re-renders **only** from the JSON payload through
  the existing escaping renderer -- the `text/html` bundle is never
  injected back into the DOM, so a tampered notebook file cannot
  stored-XSS the app.
- Payoff of nbformat compatibility: saved notebooks render on
  GitHub/nbviewer out of the box, version-control friendly, and a
  future real Jupyter SQL kernel could open them.
- **Save / load** mirror the query-box buttons: client-side download
  and file-picker upload; no server round-trip. “Include outputs”
  toggle on save (stripped outputs for clean VCS diffs). Autosave of
  the working notebook to `localStorage` (same channel family as
  `ps.sql`) so a reload does not lose work; an explicit “Discard
  draft” escape hatch.
- **Export HTML**: a single self-contained static page (inline CSS,
  the `text/html` bundles, the SVG snapshots) for sharing results
  with someone who does not run Studio.

### 6. Server API additions

```
POST   /api/nb/session              -> {session_id}        create kernel
DELETE /api/nb/session/<id>         -> 204                 close kernel
POST   /api/nb/exec                 {session_id, sql, options} -> blocks
        (same response shape as /api/exec; request_id wired into the
         existing inflight/cancel registry)
GET    /api/nb/session/<id>/status  -> {alive, pid, db, idle_seconds}
```

Everything else reuses existing endpoints (`/api/circuit`,
`/api/evaluate`, `/api/cancel`, `/api/relations`, `/api/schema`).
No extension-side (C/SQL) changes are required: this is a pure
Studio feature; the compatibility floor does not move.

### 7. Seeded notebooks: tutorial and case studies

- Ship the **tutorial as a notebook** (`tutorial.ipynb`): the prose
  of `doc/source/user/tutorial.rst` interleaved with its queries,
  pre-seeded; likewise, progressively, one notebook per case study.
  Generation should be scripted (rst → notebook converter or a
  hand-maintained source of truth with a CI check against the rst)
  to avoid drift -- decide during implementation, start with CS1
  hand-written to validate the format.
- Studio gains an “Open example…” menu listing the bundled
  notebooks (packaged under `provsql_studio/notebooks/`).
- Deep links extend to `?mode=notebook&nb=tutorial` (Playground) /
  `/notebook?nb=tutorial` (local).

### 8. Playground specifics

- The unmodified-Python rule holds: the Flask notebook endpoints run
  in Pyodide as-is. The **single-session caveat**: PGlite has one
  backend, so the “pinned kernel connection” and the pool are the
  same underlying session -- kernel state is visible to other API
  calls and vice versa. Acceptable for a sandbox; document it.
  Kernel restart maps to `DISCARD ALL` instead of close+reopen
  (the `psycopg_pglite` shim grows that one special case).
- Notebook persistence in the browser: autosave to localStorage,
  download/upload as locally. The seeded tutorial/case-study
  notebooks pair naturally with the already-seeded databases (the
  connection chip switches both).
- `build.sh` vendors the markdown renderer/sanitizer; the e2e
  zero-off-origin assertion keeps it honest.

### 9. Testing

- **Unit** (`studio/tests/test_notebook.py`): session lifecycle
  (create / exec / close / idle GC / cap), state persistence across
  cells (temp table created in cell 1 queried in cell 2), per-cell
  transaction rollback on error, kernel-death surfacing, the
  `exec_batch` / `exec_batch_on` refactor (existing `test_exec.py`
  must pass untouched), nbformat round-trip (save → load →
  identical model; outputs stripped mode), block-JSON re-render
  path never consuming `text/html`.
- **e2e** (`tests/e2e/test_notebook.py`): create cells, run one /
  run all, markdown render, out-of-order counters, interrupt a
  long-running cell, restart kernel clears temp tables, “→ Circuit”
  inserts a circuit cell, save/load round-trip via the file
  chooser, deep link opens the seeded notebook.
- **Web e2e** (`tests/web/`): notebook mode boots in the Playground,
  kernel restart via DISCARD ALL, seeded notebook runs end-to-end.

### 10. Documentation and release

- `doc/source/user/studio.rst`: a “Notebook mode” section with
  screenshots (capture per the CLAUDE.md OS-level workflow);
  `doc/source/dev/`: a short architecture note (kernel registry,
  nbformat mapping) in the Studio/playground dev pages.
- Release as **Studio 1.2.0** (independent stream; no extension
  bump needed). Changelog entry per the release process;
  compatibility table unchanged.

## Priorities

1. **MVP (Studio 1.2.0)**: mode + cell list; Markdown and SQL cells;
   kernel sessions with run / run-all / interrupt / restart;
   per-cell results through the existing renderer; autosave +
   `.ipynb` download/load (outputs included); unit + e2e tests.
2. **Provenance-native increment**: circuit cells, evaluation cells,
   per-cell scheme options, “→ Circuit” affordance, seeded CS1
   notebook, deep links, HTML export.
3. **Playground + content**: Playground integration (shim
   `DISCARD ALL`, vendoring, web e2e), tutorial + remaining
   case-study notebooks with a drift check, docs screenshots.
4. **Later, if pulled**: chart cell for `rv_histogram` /
   `distribution-profile` outputs; variable templating; a real
   Jupyter kernel speaking to the same backend (would make the
   `.ipynb` files executable in JupyterLab too).

## Implementation observations

- **Reuse inventory**: the result renderer (`renderBlocks` and the
  block model), the highlight-overlay editor, NOTICE pills and the
  classifier badge, the inflight/cancel registry, `circuit.js`’s
  scene fetch + SVG, the eval strip’s dispatch (`/api/evaluate`),
  the localStorage carry conventions, the load/download file
  patterns from the query box. The notebook is mostly *composition*
  of existing pieces; the genuinely new server-side surface is the
  kernel registry and the `exec_batch_on` refactor.
- **Risk: editor ergonomics.** One textarea per SQL cell with the
  existing overlay scales fine to tens of cells; do not reach for
  CodeMirror in the MVP. If cell counts grow problematic, virtualise
  rendering before swapping editors.
- **Risk: kernel leakage.** Pinned connections held by abandoned
  tabs are the main resource hazard; the idle timeout + cap + beacon
  cleanup triad must land in the MVP, not later.
- **Mode-switch carry**: the `ps.sql` carry channel moves a *query*
  between modes; define the notebook counterpart narrowly (carrying
  the current cell’s SQL into Where/Circuit mode on switch) and
  nothing more -- a whole-notebook carry has no receiving surface.
