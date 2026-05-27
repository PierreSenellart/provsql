#!/bin/sh
# Run a command (typically `make installcheck`) with a tdkc KCMCP server
# available, so the kcmcp_client_endpoint / kcmcp_client_managed regression
# tests exercise the real socket round-trip instead of skipping.
#
# It best-effort provisions BOTH client modes and tears them down on exit:
#
#   endpoint mode  a tdkc server this script starts, on a fixed Unix socket
#                  the test connects to directly; chmod 0777 so the postgres
#                  backend (possibly a different OS user than whoever runs the
#                  tests) can connect.  The test guards on the socket existing.
#
#   managed mode   a postgres-reachable copy of the tdkc binary, so the
#                  kcmcp_client_managed test can set provsql.kcmcp_server to
#                  it (ALTER SYSTEM + reload) and let ProvSQL's supervisor
#                  worker launch/own the server.  The test guards on that
#                  binary existing and resets the GUC when done.
#
# Provisioning is best-effort: if tdkc is missing or unreachable, the wrapped
# command still runs and both tests take their skip branch (so this is always
# safe to wrap around installcheck, including where the backend's OS user
# cannot reach the build tree).  A bare `make installcheck` with no wrapper
# likewise skips.
#
# Env: TDKC overrides the server binary (default ./tdkc, i.e. `make tdkc`).
set -u

SOCK=/tmp/.provsql-kcmcp-regress.sock     # endpoint-mode socket (contract with the .sql)
MBIN=/tmp/tdkc-regress                    # managed-mode binary  (contract with the .sql)
TDKC="${TDKC:-./tdkc}"
TDKC_PID=""

cleanup() {
  [ -n "$TDKC_PID" ] && kill "$TDKC_PID" 2>/dev/null
  rm -f "$SOCK" "$MBIN"
  # Best-effort GUC reset in case a crashed test left it set (needs a working
  # psql connection from the environment; silently skipped otherwise).
  psql -X -q -c "ALTER SYSTEM RESET provsql.kcmcp_server" >/dev/null 2>&1 || true
  psql -X -q -c "SELECT pg_reload_conf()" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

if [ -x "$TDKC" ]; then
  # Managed mode: a copy the postgres OS user can exec (the build tree under
  # $HOME is typically unreadable to postgres).
  cp -f "$TDKC" "$MBIN" 2>/dev/null && chmod 0755 "$MBIN" 2>/dev/null || true
  # Endpoint mode: a server we own, on the fixed socket.
  rm -f "$SOCK"
  "$TDKC" --kcmcp "unix:$SOCK" >/dev/null 2>&1 &
  TDKC_PID=$!
  i=0
  while [ ! -S "$SOCK" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
  chmod 0777 "$SOCK" 2>/dev/null || true
else
  echo "with-tdkc.sh: $TDKC not executable; the KCMCP client tests will skip." >&2
fi

exec "$@"
