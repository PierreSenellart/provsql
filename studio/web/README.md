# ProvSQL Studio — browser (PGlite) build

A second distribution target for Studio: the same UI running **fully
client-side** over an in-page PGlite (PostgreSQL + ProvSQL in WebAssembly),
with no Flask server and no database connection. The PyPI `provsql-studio`
package remains the canonical server product; this build shares its
frontend assets.

Status: **scaffold + working seed; full-Studio approach validated.**
`demo.html` is a self-contained page that boots PGlite + ProvSQL and runs
the core flow (query → provenance → probability). The full Studio UI runs
via Pyodide (below); the sync/async bridge is proven, the build remains.

## Architecture: reuse the real Python via Pyodide (no parallel port)

Goal: the **full Studio**, client-side, with **no separate JS/TS
reimplementation to maintain** — Studio feature changes must flow through
untouched. Efficiency is irrelevant (it's a demo; real users install
locally). So we run the **unmodified `provsql_studio` Python** in
**[Pyodide](https://pyodide.org)** (CPython→WASM) beside PGlite:

```
static/app.js ─fetch('/api/exec')→ shim (window.fetch override, JS)
                                     │  enters Python via PyProxy.callPromising()
                                     ▼
   Pyodide:  app.py (Flask app.test_client) → db.py → fake psycopg
                                     │  cursor.execute → run_sync(pg.query(...))
                                     ▼
                          PGlite + provsql  (WASM, this tab)
```

- **Unchanged:** `app.py`, `db.py`, `circuit.py`, and `static/` — the whole
  Studio. The only new, stable code is the **fake `psycopg`** module + a
  ~30-line `fetch`→`test_client` bridge.
- **psycopg shim surface** (all `db.py` uses): `ConnectionPool.connection()`
  → `conn.cursor()` → `execute` / `fetch{all,one,many}` / `description` /
  `rowcount`; `sql.SQL` / `sql.Identifier.format()`; `psycopg.errors.*`
  (`UndefinedFunction`, `UndefinedObject`, `InsufficientPrivilege`) and
  `psycopg.Error`; SAVEPOINT / `SET LOCAL` / rollback. Each maps onto
  PGlite; the pool/PID-cancel/`subprocess`(kc)/`threading` parts collapse
  (single connection, no external tools).
- **`flask` + `sqlparse`** install via micropip (pure-Python).
- **Sync→async bridge (validated):** `db.py` is synchronous; PGlite is
  async. The shim's `cursor.execute` does `run_sync(pg.query(...))`. This
  needs JSPI (WASM stack-switching) and a JSPI-aware entry: the fetch-shim
  calls the Python request handler via `PyProxy.callPromising()`. Confirmed
  working (sync Python synchronously awaiting an async JS call).

**Browser support:** JSPI ships in recent **Chrome/Edge** (and Node with
`--experimental-wasm-stack-switching`); Firefox/Safari are partial/flagged.
The Pyodide-Studio demo is therefore Chromium-only for now (acceptable for
a demo; an Atomics+worker bridge would lift that at the cost of COOP/COEP +
a worker). `demo.html` itself (plain PGlite, no Pyodide) has no such
constraint.

`/api/kc/*` (external knowledge-compiler tools) return "no tools" in the
browser — the registry-driven pickers already tolerate an empty CLI set.

## Build & serve

`build.sh` assembles the doc-root reproducibly. It copies the unmodified
Studio frontend + backend from `../provsql_studio/`, derives the boot-shell
`index.html`, and pulls in the two WebAssembly artifacts built by
[`../../wasm/`](../../wasm/README.md) (the matched `@electric-sql/pglite`
dist and `provsql.tar.gz`):

```
./build.sh --pglite <pglite-dist-dir> --provsql <provsql.tar.gz>
# or:  PGLITE_DIST=<dir> PROVSQL_TARGZ=<file> ./build.sh
python3 serve.py            # then open http://localhost:8089/
```

Tracked inputs are `studio-boot.js`, `psycopg_pglite.py`, `serve.py` and
`build.sh`; everything `build.sh` writes is git-ignored. The assembled
doc-root:

```
studio/web/                  # this dir is itself the doc-root
  index.html                 # GENERATED entry: boot-status bar + studio-boot.js
  studio-boot.js             # boots PGlite + Pyodide + the shims, injects app.js
  psycopg_pglite.py          # the fake psycopg / psycopg_pool / subprocess module
  build.sh serve.py          # assembler + dev static server (tracked)
  app.js circuit.js          # copied from ../provsql_studio/static (unmodified)
  app.css colors_and_type.css fonts-face.css fonts/ img/
  pkg/                       # copy of ../provsql_studio/*.py (the unmodified backend)
  pglite/                    # the built @electric-sql/pglite dist (wasm/data/js + contrib)
  provsql.tar.gz             # the extension bundle
  static/circuit.js          # app.js loads /static/circuit.js  (hard-coded path)
  static/app.css             # circuit.js loads /static/app.css  (hard-coded path)
  casestudies/               # one DB per tutorial / case study + manifest.json
  build-casestudies.py       # converts doc/*/setup.sql into the above (tracked)
  demo.html                  # standalone plain-PGlite demo (no Pyodide)
```

### Tutorial and case-study databases

The one IndexedDB-persisted cluster holds a database per tutorial / case
study (`tutorial`, `cs1`, `cs2`, `cs4`, `cs5`, `cs6`, `cs7`), switchable from
the connection chip. `build-casestudies.py` derives them from the canonical
`doc/{tutorial,casestudyN}/setup.sql` scripts, rewriting the psql-only
`COPY … FROM stdin` / `\copy … CSV` constructs into INSERTs and splitting each
script into individual statements (PGlite runs a whole `exec()` as one
transaction, which breaks the `ON COMMIT DROP` temp table
`create_provenance_mapping` uses). studio-boot.js creates the databases up
front and seeds each lazily on first switch. `casestudy3` is omitted: it loads
a multi-megabyte GTFS dataset that must be downloaded separately.

### Designed for a dumb static host

The build runs on a plain file server (Apache with no CGI, a CDN, `file://`):
no per-request HTML rewriting, no app server. The only server cooperation is
**two redirects**, for the clean mode paths the unmodified `app.js` navigates
to:

```apache
Redirect /circuit /?mode=circuit
Redirect /where   /?mode=where
```

`studio-boot.js` reads `?mode=` (default `circuit`), sets the `<body>` mode
class before injecting `app.js`, and resolves all its sibling assets against
its own module URL — so the single real page at `/` is what every mode route
lands on. The two `/static/<f>` paths above are the only absolute asset URLs
hard-coded in the unmodified frontend, so they exist as real files. PGlite is
single-threaded, so no COOP/COEP headers are needed.

`serve.py` is a dev server that does exactly this and nothing more (threaded
static file serving + those two redirects); it is the local mirror of the
Apache config above, not a runtime dependency.

Mode switching is a full-page navigation (the frontend is path-routed), which
reboots the tab; the DB is therefore persisted to IndexedDB so its provenance
circuit survives the reload (a token carried across a switch — e.g.
jump-to-circuit — must still resolve).

## CI/CD

- The `wasm` job in [`.github/workflows/wasm.yml`](../../.github/workflows/wasm.yml)
  (`workflow_dispatch`) builds the WASM artifacts, then runs `build.sh` and
  the **browser e2e** (`studio/tests/web/`) in headless Chromium, and uploads
  the assembled `studio/web/` as an artifact.
- The e2e is **pytest-playwright** driving the real frontend + Python backend
  against the in-page PGlite (no PostgreSQL): it covers boot + JSPI, the
  query → circuit → semiring path, the `/api` surface (where-provenance,
  `/api/circuit`, `/api/kc/td`), the database list/switch, and Reset. The
  fixtures skip if the doc-root isn't assembled. Run locally with:
  ```
  cd studio && pytest tests/web/        # needs build.sh to have run
  ```
  On this host add `PLAYWRIGHT_HOST_PLATFORM_OVERRIDE=ubuntu24.04-x64`
  (Ubuntu 26.04 isn't an official Playwright platform; see `CLAUDE.local.md`).
- Studio's own version stream and the PyPI release are unchanged; the
  browser build is published as static assets (e.g. alongside the docs
  site), pinned to a PGlite/ProvSQL pair.
