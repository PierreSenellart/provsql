// Shell half of the browser ProvSQL Playground (paired with child-boot.js).
//
// This document (app.html) never reloads: it owns the warm backend --
//   PGlite (Postgres+provsql, WASM)  +  Pyodide (the UNMODIFIED Studio
//   Python)  +  a psycopg->PGlite shim  +  a Flask test_client --
// and mounts the unmodified Studio UI in an <iframe> (ui.html, driven by
// child-boot.js). The UI's /api/* fetches arrive here over postMessage; a mode
// switch reloads only the iframe and a database switch reopens just PGlite, so
// Pyodide and Flask stay live across both. JSPI (run_sync over async PGlite)
// runs here, in the top frame; the iframe needs none.
//
// Requires a JSPI-capable browser for run_sync. Everything is self-hosted
// (vendor.sh): no third-party CDN at run time.
import { PGlite } from './pglite/index.js'
import { uuid_ossp } from './pglite/contrib/uuid_ossp.js'
import { loadPyodide } from './pyodide/pyodide.mjs'
import { Graphviz } from './vendor/graphviz/index.js'

const PKG = ['__init__.py', 'db.py', 'app.py', 'circuit.py', 'kc.py']
// Resolve sibling assets against this module's URL, not the document's, so the
// page can be served from any path (sub-path deploy, file://).
const asset = (p) => new URL(p, import.meta.url)

// Shareable deep links carry the whole view in the query string:
//   ?mode=circuit|where  &db=<database>  &q=<url-encoded SQL>
// The shell consumes ?db (it owns the connection) and forwards ?mode / ?q to
// the iframe, where child-boot.js applies them.
const params = new URLSearchParams(location.search)
const mode = params.get('mode') === 'where' ? 'where' : 'circuit'
const linkedQuery = params.get('q')

