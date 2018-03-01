#!/bin/bash

rm -f /var/www/html/*
cp /opt/provsql/where_panel/* /var/www/html/
sed -i 's/demo/provsql/g' /var/www/html/config
/etc/init.d/apache2 start
/etc/init.d/postgresql start

su - postgres psql -c "ALTER USER \"provsql\" WITH PASSWORD 'provsql';"
while true ; do
    sleep 100
done ;
