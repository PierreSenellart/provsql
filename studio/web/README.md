# ProvSQL Studio — browser (PGlite) build

A second distribution target for Studio: the same UI running **fully
client-side** over an in-page PGlite (PostgreSQL + ProvSQL in WebAssembly),
with no Flask server and no database connection. The PyPI `provsql-studio`
package remains the canonical server product; this build shares its
frontend assets.

Status: **scaffold + working seed**. `demo.html` is a self-contained page
that boots PGlite + ProvSQL and runs the core flow (query → provenance
tokens → probability) — it is the e2e smoke target. The full UI port is in
progress; the architecture and plan are below.

## Architecture: a fetch-shim, not a rewrite

Studio's frontend (`provsql_studio/static/app.js` + `circuit.js`, ~7000
lines) already runs in the browser and talks to the Flask backend over
`fetch('/api/...')`. The Flask backend (`app.py` routes + `db.py` +
`circuit.py`) is the only server-side piece. To go static we **keep the
frontend unchanged** and replace the HTTP backend with an **in-page
`fetch` interceptor** that answers `/api/*` from an in-page PGlite:

```
static/app.js  ──fetch('/api/exec')──►  shim.js (window.fetch override)
                                          │  ports db.py / app.py logic
                                          ▼
                                        PGlite + provsql  (WASM, this tab)
```

This avoids reimplementing the 7000-line UI; the work is porting the
backend request/response contracts (the `/api/*` handlers) to JS over
`pg.query`. PGlite is single-connection, so the pool/PID-cancellation
machinery collapses to a single backend.

## Endpoint inventory (the port work-list)

From `provsql_studio/app.py`. `[trivial]` = constant/single-DB in the
browser; `[core]` = needed for query + provenance; `[viz]` = circuit mode;
`[n/a]` = no external tools in the browser (return empty/disabled).

| Endpoint | role | port |
|---|---|---|
| `/api/conn`, `/api/databases`, `/api/config` | connection/config | [trivial] |
| `/api/relations`, `/api/schema` | sidebar | [core] `db.list_relations`/`list_schema` |
| `/api/exec` | run SQL + provenance | [core] `db.exec_batch` (where/boolean/semiring, wrap_last) |
| `/api/set_prob`, `/api/provenance_mappings`, `/api/custom_semirings` | inputs to eval | [core] |
| `/api/circuit/<t>`, `/circuit/<t>/expand`, `/api/leaf/<t>` | circuit DAG | [viz] `circuit.py` |
| `/api/evaluate` | semiring / probability | [core] |
| `/api/kc/*` (tools, registry, cnf/ddnnf/td, benchmark) | external compilers | [n/a] empty/disabled |
| `/api/cancel/<id>` | query cancel | [trivial] (single backend) |

Port order: trivial → exec + relations/schema (a working query console with
provenance) → evaluate + set_prob (semirings/probability) → circuit viz →
where-mode highlights. `kc/*` return "no tools" (the registry-driven
pickers already tolerate an empty CLI-tool set in-browser).

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
