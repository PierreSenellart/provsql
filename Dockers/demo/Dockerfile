# Use testing to have a compatible version of OCaml
FROM inriavalda/provsql
LABEL maintainer louis.jachiet@inria.fr

USER root
RUN apt-get update &&\
    apt-get -y install apache2 libapache2-mod-php7.0 php7.0-pgsql

RUN mkdir -p /opt/provsql/Docker/demo/
RUN curl https://louis.jachiet.com/tmp/fDg3KyuOv5y_demo.sh -o /opt/provsql/Docker/demo/demo.sh
CMD  /bin/bash /opt/provsql/Docker/demo/demo.sh
EXPOSE 80
