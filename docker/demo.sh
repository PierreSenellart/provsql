#!/bin/bash

rm -rf /var/www/html/*
touch /messages
mkdir -p /var/www/html/pdf
chmod a+rwX /var/www/html/pdf
chmod a+rw /messages

IP=$(ip addr |grep -v 127.0.0 | sed -n 's_^.*inet \(.*\)/.* brd.*$_\1_p')

echo "CONTAINER IP: ${IP}"
echo ""
echo ""
echo " =====  Starting Postgres ... "
/etc/init.d/postgresql start
echo ""
echo ""
echo " =====  Starting Apache Web Server... "
/etc/init.d/apache2 start
echo ""
echo ""

echo -n "Waiting for PostgreSQL (this can take a few minutes)..." ;
while [[ $( psql -c "SELECT md5('Hello');" test test 2>/dev/null| grep -c '8b1a9953c4611296a827abf8c47804d7') -ne 1 ]] ;
do
    echo -n ".";
    sleep 1 ;
done ;

echo ""
echo ""
echo ""
echo "The psql shell should now be available with the command "
echo "  psql -h ${IP} -p 5432 test test"
echo ""
echo ""
echo "Docker fully started" >> /messages
sleep 3 ;
#su - postgres psql -c "ALTER USER \"test\" WITH PASSWORD 'test';"
while true ; do
    tail -f /messages | sed -u "s_ /_ http://${IP}/_";
    sleep 1 ;
done ;
