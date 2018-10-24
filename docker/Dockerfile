FROM debian:latest
LABEL maintainer pierre@senellart.com

# needed to build provsql the tools + libc6-i386 for running c2d
RUN apt-get update &&\
    apt-get -y install git build-essential curl\
               libc6-i386 unzip mercurial libgmp-dev

# Specify which version we are building against
ARG PSQL_VERSION=9.6
ENV PSQL_VERSION=$PSQL_VERSION

RUN apt-get update &&\
    apt-get -y install  postgresql-${PSQL_VERSION} postgresql-server-dev-${PSQL_VERSION} postgresql-contrib-${PSQL_VERSION}


# Ensure a sane environment
ENV LANG=C.UTF-8 LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive

# Ensure that bash is the default shell
ENV SHELL=/bin/bash


############################## GETTING ADD-ON TOOLS ##############################   

RUN mkdir /tmp/tools/

# TOOL c2d
RUN curl 'http://reasoning.cs.ucla.edu/c2d/fetchme.php' \
         -H 'Content-Type: application/x-www-form-urlencoded'\
         --data 'os=Linux+i386&type=&s=&n=Pierre+Senellart+DOCKER&e=pierre@senellart.com&o=ENS'\
         -o /tmp/c2d.zip &&\
         unzip /tmp/c2d.zip -d /tmp/ &&\
         rm /tmp/c2d.zip &&\
         mv /tmp/c2d_linux /tmp/tools/c2d &&\
         chmod a+x /tmp/tools/c2d

# TOOL d4
RUN curl http://www.cril.univ-artois.fr/KC/ressources/d4 -o /tmp/tools/d4 &&\
    chmod a+x /tmp/tools/d4

# TOOL dsharp
RUN cd /tmp/ &&\
    hg clone https://bitbucket.org/haz/dsharp &&\
    cd dsharp &&\
    mv Makefile_gmp Makefile &&\
    make &&\
    mv dsharp /tmp/tools/ &&\
    chmod a+x /tmp/tools/dsharp
    
# mv the additional tools
RUN bash -c "mv /tmp/tools/* /usr/local/bin"

##############################   GETTING  PROVSQL  ##############################   

# Add a folder for ProvSQL
WORKDIR /opt/provsql

# Provsql will be built & run as user postgres
RUN chown postgres:postgres /opt/provsql
USER postgres

# Getting and building
RUN git clone https://github.com/PierreSenellart/provsql.git /opt/provsql
RUN cd /opt/provsql && make

# install provsql
USER root
RUN echo "shared_preload_libraries = 'provsql'" >> /etc/postgresql/${PSQL_VERSION}/main/postgresql.conf
RUN echo "local all all trust" > /etc/postgresql/${PSQL_VERSION}/main/pg_hba.conf  

EXPOSE 5432

RUN cd /opt/provsql ; make install

USER postgres
#create a db test
RUN /etc/init.d/postgresql start &&\
    createuser -s test &&\
    createdb test &&\
    psql -f /opt/provsql/test/sql/setup.sql test test  &&\
    psql -f /opt/provsql/test/sql/add_provenance.sql test test &&\
    psql -f /opt/provsql/test/sql/formula.sql test test  &&\
    psql -f /opt/provsql/test/sql/security.sql test test &&\
    psql -c "ALTER ROLE test SET search_path TO public, provsql";     

############################## FINISHING THE DOCKER  ##############################   

#allow access
RUN echo "listen_addresses = '*'"  >> /etc/postgresql/${PSQL_VERSION}/main/postgresql.conf
RUN echo "host all all 0.0.0.0/0 trust" >> /etc/postgresql/${PSQL_VERSION}/main/pg_hba.conf  
RUN echo "host all all ::/0 trust" >> /etc/postgresql/${PSQL_VERSION}/main/pg_hba.conf  

USER postgres
CMD /usr/bin/pg_ctlcluster $PSQL_VERSION main start --foreground

USER root
RUN apt-get update &&\
    apt-get -y install apache2 libapache2-mod-php7.0 php7.0-pgsql graphviz

CMD /bin/bash /opt/provsql/docker/demo.sh
EXPOSE 80
