// Boot the full ProvSQL Studio in the browser, client-side:
//   PGlite (Postgres+provsql, WASM)  +  Pyodide (the UNMODIFIED Studio
//   Python)  +  a psycopg->PGlite shim  +  a fetch->Flask-test_client shim.
// Then inject the real static/app.js, which runs against the in-page backend.
//
// Requires a JSPI-capable browser (recent Chrome/Edge) for run_sync.
import { PGlite } from './pglite/index.js'
import { uuid_ossp } from './pglite/contrib/uuid_ossp.js'
import { loadPyodide } from 'https://cdn.jsdelivr.net/pyodide/v0.29.4/full/pyodide.mjs'
import { Graphviz } from 'https://cdn.jsdelivr.net/npm/@hpcc-js/wasm-graphviz@1.22.0/dist/index.js'

const PKG = ['__init__.py', 'db.py', 'app.py', 'circuit.py', 'kc.py']
// Resolve sibling assets against this module's URL, not the document's, so the
// page can be served from any path (the mode routes redirect to /?mode=...).
const asset = (p) => new URL(p, import.meta.url)
// Mode is carried in the query string: the server only has to redirect the
// clean /circuit and /where paths (that app.js navigates to) onto ?mode=...,
// which keeps it a plain static host with no per-route HTML rewriting.
const mode = new URLSearchParams(location.search).get('mode') === 'where' ? 'where' : 'circuit'
document.body.className = 'mode-' + mode
// Hide UI that cannot apply to a single in-page database:
//  - Config rows for GUCs that only govern external tools the browser cannot
//    spawn: tool_search_path (a $PATH for subprocesses) and fallback_compiler
//    (an external compiler tried after the in-process routes).
//  - The DSN editor (the connection-edit button + its panel): there is one
//    in-page PGlite, so connecting to an arbitrary DSN is impossible. The DB
//    switcher stays, for the eventual multi-database support.
// Elements stay in the DOM so app.js's load/save paths remain null-safe.
{
  const st = document.createElement('style')
  st.textContent =
    '.wp-config__row:has(#cfg-tool-search-path),' +
    '.wp-config__row:has(#cfg-fallback-compiler),' +
    '#conn-dot,#dsn-panel{display:none}'
  document.head.appendChild(st)
}
const boot = document.getElementById('studio-boot-status')
const say = (m) => { if (boot) boot.textContent = m; console.log('[studio-boot]', m) }
// Surface a boot failure in the status bar rather than leaving it stuck on the
// last say(): a rejected top-level await otherwise reads as a silent hang.
const fail = (e) => {
  const m = (e && (e.stack || e.message)) || String(e)
  if (boot) { boot.style.display = 'block'; boot.style.background = '#5a1111'; boot.textContent = 'boot error: ' + m }
  console.error('[studio-boot]', e)
}
window.addEventListener('unhandledrejection', (ev) => fail(ev.reason))
window.addEventListener('error', (ev) => fail(ev.error || ev.message))

say('starting PostgreSQL + ProvSQL (WASM)…')
const provsql = {
  name: 'provsql',
  setup: async (_p, o) => ({ emscriptenOpts: o, bundlePath: new URL('./provsql.tar.gz', import.meta.url) }),
}
const EXT = { uuid_ossp, provsql }
// One IndexedDB-persisted cluster holds every tutorial / case-study database
// (manifest below). Persistence matters because mode switching is a full-page
// navigation that reboots the tab; the DBs and their provenance circuits must
// survive the reload (a token carried across a switch must still resolve).
const DATADIR = 'idb://provsql-studio'
const DEFAULT_DB = 'tutorial'
const manifest = await (await fetch(asset('./casestudies/manifest.json'))).json()

