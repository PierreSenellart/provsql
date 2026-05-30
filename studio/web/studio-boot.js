// Boot the full ProvSQL Studio in the browser, client-side:
//   PGlite (Postgres+provsql, WASM)  +  Pyodide (the UNMODIFIED Studio
//   Python)  +  a psycopg->PGlite shim  +  a fetch->Flask-test_client shim.
// Then inject the real static/app.js, which runs against the in-page backend.
//
// Requires a JSPI-capable browser (recent Chrome/Edge) for run_sync.
import { PGlite } from './pglite/index.js'
import { uuid_ossp } from './pglite/contrib/uuid_ossp.js'
import { loadPyodide } from 'https://cdn.jsdelivr.net/pyodide/v0.29.4/full/pyodide.mjs'

const PKG = ['__init__.py', 'db.py', 'app.py', 'circuit.py', 'kc.py']
const boot = document.getElementById('studio-boot-status')
const say = (m) => { if (boot) boot.textContent = m; console.log('[studio-boot]', m) }

say('starting PostgreSQL + ProvSQL (WASM)…')
const provsql = {
  name: 'provsql',
  setup: async (_p, o) => ({ emscriptenOpts: o, bundlePath: new URL('./provsql.tar.gz', import.meta.url) }),
}
const pg = await PGlite.create({ extensions: { uuid_ossp, provsql } })
await pg.exec('CREATE EXTENSION provsql CASCADE')
await pg.exec('SET search_path TO provsql, public')
// demo data
await pg.exec(`CREATE TABLE IF NOT EXISTS personnel(id int, name text, city text, classification text);
  INSERT INTO personnel VALUES (1,'John','New York','unclassified'),(2,'Paul','New York','restricted'),
  (3,'Dave','Paris','confidential'),(4,'Ellen','Berlin','secret'),(5,'Magdalen','Paris','top_secret'),
  (6,'Nancy','Paris','restricted'),(7,'Susan','Berlin','secret');`)
await pg.query("SELECT add_provenance('personnel'::regclass)")
await pg.query('SELECT set_prob(provenance(), 0.5) FROM personnel')

// async PGlite bridge (the shim's run_sync target), with NOTICE capture.
globalThis.pgQuery = async (sql, params) => {
  const notices = []
  try {
    const r = await pg.query(sql, params ? Array.from(params) : [],
      { onNotice: n => notices.push({ severity: n.severity || 'NOTICE', message_primary: n.message || '',
                                      message_detail: n.detail || '', message_hint: n.hint || '' }) })
    return { ok: true, rows: r.rows, fields: r.fields, affected: r.affectedRows ?? 0, notices }
  } catch (e) { return { ok: false, message: String(e.message || e), notices } }
}

say('loading Python (Pyodide)…')
const py = await loadPyodide()
await py.loadPackage('micropip')
say('installing flask + sqlparse…')
await py.runPythonAsync(`import micropip; await micropip.install(['flask','sqlparse'])`)

say('wiring the Studio backend…')
py.runPython(`import os; os.environ['PROVSQL_STUDIO_CONFIG_DIR']='/cfg'; os.makedirs('/cfg', exist_ok=True)`)
py.runPython(await (await fetch('./psycopg_pglite.py')).text())   // registers fake psycopg

py.FS.mkdir('/studio'); py.FS.mkdir('/studio/provsql_studio')
for (const f of PKG) {
  py.FS.writeFile(`/studio/provsql_studio/${f}`, await (await fetch(`./pkg/${f}`)).text())
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

// Route the real frontend's /api/* fetches into the in-page Flask app.
const realFetch = window.fetch.bind(window)
window.fetch = async (input, init = {}) => {
  const url = typeof input === 'string' ? input : input.url
  const path = url.replace(location.origin, '')
  if (path.startsWith('/api/')) {
    const method = (init.method || (typeof input !== 'string' && input.method) || 'GET').toUpperCase()
    const body = init.body || null
    const out = JSON.parse(await handle.callPromising(method, path, body ? String(body) : ''))
    return new Response(out.body, { status: out.status, headers: { 'Content-Type': out.ctype } })
  }
  return realFetch(input, init)
}

say('starting the Studio UI…')
await new Promise((resolve) => {
  const s = document.createElement('script')
  s.src = 'app.js'; s.onload = resolve; document.body.appendChild(s)
})
if (boot) boot.style.display = 'none'
