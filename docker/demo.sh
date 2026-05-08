#!/bin/bash

touch /messages
chmod a+rw /messages

IP=$(ip addr |grep -v 127.0.0 | sed -n 's_^.*inet \(.*\)/.* brd.*$_\1_p')

echo "CONTAINER IP: ${IP}"
echo ""
echo ""
echo " =====  Starting Postgres ... "
/etc/init.d/postgresql start
echo ""

echo -n "Waiting for PostgreSQL (this can take a few minutes)..." ;
while [[ $( psql -c "SELECT md5('Hello');" test test 2>/dev/null| grep -c '8b1a9953c4611296a827abf8c47804d7') -ne 1 ]] ;
do
    echo -n ".";
    sleep 1 ;
done ;

echo ""
echo " =====  Starting ProvSQL Studio... "
# `--search-path provsql_test` makes the demo `personnel` fixture (set
# up at image-build time from `test/sql/setup.sql`) reachable without
# the user having to type schema-qualified names. Studio appends
# `provsql` itself.
provsql-studio \
    --host 0.0.0.0 \
    --port 8000 \
    --dsn 'dbname=test user=test' \
    --search-path provsql_test &
echo ""

echo ""
echo "The psql shell should now be available with the command "
echo "  psql -h ${IP} -p 5432 test test"
echo ""
echo "ProvSQL Studio is also running and available at"
echo "  http://${IP}:8000"
echo ""

echo "Docker fully started" >> /messages

# Replace this shell with `tail -f` so PID 1 stays alive and any future
# writes to /messages stream to the container's stdout. `exec` matters:
# without it, an SIGTERM (`docker stop`) would kill `tail` but leave
# this shell pondering, which makes shutdown slow.
exec tail -f /messages
