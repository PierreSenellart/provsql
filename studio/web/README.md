# ProvSQL Playground – ProvSQL Studio in the browser

**ProvSQL Playground** is a second distribution target for Studio: the same UI
running **fully client-side** over an in-page PGlite (PostgreSQL + ProvSQL in
WebAssembly), with no Flask server and no database connection. The PyPI
`provsql-studio` package remains the canonical server product; this build
shares its frontend assets and rebrands the wordmark to “ProvSQL Playground”.
It is published as static files (e.g. at `provsql.org/playground/`).

## Architecture: reuse the real Python via Pyodide (no parallel port)

Goal: the **full Studio**, client-side, with **no separate JS/TS
reimplementation to maintain** – Studio feature changes must flow through
untouched. Efficiency is irrelevant (it's a demo; real users install
locally). So we run the **unmodified `provsql_studio` Python** in
**[Pyodide](https://pyodide.org)** (CPython→WASM) beside PGlite, and reach it
from the frontend's `fetch('/api/*')`.

The backend (PGlite + Pyodide) is expensive to instantiate, and the unmodified
Studio frontend reloads the page to switch mode or database. To keep a reload
from re-initialising everything, the page is split in two same-origin
documents:

```
app.html  (SHELL, never reloads)  shell-boot.js
  PGlite cluster + active DB  ·  Pyodide + Flask test_client  ·  WASM Graphviz
  JSPI lives here (top frame)  ·  postMessage server  ·  <iframe src=ui.html>
   └─ ui.html  (CHILD, the Studio UI)  child-boot.js
        unmodified app.js + circuit.js
        window.fetch('/api/*')  ── postMessage ──►  shell  ──► Pyodide/PGlite
```

- The **shell** owns the warm backend. A **mode switch** navigates only the
  iframe (≈140 KB of JS); a **database switch** reopens just PGlite (the shell
  handles `POST /api/conn` in place); Pyodide + Flask stay live across both.
  **JSPI** (the sync→async bridge, below) is needed only in this top frame.
- The **child** runs the unmodified frontend and forwards every `/api/*`
  fetch to the shell over `postMessage`, rebuilding the reply into a
  `Response`. Each load tags its messages with an epoch so a reply straddling
  a reload cannot resolve the wrong request.

Inside the shell, the request path is:

```
ui.html app.js ─fetch('/api/exec')→ child-boot bridge ─postMessage→ shell
                                     │  enters Python via PyProxy.callPromising()
                                     ▼
   Pyodide:  app.py (Flask app.test_client) → db.py → fake psycopg
                                     │  cursor.execute → run_sync(pg.query(...))
                                     ▼
                          PGlite + provsql  (WASM, this tab)
```

- **Unchanged:** `app.py`, `db.py`, `circuit.py`, and `static/` – the whole
  Studio. The only new, stable code is the **fake `psycopg`** module, the
  `fetch`→`test_client` bridge, and the shell/child boot pair.
- Dump-style `COPY … FROM stdin` units (notebook setup cells, pasted pg_dump
  output) work: db.py routes them through `cursor.copy()`, which the shim
  maps onto a single `COPY … FROM '/dev/blob'` with the written rows as
  PGlite's per-query `blob` option (PGlite speaks no COPY sub-protocol over
  `query()`).
- **psycopg shim surface** (all `db.py` uses): `ConnectionPool.connection()`
  → `conn.cursor()` → `execute` / `fetch{all,one,many}` / `description` /
  `rowcount`; `sql.SQL` / `sql.Identifier.format()`; `psycopg.errors.*`
  (`UndefinedFunction`, `UndefinedObject`, `InsufficientPrivilege`) and
  `psycopg.Error`; SAVEPOINT / `SET LOCAL` / rollback. Each maps onto
  PGlite; the pool/PID-cancel/`subprocess`(kc)/`threading` parts collapse
  (single connection, no external tools).
- **`flask` + `sqlparse`** are installed by micropip from the **vendored**
  wheel closure (`wheels/`), not PyPI.
- **Sync→async bridge:** `db.py` is synchronous; PGlite is async. The shim's
  `cursor.execute` does `run_sync(pg.query(...))`. This needs JSPI (WASM
  stack-switching) and a JSPI-aware entry: the shell calls the Python request
  handler via `PyProxy.callPromising()`. Backend calls are serialised on one
  chain (the whole app shares one PGlite connection, while Flask assumes a
  private one per request); `switchDb` and Reset run on that same chain.

**Browser support:** the Playground needs a browser with WebAssembly JSPI. The
landing page (`landing.html`) is the single maintained source of truth for
current browser support; keep version specifics there only.

**Self-hosted:** the build loads **nothing from a CDN at run time**.
`vendor.sh` fetches Pyodide, the wheel closure, Graphviz
(`@hpcc-js/wasm-graphviz`) and Font Awesome into the doc-root at build time,
and the boot modules reference only local paths. The `test_fully_self_hosted`
e2e boots with every off-origin request blocked to guarantee it. Because these
are now redistributed, `build.sh` bundles their license texts under
`licenses/` and writes `THIRD-PARTY.html` (linked from the footer).

`/api/kc/*` (external knowledge-compiler tools) return “no tools” in the
browser – the registry-driven pickers already tolerate an empty CLI set.

## Build & serve

`build.sh` assembles the doc-root reproducibly. It copies the unmodified
Studio frontend + backend from `../provsql_studio/`, derives the UI page
`ui.html` and the shell `app.html`, and pulls in the two WebAssembly artifacts
built by [`../../wasm/`](../../wasm/README.md) (the matched `@electric-sql/pglite`
dist and `provsql.tar.gz`):

```
./build.sh --pglite <pglite-dist-dir> --provsql <provsql.tar.gz>
# or:  PGLITE_DIST=<dir> PROVSQL_TARGZ=<file> ./build.sh
# pass neither to reuse the in-place WASM artifacts (re-assemble after a
# Studio-source change with no WASM rebuild)
python3 serve.py            # then open http://localhost:8089/
```

Tracked inputs are `shell-boot.js`, `child-boot.js`, `landing.html`,
`psycopg_pglite.py`, `serve.py` and `build.sh`; everything `build.sh` writes
is git-ignored. The assembled doc-root:

```
studio/web/                  # this dir is itself the doc-root
  index.html                 # GENERATED landing (= landing.html): JSPI gate + Launch
  landing.html               # the landing source (tracked)
  app.html                   # GENERATED shell: boot-status bar + shell-boot.js + iframe
  shell-boot.js              # owns the warm backend (PGlite + Pyodide), mounts ui.html
  ui.html                    # GENERATED UI page: the Studio frontend + child-boot.js
  child-boot.js              # /api/* -> shell bridge, injects app.js, WASM-only UI bits
  psycopg_pglite.py          # the fake psycopg / psycopg_pool / subprocess module
  build.sh serve.py          # assembler + dev static server (tracked)
  app.js circuit.js          # copied from ../provsql_studio/static (unmodified)
  app.css colors_and_type.css fonts-face.css fonts/ img/
  pkg/                       # copy of ../provsql_studio/*.py (the unmodified backend)
  pkg/notebooks/             # the bundled example notebooks + manifest.json;
                             # shell-boot mirrors them into the Pyodide FS
  pglite/                    # the built @electric-sql/pglite dist (wasm/data/js + contrib)
  provsql.tar.gz             # the extension bundle
  static/circuit.js          # app.js loads /static/circuit.js  (hard-coded path)
  static/notebook.js         # app.js loads /static/notebook.js (path-rewritten copy)
  static/vendor/             # marked + DOMPurify (markdown cells)
  static/app.css             # circuit.js loads /static/app.css  (hard-coded path)
  casestudies/               # one DB per tutorial / case study + manifest.json
  build-casestudies.py       # converts doc/*/setup.sql into the above (tracked)
  demo.html                  # standalone plain-PGlite demo (no Pyodide, no JSPI)
```

### Tutorial and case-study databases

The one IndexedDB-persisted cluster holds a database per tutorial / case
study (`tutorial`, `cs1`, `cs2`, `cs4`, `cs5`, `cs6`, `cs7`), switchable from
the connection chip. `build-casestudies.py` derives them from the canonical
`doc/{tutorial,casestudyN}/setup.sql` scripts, rewriting the psql-only
`COPY … FROM stdin` / `\copy … CSV` constructs into INSERTs and splitting each
script into individual statements (PGlite runs a whole `exec()` as one
transaction, which breaks the `ON COMMIT DROP` temp table
`create_provenance_mapping` uses). `shell-boot.js` creates the databases up
front (skipped on later boots via a manifest-signature flag) and seeds each
lazily on first switch. `casestudy3` is omitted: it loads a multi-megabyte
GTFS dataset that must be downloaded separately. The **Reset** button asks the
shell to drop and re-seed every database.

### Landing page and JSPI gate

A bare visit hits **index.html**, a small static landing that explains the
**JSPI requirement** (browser support, and the Firefox `about:config` flag)
and links to the shell (**app.html**). It feature-detects JSPI: shared deep
links (`?mode=`/`?db=`/`?q=`) are forwarded straight to the app when JSPI is
present, and otherwise the landing is shown so the user sees the requirement
instead of a silent hang.

### Designed for a dumb static host

The build runs on a plain file server (Apache with no CGI, a CDN, `file://`):
no per-request rewriting, no app server, **no redirects**. `build.sh` makes the
frontend path-portable by rewriting the few root-absolute paths in the copied
`app.js` (`/static/circuit.js` and the `/circuit` / `/where` mode navigation)
to **relative** URLs (`static/circuit.js`, `?mode=…`). The boot modules resolve
all sibling assets against their own module URL, and the shell mounts `ui.html`
by a relative URL, so the whole thing works unchanged at the server root or
under a sub-path (`https://host/playground/`). PGlite is single-threaded, so no
COOP/COEP headers are needed; the only server nicety is the WASM MIME type,
supplied by a shipped `.htaccess` (`AddType application/wasm .wasm`).

`serve.py` is a dev server that does exactly this and nothing more (threaded
static file serving + the WASM MIME types); it is the local mirror of the
hosting requirements, not a runtime dependency.

A **mode switch** reloads only the iframe and a **database switch** reopens
just PGlite, so the heavy backend is not re-initialised; the DB is persisted to
IndexedDB so its provenance circuit survives (a token carried across a switch –
e.g. jump-to-circuit – must still resolve).

### Shareable links

A view is fully captured in the URL query string:
`?mode=circuit|where|notebook&db=<database>&q=<url-encoded SQL>&nb=<example>`.
Opening such a link lands on that database and mode with the query pre-filled
and auto-run; the shell consumes `?db` and forwards `?mode`/`?q`/`?nb` to the
iframe, where `child-boot` feeds the query into the same sessionStorage channel
app.js uses for its mode-switch carry (`?nb=` opens the named bundled example
notebook and implies notebook mode). The **Link** button in the nav copies the
current database + mode + query box as one of these URLs, pointing at the shell
(the top frame).

### Notebook mode: the single-session mapping

The notebook front-end and `/api/nb/*` endpoints run unmodified; PGlite's one
shared backend session changes what a "kernel" is. The pinned kernel
connection and the request pool are the same session (kernel state is visible
across notebook tabs and plain API calls); kernel close/restart maps to
`DISCARD ALL` + search_path restore in `psycopg_pglite.py` (so restarting any
tab's kernel resets them all); the pagehide kernel-close `sendBeacon` is
rerouted through the postMessage bridge by `child-boot.js` (a real beacon
would 404 on the static host and leak the kernel against MAX_KERNELS). The
binding banner's "Create X" works: `POST /api/databases` flows to the Python
backend, whose CREATE DATABASE grows the shared cluster -- the shell lists
databases live from pg_database, installs the extension on first switch (the
per-open PREP), and Reset drops user-created databases too.

## CI/CD

- The WASM artifacts, `build.sh`, and the **browser e2e** (`studio/tests/web/`)
  are built and run **locally** via `make wasm` / `make playground-test`
  (`wasm/build-wasm.sh`), not in CI -- that build is heavy and was only ever
  opt-in, so the local script is the single source of truth.
  [`.github/workflows/wasm.yml`](../../.github/workflows/wasm.yml) keeps just
  the cheap per-PR in-process-store single-session smoke.
- The e2e is **pytest-playwright** driving the real frontend + Python backend
  against the in-page PGlite (no PostgreSQL): it covers boot + JSPI, the
  query → circuit → semiring path, the `/api` surface (where-provenance,
  `/api/circuit`, `/api/kc/td`), the database list/switch, Reset, deep links,
  sub-path portability, and that a mode switch keeps the backend warm. The
  fixtures skip if the doc-root isn't assembled. Run locally with:
  ```
  cd studio && pytest tests/web/        # needs build.sh to have run
  ```
  On this host add `PLAYWRIGHT_HOST_PLATFORM_OVERRIDE=ubuntu24.04-x64`
  (Ubuntu 26.04 isn't an official Playwright platform; see `CLAUDE.local.md`).
- Studio's own version stream and the PyPI release are unchanged; the
  browser build is published as static assets (e.g. alongside the docs
  site), pinned to a PGlite/ProvSQL pair.
