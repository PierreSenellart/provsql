EXTENSION = provsql
EXTVERSION = $(shell grep default_version $(EXTENSION).control | \
           sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

MODULE_big = provsql
OBJS = $(patsubst %.c,%.o,$(wildcard src/*.c))

DOCS = $(wildcard doc/*.md)
DATA = sql/$(EXTENSION)--$(EXTVERSION).sql
EXTRA_CLEAN = sql/$(EXTENSION)--$(EXTVERSION).sql

# We want REGRESS to be empty, since we are going to provide a schedule
# of tests. But we want REGRESS to be defined, otherwise installcheck
# does nothing. So we use the following Makefile trick to have a defined
# variable with empty value
EMPTY = 
REGRESS = $(EMPTY) 
REGRESS_OPTS = --inputdir=test --load-language=plpgsql --outputdir=$(shell mktemp -d --tmpdir tmp.provsqlXXXX) --schedule test/schedule

all: $(DATA) $(MODULE_big).so

$(OBJS): $(wildcard src/*.h)

sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
	cp $< $@

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
