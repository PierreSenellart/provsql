// Headless smoke test for the WASM build: load provsql into the matched
// PGlite and assert the full stack (provenance rewriting + in-process
// store + probability) produces correct results.  Exits non-zero on
// failure so CI can gate on it.
//
//   PGLITE_DIST     dir containing the built PGlite package (index.js,
//                   contrib/uuid_ossp.js, pglite.wasm, ...)
//   PROVSQL_TARBALL path to provsql.tar.gz
import { pathToFileURL } from 'node:url'

const dist = process.env.PGLITE_DIST || './pglite'
const tarball = process.env.PROVSQL_TARBALL || './provsql.tar.gz'

const { PGlite } = await import(`${dist}/index.js`)
const { uuid_ossp } = await import(`${dist}/contrib/uuid_ossp.js`)

const provsql = {
  name: 'provsql',
  setup: async (_pg, o) => ({ emscriptenOpts: o, bundlePath: pathToFileURL(tarball) }),
}

let failures = 0
const check = (cond, msg) => { console.log((cond ? 'ok   ' : 'FAIL ') + msg); if (!cond) failures++ }

const pg = await PGlite.create({ extensions: { uuid_ossp, provsql } })
const v = (await pg.query('select version()')).rows[0].version
check(/wasm32/.test(v), 'PGlite is the wasm32 build: ' + v.split(',')[0])

await pg.exec('CREATE EXTENSION provsql CASCADE')
const ext = (await pg.query("select extname from pg_extension where extname='provsql'")).rows
check(ext.length === 1, 'CREATE EXTENSION provsql')

await pg.exec('SET search_path TO provsql, public')
await pg.exec('CREATE TABLE r(a int, b int); INSERT INTO r VALUES (1,2),(3,4),(1,5)')
await pg.query("SELECT add_provenance('r'::regclass)")
const rows = (await pg.query('SELECT a, provenance() IS NOT NULL AS p FROM r')).rows
check(rows.length === 3 && rows.every((r) => r.p === true), 'planner hook attaches provenance tokens')

await pg.query('SELECT set_prob(provenance(), 0.5) FROM r')
const pr = (await pg.query(
  'SELECT a, round(probability_evaluate(provenance())::numeric,4) AS prob FROM r GROUP BY a ORDER BY a')).rows
const byA = Object.fromEntries(pr.map((r) => [r.a, Number(r.prob)]))
check(byA[1] === 0.75, 'probability_evaluate: two-tuple OR-group a=1 = 0.75 (got ' + byA[1] + ')')
check(byA[3] === 0.5, 'probability_evaluate: singleton a=3 = 0.5 (got ' + byA[3] + ')')

// C++ exceptions must be caught inside the extension.  The truncated-normal
// moments go through a throw-and-catch, and an infeasible conditioning event
// is a C++ exception that must come back as an SQL error, leaving the session
// usable; with exception catching disabled the first returns no row and the
// second breaks the backend (ERRORDATA_STACK_SIZE exceeded a few calls later).
const tn = (await pg.query(
  'WITH r AS (SELECT normal(20, 5) AS x) ' +
  'SELECT round(expected(x | (x > 25))::numeric, 4) AS e, ' +
  'round(variance(x | (x > 25))::numeric, 4) AS v FROM r')).rows
check(tn.length === 1 && Number(tn[0].e) === 27.6257 && Number(tn[0].v) === 4.9774,
  'conditioned normal: E = 27.6257, Var = 4.9774 (got ' + JSON.stringify(tn) + ')')
// The normal CDF and the Monte Carlo sampler: libm functions only the
// distribution families call, and a template member referenced from files
// that do not include its definition, both once left unresolved here.
const pn = (await pg.query(
  'WITH r AS (SELECT normal(20, 5) AS x) ' +
  'SELECT round(probability(x > 25)::numeric, 4) AS p FROM r')).rows
check(pn.length === 1 && Number(pn[0].p) === 0.1587,
  'P(Normal(20, 5) > 25) = 0.1587 (got ' + JSON.stringify(pn) + ')')
let infeasible = ''
try {
  await pg.query('WITH r AS (SELECT uniform(0, 1) AS x) SELECT expected(x | (x > 2)) FROM r')
} catch (e) { infeasible = e.message }
check(/infeasible/.test(infeasible), 'C++ exception surfaces as an SQL error: ' + infeasible)
const alive = (await pg.query('SELECT 1 AS one')).rows
check(alive.length === 1 && alive[0].one === 1, 'session still usable after the error')

await pg.close()
console.log(failures ? `\n${failures} FAILURE(S)` : '\nALL OK')
process.exit(failures ? 1 : 0)
