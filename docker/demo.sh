#!/bin/bash

rm -rf /var/www/html/*
cp -r /opt/provsql/where_panel/* /var/www/html/
cp /opt/provsql/docker/view /usr/local/bin/evince
chmod a+rx /usr/local/bin/evince
touch /messages
mkdir -p /var/www/html/pdf
chmod a+rwX /var/www/html/pdf
chmod a+rw /messages

sed -i 's/demo/test/g' /var/www/html/config
sed -i 's/localhost/127.0.0.1/g' /var/www/html/config

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
echo "The where_panel web interface is running and is available  "
echo "at the address           http://${IP} "
echo ""
echo "If you ran 'docker run  -p 8080:80 inriavalda/provsqldemo'"
echo "it is also at            http://localhost:8080 "
echo ""
echo ""
echo "The psql shell should now be available with the command "
echo "                         psql -h ${IP} -p 5432 test test"
echo ""

echo "Docker fully started" >> /messages
sleep 3 ;
#su - postgres psql -c "ALTER USER \"test\" WITH PASSWORD 'test';"
while true ; do
    tail -f /messages | sed -u "s_ /_ http://${IP}/_";
    sleep 1 ;
done ;