// Per-database preparation, idempotent, re-run on every open: the extension,
// the session search_path, and the browser-specific tool handling. External
// knowledge compilers (d4, c2d, …) need subprocesses the browser cannot spawn,
// so every registry entry is disabled (they never appear as eval-strip
// options); tool_available is redefined to report only `dot`, which is real
// here — supplied as WASM Graphviz through the subprocess shim, and gated on
// by the tree-decomposition / d-DNNF image endpoints.
const PREP = [
  'CREATE EXTENSION IF NOT EXISTS "uuid-ossp"',
  'CREATE EXTENSION IF NOT EXISTS provsql CASCADE',
  'SET search_path TO public, provsql',
  `DO $$ DECLARE r record; BEGIN
     IF to_regclass('provsql.tools') IS NOT NULL THEN
       FOR r IN SELECT name FROM provsql.tools WHERE enabled LOOP
         PERFORM provsql.set_tool_enabled(r.name, false);
       END LOOP;
     END IF;
   END $$`,
  `CREATE OR REPLACE FUNCTION provsql.tool_available(name TEXT)
     RETURNS boolean LANGUAGE sql IMMUTABLE AS $$ SELECT lower(name) = 'dot' $$`,
]

// Create every database once (cheap, metadata only); seeding is lazy, on first
// open. PGlite is single-database per connection, so each database is a real
// database in the shared cluster, reached by reopening with {database}.
let _bootstrap = await PGlite.create({ dataDir: DATADIR, extensions: EXT })
const _have = (await _bootstrap.query('SELECT datname FROM pg_database')).rows.map((r) => r.datname)
for (const m of manifest) {
  if (!_have.includes(m.name)) await _bootstrap.exec(`CREATE DATABASE ${m.name}`)
}
await _bootstrap.close()

// The currently-open database. PGlite holds one connection at a time, so
// switching closes the active instance and reopens on the target.
let activeDb = null
let activePg = null
async function switchDb(name) {
  if (name === activeDb) return
  if (activePg) { await activePg.close(); activePg = null }
  const inst = await PGlite.create({ dataDir: DATADIR, database: name, extensions: EXT })
  for (const s of PREP) await inst.exec(s)
  // "Seeded" is recorded as a database comment, not a table, so nothing shows
  // up in the schema panel. (oldtable migrates away the marker table an
  // earlier build created in public.)
  const mark = (await inst.query(
    "SELECT coalesce(shobj_description(d.oid, 'pg_database'), '') = 'provsql-studio-seeded' AS commented,"
    + " to_regclass('public.__provsql_seeded') IS NOT NULL AS oldtable"
    + " FROM pg_database d WHERE d.datname = current_database()")).rows[0]
  if (!mark.commented && !mark.oldtable) {
    say(`loading ${name}…`)
    const entry = manifest.find((m) => m.name === name)
    const stmts = await (await fetch(asset(`./casestudies/${entry.file}`))).json()
    for (const s of stmts) await inst.exec(s)         // one stmt per exec: see build-casestudies.py
  }
  if (mark.oldtable) await inst.exec('DROP TABLE public.__provsql_seeded')
  if (!mark.commented) await inst.exec(`COMMENT ON DATABASE ${name} IS 'provsql-studio-seeded'`)
  activeDb = name
  activePg = inst
  // Persist the choice: a DB switch (and a mode switch) reloads the page, and
  // boot must reopen the same database rather than snap back to the default.
  localStorage.setItem('ps.activeDb', name)
}
const _saved = localStorage.getItem('ps.activeDb')
await switchDb(manifest.some((m) => m.name === _saved) ? _saved : DEFAULT_DB)

// async PGlite bridge (the shim's run_sync target) -> the active database.
globalThis.pgQuery = async (sql, params) => {
  const notices = []
  try {
    const r = await activePg.query(sql, params ? Array.from(params) : [],
      { onNotice: n => notices.push({ severity: n.severity || 'NOTICE', message_primary: n.message || '',
                                      message_detail: n.detail || '', message_hint: n.hint || '' }) })
    return { ok: true, rows: r.rows, fields: r.fields, affected: r.affectedRows ?? 0, notices }
  } catch (e) { return { ok: false, message: String(e.message || e), notices } }
}

// Graph layout/rendering: the unmodified backend shells out to `dot` (-Tjson
// for canvas positions, -Tsvg for the tree-decomposition / d-DNNF images);
// the subprocess shim routes both to this WASM Graphviz with the matching
// output format.
const graphviz = await Graphviz.load()
globalThis.graphvizDot = async (src, format) => graphviz.layout(src, format || 'json', 'dot')

say('loading Python (Pyodide)…')
const py = await loadPyodide()
await py.loadPackage('micropip')
say('installing flask + sqlparse…')
await py.runPythonAsync(`import micropip; await micropip.install(['flask','sqlparse'])`)

