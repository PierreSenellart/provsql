# Test coverage

`make coverage` measures how well the test suite exercises ProvSQL, at two
granularities:

- **Function-call coverage (plpgsql + C).** With `track_functions = all`, the
  server records a call count per function in `pg_stat_user_functions`. After
  the run, `coverage/zero_call.txt` lists `provsql` plpgsql/C functions with
  zero calls (from `zero_call_functions.sql`). The coverage run drops the
  `extension_upgrade` test from the schedule, because its `DROP EXTENSION` would
  purge these counters (they are keyed by function OID). `LANGUAGE sql`
  functions are excluded entirely — inlined SQL functions are never counted, so
  they would always look uncalled. The plpgsql rows are reliable; a few C rows
  (type I/O, casts, functions called only via `DirectFunctionCall`/SPI from the
  C core) can still appear despite being exercised, so cross-check a C entry
  against the gcovr line report.
- **C/C++ line and branch coverage.** The extension is rebuilt with gcov
  instrumentation (`--coverage`, LTO off, `-O0`); `gcovr` then produces a
  per-file line **and branch** report under `coverage/` (open `index.html`).

`make coverage` needs [`gcovr`](https://gcovr.com/). Install it isolated with
`pipx install gcovr` (a system `gcovr` can clash with an unrelated `jinja2` in
`~/.local`); override with `make coverage GCOVR=/path/to/gcovr` if needed.

## How it works

Measuring coverage against a shared PostgreSQL server is awkward: provsql is in
`shared_preload_libraries`, so the postmaster holds the loaded `.so` (an
instrumented build only takes effect after a restart); the gcov `.gcda` files
are written by the backend processes next to the `.gcno` under `src/`, so the
server must run as the user who built the tree; the long-lived postmaster /
background-worker counters only flush on a clean stop; and installing an
instrumented build system-wide needs root and disturbs the running server.

`test/coverage/run-coverage.sh` sidesteps all of that. It stages the
instrumented extension into a **private prefix** under `/tmp` with
`make install DESTDIR=...` (no sudo, nothing written to the system PostgreSQL),
then runs the suite against a **throwaway cluster it creates under
`/tmp/provsql_coverage`, owned by whoever runs `make coverage`**. The cluster is
pointed at the staged build with `extension_control_path` (for `CREATE
EXTENSION`), `dynamic_library_path`, and an absolute `shared_preload_libraries`;
the staged control file's `module_pathname` is rewritten to the staged `.so` so
the extension's C functions load the instrumented copy. It sets
`track_functions=all`, runs the regression schedule (`installcheck`, under the
tdkc supervisor), and is stopped cleanly so every `.gcda` is flushed before
`gcovr` runs.

This relies on `extension_control_path`, so it needs **PostgreSQL >= 18**.
Override the cluster/staging location or port with `PROVSQL_COVERAGE_DIR` /
`PROVSQL_COVERAGE_STAGE` / `PROVSQL_COVERAGE_PORT`.

## After a run

Nothing is installed into the system PostgreSQL, so there is nothing to undo
there. The local build tree is left **instrumented**; rebuild the normal
optimised objects when you next need them with:

```sh
make
```

## Troubleshooting

- **gcovr reports 0 % everywhere / no `.gcda`.** The backends could not write
  `.gcda` (wrong user) or ran an uninstrumented library. The temp-cluster script
  avoids both; if you measure by hand against your own server instead, it must
  run the instrumented build and run as the user who owns `src/`.
- **`ImportError: cannot import name 'Markup' from 'jinja2'`.** An old `gcovr`
  against `jinja2 >= 3.1`. Use a `pipx`-installed `gcovr` and point the target
  at it with `GCOVR=`.

## plpgsql line/branch coverage (not wired in)

`pg_stat_user_functions` gives plpgsql functions only a call count, not
statement coverage. For per-statement / per-branch plpgsql coverage, install
[`plpgsql_check`](https://github.com/okbob/plpgsql_check) and use its profiler
(`plpgsql_coverage_statements()`, `plpgsql_coverage_branches()`) over the
regression database after a run.