const boot = document.getElementById('studio-boot-status')
const say = (m) => { if (boot) { boot.style.display = 'block'; boot.textContent = m } console.log('[shell-boot]', m) }
// Surface a boot failure in the status bar rather than leaving it stuck on the
// last say(): a rejected top-level await otherwise reads as a silent hang.
const fail = (e) => {
  const m = (e && (e.stack || e.message)) || String(e)
  if (boot) { boot.style.display = 'block'; boot.style.background = '#5a1111'; boot.textContent = 'boot error: ' + m }
  console.error('[shell-boot]', e)
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
// (manifest below). Persistence matters because a database switch reopens the
// connection and a mode switch reloads the iframe; the DBs and their provenance
// circuits must survive (a token carried across a switch must still resolve).
const DATADIR = 'idb://provsql-studio'
const DEFAULT_DB = 'tutorial'
const manifest = await (await fetch(asset('./casestudies/manifest.json'))).json()

// Per-database preparation, idempotent, re-run on every open: the extension,
// the session search_path, and the browser-specific tool handling. External
// knowledge compilers (d4, c2d…) need subprocesses the browser cannot spawn,
// so every registry entry is disabled (they never appear as eval-strip
// options); tool_available is redefined to report only `dot`, which is real
// here – supplied as WASM Graphviz through the subprocess shim, and gated on
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
//
// Opening this bootstrap connection is itself a full PGlite instantiation. Once
// the cluster matches the manifest there is nothing to create, so we record the
// manifest signature in localStorage and skip the bootstrap on later boots.
// switchDb falls back to ensureCluster() if a target database turns out to be
// absent (the IndexedDB store was cleared while the signature lingered, or the
// manifest grew), so the skip is safe even when the two stores disagree.
const CLUSTER_SIG = manifest.map((m) => m.name).sort().join(',')
async function ensureCluster() {
  const b = await PGlite.create({ dataDir: DATADIR, extensions: EXT })
  const have = (await b.query('SELECT datname FROM pg_database')).rows.map((r) => r.datname)
  for (const m of manifest) {
    if (!have.includes(m.name)) await b.exec(`CREATE DATABASE ${m.name}`)
  }
  await b.close()
  localStorage.setItem('ps.clusterReady', CLUSTER_SIG)
}
if (localStorage.getItem('ps.clusterReady') !== CLUSTER_SIG) await ensureCluster()

// The currently-open database. PGlite holds one connection at a time, so
// switching closes the active instance and reopens on the target.
let activeDb = null
let activePg = null
async function switchDb(name) {
  if (name === activeDb && activePg) return
  if (activePg) { await activePg.close(); activePg = null }
  let inst
  try {
    inst = await PGlite.create({ dataDir: DATADIR, database: name, extensions: EXT })
  } catch (_e) {
    // The signature claimed the cluster was ready but this database is missing;
    // rebuild the cluster and retry once.
    await ensureCluster()
    inst = await PGlite.create({ dataDir: DATADIR, database: name, extensions: EXT })
  }
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
  // Persist the choice so a fresh shell (a new tab, or after Reset) reopens the
  // same database rather than snapping back to the default.
  localStorage.setItem('ps.activeDb', name)
}
// A ?db= in the link wins, then the last-used database, then the default.
const _urlDb = params.get('db')
const _saved = localStorage.getItem('ps.activeDb')
await switchDb(
  manifest.some((m) => m.name === _urlDb) ? _urlDb
  : manifest.some((m) => m.name === _saved) ? _saved
  : DEFAULT_DB)

// async PGlite bridge (the shim's run_sync target) -> the active database.
globalThis.pgQuery = async (sql, qparams) => {
  const notices = []
  try {
    const r = await activePg.query(sql, qparams ? Array.from(qparams) : [],
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
const py = await loadPyodide({ indexURL: asset('./pyodide/').href })
await py.loadPackage('micropip')   // micropip + packaging from the local indexURL
say('installing flask + sqlparse…')
// Install the vendored wheel closure (the manifest is the full dependency
// set, so micropip resolves entirely offline -- it never reaches PyPI).
const wheels = await (await fetch(asset('./wheels/manifest.json'))).json()
const wheelUrls = wheels.map((w) => asset('./wheels/' + w).href)
await py.runPythonAsync(`import micropip; await micropip.install(${JSON.stringify(wheelUrls)})`)

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
// per-request BEGIN / SET LOCAL / SAVEPOINT). The UI fires several /api/*
// requests at once (e.g. on first render); via JSPI their Python handlers
// interleave, so one request's COMMIT would close the transaction another is
// mid-way through -> "SET LOCAL can only be used in transaction blocks".
// Running them one at a time makes the shared connection behave like the
// dedicated one the backend expects. switchDb / Reset run on the same chain so
// they never reopen PGlite under an in-flight query.
let _chain = Promise.resolve()
const enqueue = (task) => {
  const r = _chain.then(task, task)
  _chain = r.then(() => {}, () => {})
  return r
}
const callBackend = (method, path, body) => enqueue(() => handle.callPromising(method, path, body))

// Resolve one /api/* request to the {status, body, ctype} envelope the iframe
// rebuilds into a Response. Database list / switch are JS-layer concerns: each
// tutorial / case study is a real database in the one cluster, reached by
// reopening the active PGlite connection; the backend's DSN switching does not
// apply.
async function dispatchApi(method, path, body) {
  if (path === '/api/databases' && method === 'GET') {
    return { status: 200, body: JSON.stringify(manifest.map((m) => m.name)), ctype: 'application/json' }
  }
  if (path.split('?')[0] === '/api/conn' && method === 'POST') {
    let target = null
    try { target = JSON.parse(body || '{}').database } catch (_e) { /* ignore */ }
    // Switch in place (no shell reload) and read back conn_info atomically, so
    // the reply already reflects the new database. The iframe reloads itself
    // afterwards; Pyodide / Flask stay warm.
    return await enqueue(async () => {
      if (target && manifest.some((m) => m.name === target)) await switchDb(target)
      return JSON.parse(await handle.callPromising('GET', '/api/conn', ''))
    })
  }
  return JSON.parse(await callBackend(method, path, body || ''))
}

// Drop every database, rebuild the empty cluster, and reopen the default
// (re-seeding it). Runs on the call chain so no query is in flight.
async function resetData() {
  return enqueue(async () => {
    if (activePg) { try { await activePg.close() } catch (_e) { /* ignore */ } activePg = null }
    const admin = await PGlite.create({ dataDir: DATADIR, extensions: EXT })
    for (const m of manifest) await admin.exec(`DROP DATABASE IF EXISTS ${m.name}`)
    await admin.close()
    localStorage.removeItem('ps.activeDb')
    localStorage.removeItem('ps.clusterReady')
    activeDb = null
    await ensureCluster()
    await switchDb(DEFAULT_DB)
  })
}

// The iframe (child-boot.js) talks to us over postMessage. We verify the
// message source is our own iframe; the payload is local API traffic (no
// secrets), so targetOrigin '*' is fine and keeps file:// working.
const ui = document.createElement('iframe')
ui.id = 'studio-ui'
ui.name = 'studio-ui'
ui.title = 'ProvSQL Playground'
ui.setAttribute('allow', 'clipboard-read; clipboard-write')
window.addEventListener('message', async (ev) => {
  if (ev.source !== ui.contentWindow) return
  const d = ev.data
  if (!d || typeof d !== 'object') return
  if (d.type === 'ready') { if (boot) boot.style.display = 'none'; return }
  if (d.type === 'reset') {
    say('resetting databases…')
    try { await resetData() } catch (e) { fail(e); return }
    ui.contentWindow.location.reload()   // the UI re-reads /api/conn -> pristine default
    return
  }
  if (d.type === 'api') {
    let out
    try { out = await dispatchApi(d.method, d.path, d.body) }
    catch (e) { out = { status: 500, body: String((e && e.message) || e), ctype: 'text/plain' } }
    // Echo the request's epoch so a reload-straddling reply is ignored by the
    // fresh child (see child-boot.js). contentWindow may already be the new
    // child here; the epoch guard makes that harmless.
    ui.contentWindow.postMessage({ type: 'api-reply', id: d.id, epoch: d.epoch, ...out }, '*')
  }
})

// Backend is up: mount the unmodified Studio UI. ?mode / ?q are forwarded; the
// database is already open here (the UI reads it back via /api/conn).
say('starting the Studio UI…')
const uiUrl = new URL(asset('./ui.html'))
uiUrl.searchParams.set('mode', mode)
if (linkedQuery != null) uiUrl.searchParams.set('q', linkedQuery)
ui.src = uiUrl.href
ui.style.cssText = 'position:fixed;inset:0;width:100%;height:100%;border:0'
document.body.appendChild(ui)
