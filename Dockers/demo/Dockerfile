# Use testing to have a compatible version of OCaml
FROM inriavalda/provsql
LABEL maintainer louis.jachiet@inria.fr

USER root
RUN apt-get update &&\
    apt-get -y install apache2 libapache2-mod-php7.0 php7.0-pgsql graphviz

RUN mkdir -p /opt/provsql/Docker/demo/ &&\
    curl https://louis.jachiet.com/tmp/iTRiNmVTOxJ_demo.sh -o /opt/provsql/Docker/demo/demo.sh &&\
    curl https://louis.jachiet.com/tmp/o600DIKLY1f_view -o /opt/provsql/Docker/demo/view &&\
    cp /opt/provsql/Docker/demo/view /usr/local/bin/evince  &&\
    chmod a+rx /usr/local/bin/evince &&\
    touch /messages &&\
    mkdir -p /var/www/html/pdf &&\
    chmod a+rwX /var/www/html/pdf &&\
    chmod a+rw /messages
CMD  /bin/bash /opt/provsql/Docker/demo/demo.sh
EXPOSE 80
