#!/bin/bash
#
# Container entrypoint: brings up PostgreSQL + ProvSQL Studio, waits for
# both to be reachable, prints a one-block summary, and keeps PID 1 alive
# by tailing the sentinel log.

touch /messages
chmod a+rw /messages

IP=$(ip addr | grep -v 127.0.0 | sed -n 's_^.*inet \(.*\)/.* brd.*$_\1_p')

echo "===== Starting PostgreSQL..."
/etc/init.d/postgresql start
echo -n "Waiting for PostgreSQL..."
while [[ $(psql -c "SELECT md5('Hello');" postgres test 2>/dev/null \
            | grep -c '8b1a9953c4611296a827abf8c47804d7') -ne 1 ]]; do
    echo -n "."
    sleep 1
done
echo " ready."
echo

echo "===== Starting ProvSQL Studio..."
# Studio lands on the `tutorial` database; the connection chip switches to the
# case studies (cs1, cs2, cs4..cs7). The case-study schemas live in `public`,
# so `--search-path public` lets unqualified table names resolve (Studio
# appends `provsql` itself).
#
# Studio's stdout/stderr is piped through `grep -v` to drop Werkzeug's
# stock "development server" warning, then accumulated in a small
# /tmp ring file while we poll for readiness. The summary block
# below prints next, and only then does the file get streamed to the
# container's stdout (so `docker logs` shows summary first, then
# Studio's "Running on ..." lines, instead of interleaving).
provsql-studio \
    --host 0.0.0.0 \
    --port 8000 \
    --dsn 'dbname=tutorial user=test' \
    --search-path public 2>&1 \
    | grep -v --line-buffered 'This is a development server' \
    > /tmp/studio.log &

echo -n "Waiting for ProvSQL Studio..."
while ! curl -sf "http://127.0.0.1:8000/" > /dev/null 2>&1; do
    echo -n "."
    sleep 1
done
echo " ready."

cat <<EOF

================================================================
  ProvSQL container ready

  PostgreSQL listens on port 5432 and ProvSQL Studio on port 8000 inside
  the container. Publish them to free host ports of your choice with -p,
  e.g. (5433/8001 avoid clashing with a local PostgreSQL/Studio):

      docker run -p 5433:5432 -p 8001:8000 inriavalda/provsql

  then reach them on those host ports:
      psql shell:     psql -h localhost -p 5433 tutorial test
      Studio web UI:  http://localhost:8001

  Databases (the same set as the ProvSQL Playground): tutorial, cs1, cs2,
  cs4, cs5, cs6, cs7. Studio lands on 'tutorial'; switch between them from
  its connection chip, or connect psql directly, e.g.
  psql -h localhost -p 5433 cs1 test.

  (On a native-Linux Docker bridge the container is also reachable
   directly at IP ${IP}; under Docker Desktop or rootless podman, use
   the published localhost ports above.)
================================================================

EOF

echo "Docker fully started" >> /messages

# Stream the buffered Studio log + ongoing output to the container's
# stdout, so `docker logs` shows everything Studio writes from here on.
# We must NOT `exec tail` here: as PID 1 `tail` installs no signal handler
# and the kernel applies no default action to PID 1, so SIGINT (Ctrl-C) and
# SIGTERM (`docker stop`) would be ignored and only SIGKILL could stop the
# container. Instead keep the shell as PID 1 with a trap, so Ctrl-C / stop
# shut PostgreSQL down cleanly and exit promptly.
trap 'echo; /etc/init.d/postgresql stop >/dev/null 2>&1; exit 0' INT TERM
tail -F /tmp/studio.log &
wait $!
