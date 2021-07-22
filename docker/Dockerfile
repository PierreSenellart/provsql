FROM debian:buster
LABEL maintainer pierre@senellart.com

# Copy the source into /opt/provsql
COPY . /opt/provsql
WORKDIR /opt/provsql

# needed to build provsql the tools + libc6-i386 for running c2d
RUN apt-get update &&\
    apt-get -y install git build-essential curl\
               libc6-i386 unzip mercurial libgmp-dev libboost-dev

# Specify which version we are building against
ARG PSQL_VERSION=11
ENV PSQL_VERSION=$PSQL_VERSION

RUN apt-get update &&\
    apt-get -y install zlib1g-dev postgresql-${PSQL_VERSION} postgresql-server-dev-${PSQL_VERSION}

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
RUN cd /tmp/ &&\
    git clone https://github.com/crillab/d4.git &&\
    cd d4 &&\
    make &&\
    mv d4 /tmp/tools

# TOOL dsharp
RUN cd /tmp/ &&\
    git clone https://github.com/QuMuLab/dsharp.git &&\ 
    cd dsharp &&\
    mv Makefile_gmp Makefile &&\
    make &&\
    mv dsharp /tmp/tools/ &&\
    chmod a+x /tmp/tools/dsharp

# TOOL weightmc
RUN cd /tmp &&\
    git clone https://bitbucket.org/kuldeepmeel/weightmc/src/master/ weightmc &&\
    cd weightmc/wmc-src &&\
    ./configure &&\
    make &&\
    mv weightmc /tmp/tools &&\
    chmod a+x /tmp/tools/weightmc
    
# mv the additional tools
RUN bash -c "mv /tmp/tools/* /usr/local/bin"

##############################   GETTING  PROVSQL  ##############################   

# Provsql will be built & run as user postgres
RUN chown -R postgres:postgres /opt/provsql
USER postgres

# Building
RUN make

# install provsql
USER root
RUN echo "shared_preload_libraries = 'provsql'" >> /etc/postgresql/${PSQL_VERSION}/main/postgresql.conf
RUN echo "local all all trust" > /etc/postgresql/${PSQL_VERSION}/main/pg_hba.conf  

EXPOSE 5432

RUN make install

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
    apt-get -y install apache2 libapache2-mod-php php-pgsql graphviz libgraph-easy-perl

CMD /bin/bash /opt/provsql/docker/demo.sh
EXPOSE 80
