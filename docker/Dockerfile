FROM debian:bookworm

ARG PROVSQL_VERSION=dev
ARG PSQL_VERSION=17

LABEL maintainer=pierre@senellart.com
LABEL org.opencontainers.image.version="${PROVSQL_VERSION}"

# Ensure a sane environment
ENV LANG=C.UTF-8 LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive
ENV SHELL=/bin/bash
ENV PSQL_VERSION=$PSQL_VERSION

# Single apt-get install for: ProvSQL build deps, external-tool clone
# deps, the postgresql server matching $PSQL_VERSION, demo.sh
# utilities (iproute2 for `ip`), the Studio runtime stack
# (python3 + venv + graphviz), and libgraph-easy-perl for the
# extension's ASCII view_circuit rendering. Consolidating all deps
# into one RUN keeps the image to a single apt layer and a single
# apt-get update.
#
# Debian bookworm ships PostgreSQL 15; for any other PG, we add the
# PGDG apt source first via postgresql-common's bundled installer so
# `postgresql-${PSQL_VERSION}` resolves for the whole supported range.
RUN apt-get update &&\
    apt-get -y install postgresql-common ca-certificates curl gnupg &&\
    /usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y &&\
    apt-get -y install \
        build-essential git mercurial unzip \
        libc6-i386 libgmp-dev libboost-dev libboost-serialization-dev \
        zlib1g-dev iproute2 \
        postgresql-${PSQL_VERSION} postgresql-server-dev-${PSQL_VERSION} \
        python3 python3-pip python3-venv \
        graphviz libgraph-easy-perl &&\
    rm -rf /var/lib/apt/lists/*


############################## GETTING ADD-ON TOOLS ##############################

RUN mkdir /tmp/tools/

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

# Copy the source AFTER apt + external-tool builds so a source-only
# change does not invalidate those (long) cache layers.
# `--chown=postgres:postgres` writes the files into the new layer
# already owned by `postgres`, avoiding a separate `chown -R` step
# (which would force overlay2 to copy-up every file just to change
# ownership : a 5x slowdown on a tree this large).
COPY --chown=postgres:postgres . /opt/provsql
WORKDIR /opt/provsql

USER postgres

# Building (parallelize across all available cores)
RUN make -j$(nproc)

# install provsql
USER root
RUN echo "shared_preload_libraries = 'provsql'" >> /etc/postgresql/${PSQL_VERSION}/main/postgresql.conf
RUN echo "local all all trust" > /etc/postgresql/${PSQL_VERSION}/main/pg_hba.conf

EXPOSE 5432

RUN make install

USER postgres
#create a db test
RUN /etc/init.d/postgresql start &&\
    pg_isready -t 30 &&\
    createuser -s test &&\
    createdb test &&\
    psql -f /opt/provsql/test/sql/setup.sql test test  &&\
    psql -f /opt/provsql/test/sql/add_provenance.sql test test &&\
    psql -f /opt/provsql/test/sql/formula.sql test test  &&\
    psql -f /opt/provsql/test/sql/sr_minmax.sql test test &&\
    psql -c "ALTER ROLE test SET search_path TO public, provsql";

############################## FINISHING THE DOCKER  ##############################

#allow access
RUN echo "listen_addresses = '*'"  >> /etc/postgresql/${PSQL_VERSION}/main/postgresql.conf
RUN echo "host all all 0.0.0.0/0 trust" >> /etc/postgresql/${PSQL_VERSION}/main/pg_hba.conf
RUN echo "host all all ::/0 trust" >> /etc/postgresql/${PSQL_VERSION}/main/pg_hba.conf

USER postgres
RUN /usr/bin/pg_ctlcluster $PSQL_VERSION main start

##############################   GETTING  STUDIO   ##############################

USER root

# Studio install path. By default, install the released `provsql-studio`
# from PyPI at `STUDIO_VERSION`. To build the image *before* the matching
# Studio has been published (e.g. for the simultaneous-release rehearsal),
# pass `--build-arg STUDIO_SOURCE=/opt/provsql/studio` to install in
# editable mode from the source already copied into the image.
ARG STUDIO_VERSION=1.1.1
ARG STUDIO_SOURCE=

# Debian bookworm's Python 3.11 enforces PEP 668, so we install Studio
# into a venv rather than the system site-packages. Prepending the venv's
# bin/ to PATH makes `provsql-studio` reachable everywhere.
RUN python3 -m venv /opt/studio-venv &&\
    if [ -n "$STUDIO_SOURCE" ]; then \
        /opt/studio-venv/bin/pip install -e "$STUDIO_SOURCE"; \
    else \
        /opt/studio-venv/bin/pip install "provsql-studio==$STUDIO_VERSION"; \
    fi
ENV PATH="/opt/studio-venv/bin:$PATH"

CMD ["/bin/bash", "/opt/provsql/docker/demo.sh"]
EXPOSE 8000
