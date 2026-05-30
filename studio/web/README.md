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

The browser build needs the matched `pglite.wasm` + `provsql.tar.gz` from
[`../../wasm/`](../../wasm/README.md). Assemble a static dir:

```
studio/web/
  demo.html                  # working seed (and e2e target)
  index.html                 # symlink/copy of ../provsql_studio/static/index.html (full UI; once the shim lands)
  pglite/                    # the built @electric-sql/pglite dist (wasm/data/js + contrib)
  provsql.tar.gz             # the extension bundle
  shim.js                    # the /api/* fetch interceptor (to be built)
  static/                    # app.js, circuit.js, css (from provsql_studio/static)
```

Serve it with any static server (PGlite is single-threaded — no
COOP/COEP needed) and open in a browser; or distribute on a CDN / `file://`.

## CI/CD

- The WASM artifacts are built and smoke-tested by
  [`.github/workflows/wasm.yml`](../../.github/workflows/wasm.yml)
  (`wasm` job, `workflow_dispatch`).
- The web build's e2e is **Playwright against the static `demo.html`/UI**,
  headless Chromium — a natural extension of the existing
  `studio/tests/e2e/` suite (note the
  `PLAYWRIGHT_HOST_PLATFORM_OVERRIDE=ubuntu24.04-x64` workaround for this
  host, per `CLAUDE.local.md`). It runs against the served static dir, no
  Postgres service required.
- Studio's own version stream and the PyPI release are unchanged; the
  browser build is published as static assets (e.g. alongside the docs
  site), pinned to a PGlite/ProvSQL pair.
