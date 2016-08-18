EXTENSION = provsql
EXTVERSION = $(shell grep default_version $(EXTENSION).control | \
           sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

MODULE_big = provsql
OBJS = $(patsubst %.c,%.o,$(wildcard src/*.c))

DOCS = $(wildcard doc/*.md)
DATA = sql/$(EXTENSION)--$(EXTVERSION).sql
EXTRA_CLEAN = sql/$(EXTENSION)--$(EXTVERSION).sql test/schedule

# We want REGRESS to be empty, since we are going to provide a schedule
# of tests. But we want REGRESS to be defined, otherwise installcheck
# does nothing. So we use the following Makefile trick to have a defined
# variable with empty value
EMPTY = 
REGRESS = $(EMPTY) 
REGRESS_OPTS = --inputdir=test --load-language=plpgsql --outputdir=$(shell mktemp -d --tmpdir tmp.provsqlXXXX) --schedule test/schedule

all: $(DATA) $(MODULE_big).so test/schedule

$(OBJS): $(wildcard src/*.h)

sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
	cp $< $@

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

VERSION     = $(shell $(PG_CONFIG) --version | awk '{print $$2}')
PGVER_MAJOR = $(shell echo $(VERSION) | awk -F. '{ print ($$1 + 0) }')
PGVER_MINOR = $(shell echo $(VERSION) | awk -F. '{ print ($$2 + 0) }')

test/schedule:
	cat test/schedule.common > test/schedule
	if [ $(PGVER_MAJOR) -eq 9 -a $(PGVER_MINOR) -ge 5 ] ; then \
	  cat test/schedule.9.5 >> test/schedule; \
	fi
