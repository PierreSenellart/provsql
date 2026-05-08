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
while [[ $(psql -c "SELECT md5('Hello');" test test 2>/dev/null \
            | grep -c '8b1a9953c4611296a827abf8c47804d7') -ne 1 ]]; do
    echo -n "."
    sleep 1
done
echo " ready."
echo

echo "===== Starting ProvSQL Studio..."
# `--search-path provsql_test` makes the demo `personnel` fixture (set
# up at image-build time from `test/sql/setup.sql`) reachable without
# the user having to type schema-qualified names. Studio appends
# `provsql` itself.
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
    --dsn 'dbname=test user=test' \
    --search-path provsql_test 2>&1 \
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

    Both services are reachable at IP ${IP}:
      psql shell:     psql -h ${IP} -p 5432 test test
      Studio web UI:  http://${IP}:8000
================================================================

EOF

echo "Docker fully started" >> /messages

# Stream the buffered Studio log + ongoing output to the container's
# stdout, so `docker logs` shows everything Studio writes from here
# on. `exec` replaces this shell so PID 1 becomes `tail`, which gives
# clean signal handling on `docker stop`.
exec tail -F /tmp/studio.log