say('wiring the Studio backend…')
py.runPython(`import os; os.environ['PROVSQL_STUDIO_CONFIG_DIR']='/cfg'; os.makedirs('/cfg', exist_ok=True)`)
py.runPython(await (await fetch(asset('./psycopg_pglite.py'))).text())   // registers fake psycopg

py.FS.mkdir('/studio'); py.FS.mkdir('/studio/provsql_studio')
for (const f of PKG) {
  py.FS.writeFile(`/studio/provsql_studio/${f}`, await (await fetch(asset(`./pkg/${f}`))).text())
}
py.runPython(`
import sys, json, traceback
sys.path.insert(0, '/studio')
from provsql_studio.app import create_app
_app = create_app(search_path='provsql, public')
_app.config['PROPAGATE_EXCEPTIONS'] = True
_client = _app.test_client()
def handle(method, path, body_json):
    b = json.loads(body_json) if body_json else None
    try:
        r = _client.open(path, method=method, json=b)
        return json.dumps({'status': r.status_code, 'body': r.get_data(as_text=True),
                           'ctype': r.headers.get('Content-Type','application/json')})
    except Exception:
        return json.dumps({'status': 500, 'body': traceback.format_exc(), 'ctype': 'text/plain'})
`)
const handle = py.globals.get('handle')

// Serialise backend calls. The whole Studio shares ONE PGlite connection, but
// the Flask backend assumes a private connection per request (it drives
// per-request BEGIN / SET LOCAL / SAVEPOINT). The frontend fires several
// /api/* requests at once (e.g. on a mode switch); via JSPI their Python
// handlers interleave, so one request's COMMIT would close the transaction
// another is mid-way through -> "SET LOCAL can only be used in transaction
// blocks". Running them one at a time makes the shared connection behave like
// the dedicated one the backend expects.
let _chain = Promise.resolve()
const enqueue = (task) => {
  const r = _chain.then(task, task)
  _chain = r.then(() => {}, () => {})
  return r
}
const callBackend = (method, path, body) => enqueue(() => handle.callPromising(method, path, body))
const reply = (out) => new Response(out.body, { status: out.status, headers: { 'Content-Type': out.ctype } })

// Route the real frontend's /api/* fetches into the in-page Flask app.
const realFetch = window.fetch.bind(window)
window.fetch = async (input, init = {}) => {
  const url = typeof input === 'string' ? input : input.url
  const path = url.replace(location.origin, '')
  if (path.startsWith('/api/')) {
    const method = (init.method || (typeof input !== 'string' && input.method) || 'GET').toUpperCase()
    const body = init.body || null
    // Database list / switch are JS-layer concerns: each tutorial / case study
    // is a real database in the one cluster, reached by reopening the active
    // PGlite connection. The backend's DSN-based switching does not apply.
    if (path === '/api/databases' && method === 'GET') {
      return new Response(JSON.stringify(manifest.map((m) => m.name)),
                          { status: 200, headers: { 'Content-Type': 'application/json' } })
    }
    if (path.split('?')[0] === '/api/conn' && method === 'POST') {
      let target = null
      try { target = JSON.parse(String(body) || '{}').database } catch (_e) { /* ignore */ }
      const out = JSON.parse(await enqueue(async () => {
        if (target && manifest.some((m) => m.name === target)) await switchDb(target)
        return handle.callPromising('GET', '/api/conn', '')   // conn_info for the now-active db
      }))
      return reply(out)
    }
    return reply(JSON.parse(await callBackend(method, path, body ? String(body) : '')))
  }
  return realFetch(input, init)
}

say('starting the Studio UI…')
await new Promise((resolve) => {
  const s = document.createElement('script')
  s.src = asset('./app.js'); s.onload = resolve; document.body.appendChild(s)
})
// External knowledge-compiler tools (d4, c2d, …) need subprocesses, which the
// browser has no way to run, so drop the tool-registry UI rather than offer an
// editor for tools that can never be available. The eval-strip already hides
// tool-backed methods on its own (every tool reports available:false).
document.getElementById('tools-btn')?.remove()
document.getElementById('tools-panel')?.remove()
if (boot) boot.style.display = 'none'
