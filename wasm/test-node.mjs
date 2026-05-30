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

await pg.close()
console.log(failures ? `\n${failures} FAILURE(S)` : '\nALL OK')
process.exit(failures ? 1 : 0)
