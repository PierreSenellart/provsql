#!/usr/bin/env bash
#
# Run the ProvSQL regression suite against a throwaway, user-owned PostgreSQL
# cluster that loads the freshly built *instrumented* extension, then build the
# coverage reports. Driven by `make coverage`.
#
# Nothing is installed into the system PostgreSQL. The instrumented extension is
# staged with `make install DESTDIR=...` (a private prefix under /tmp) and a
# throwaway cluster is pointed at it via extension_control_path /
# dynamic_library_path / an absolute shared_preload_libraries. This needs
# PostgreSQL >= 18 (extension_control_path). The dedicated cluster also runs as
# the invoking user, so gcov's .gcda files (written next to the .gcno under
# src/) are writable; loads the instrumented build from startup; sets
# track_functions=all; and is stopped cleanly so the postmaster / background
# worker counters flush before gcovr runs.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

BIN="$(pg_config --bindir)"
SHAREDIR="$(pg_config --sharedir)"
PKGLIBDIR="$(pg_config --pkglibdir)"
PGDIR="${PROVSQL_COVERAGE_DIR:-/tmp/provsql_coverage}"
STAGE="${PROVSQL_COVERAGE_STAGE:-/tmp/provsql_coverage_stage}"
PGPORT="${PROVSQL_COVERAGE_PORT:-54321}"
GCOVR="${GCOVR:-gcovr}"
REGRESS_DB=contrib_regression
USER_NAME="$(id -un)"
MAKE="${MAKE:-make}"

command -v "$GCOVR" >/dev/null 2>&1 || { echo "ERROR: gcovr not found (try: pipx install gcovr)"; exit 1; }

PG_MAJOR="$(pg_config --version | sed -E 's/[^0-9]*([0-9]+).*/\1/')"
if [ "${PG_MAJOR:-0}" -lt 18 ]; then
  echo "ERROR: 'make coverage' needs PostgreSQL >= 18 (for extension_control_path)" >&2
  echo "       to load the instrumented extension without a system install." >&2
  echo "       This server is PostgreSQL $PG_MAJOR." >&2
  exit 1
fi

EXTDIR="$STAGE$SHAREDIR/extension"
LIBDIR="$STAGE$PKGLIBDIR"

stop_cluster() { "$BIN/pg_ctl" -D "$PGDIR" stop -m fast -w >/dev/null 2>&1 || true; }
cleanup() { stop_cluster; rm -rf "$PGDIR" "$STAGE"; }
trap cleanup EXIT

# Fresh cluster, fresh staging, fresh execution data.
[ -d "$PGDIR" ] && { "$BIN/pg_ctl" -D "$PGDIR" stop -m immediate >/dev/null 2>&1 || true; }
rm -rf "$PGDIR" "$STAGE"
rm -f src/*.gcda src/semiring/*.gcda src/distributions/*.gcda

# Stage the instrumented extension into a private prefix (no sudo, no system
# install). COVERAGE=1 keeps the build instrumented even if anything relinks.
echo "── staging instrumented extension under $STAGE ──"
"$MAKE" -f Makefile.internal COVERAGE=1 install DESTDIR="$STAGE" with_llvm=no >/dev/null
[ -f "$EXTDIR/provsql.control" ] || { echo "ERROR: staged extension not found under $STAGE" >&2; exit 1; }
# Point the control file's C module at the staged (instrumented) .so so the
# extension's functions load it rather than the system $libdir/provsql.
sed -i "s#\$libdir/provsql#$LIBDIR/provsql#" "$EXTDIR/provsql.control"

echo "── initialising coverage cluster in $PGDIR (user $USER_NAME, port $PGPORT) ──"
"$BIN/initdb" -D "$PGDIR" -U "$USER_NAME" --auth=trust --no-sync >/dev/null
cat >> "$PGDIR/postgresql.conf" <<EOF
extension_control_path = '$STAGE$SHAREDIR:\$system'
dynamic_library_path = '$LIBDIR:\$libdir'
shared_preload_libraries = '$LIBDIR/provsql.so'
track_functions = 'all'
port = $PGPORT
unix_socket_directories = '$PGDIR'
listen_addresses = ''
fsync = off
full_page_writes = off
EOF
"$BIN/pg_ctl" -D "$PGDIR" -l "$PGDIR/server.log" -w start >/dev/null \
  || { echo "ERROR: cluster failed to start:"; tail -n 20 "$PGDIR/server.log"; exit 1; }

export PGHOST="$PGDIR" PGPORT PGUSER="$USER_NAME"

# Generate the schedule, then drop extension_upgrade from THIS run: it does
# DROP EXTENSION, which purges pg_stat_user_functions' per-function call counts
# (they are keyed by OID), discarding the stats the rest of the suite built up.
# Its C/C++ lines are covered by the rest of the suite anyway.
"$MAKE" -f Makefile.internal test/schedule with_llvm=no >/dev/null
sed -i '/^test: extension_upgrade$/d' test/schedule

# Run the suite (installcheck mode, under the tdkc supervisor for the kcmcp
# client tests). pg_regress connects to the temp cluster through PGHOST/PGPORT.
echo "── running regression suite against the coverage cluster ──"
set +e
bash test/kcmcp/with-tdkc.sh "$MAKE" -f Makefile.internal installcheck with_llvm=no \
  EXTRA_REGRESS_OPTS="--host=$PGDIR --port=$PGPORT" 2>&1 | tee test.log
rc=${PIPESTATUS[0]}
set -e

mkdir -p coverage

# Never-called functions, from PostgreSQL's own call counts (cluster still up).
if psql -X -At -d "$REGRESS_DB" -f test/coverage/zero_call_functions.sql \
     > coverage/zero_call.txt 2>/dev/null; then
  echo "── coverage/zero_call.txt: $(wc -l < coverage/zero_call.txt) provsql functions never called ──"
else
  echo "WARNING: could not read function-call stats from $REGRESS_DB" >&2
fi

# Clean stop flushes the postmaster / bgworker .gcda before gcovr reads them.
stop_cluster
trap - EXIT
rm -rf "$PGDIR" "$STAGE"

echo "── building gcovr report ──"
"$GCOVR" --root . --filter 'src/' --html-details coverage/index.html --print-summary

echo
echo "C/C++ line+branch report: coverage/index.html"
echo "Never-called functions:   coverage/zero_call.txt"
exit "$rc"
