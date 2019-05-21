EXTENSION = provsql
EXTVERSION = $(shell grep default_version $(EXTENSION).control | \
           sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

MODULE_big = provsql
OBJS = $(patsubst %.c,%.o,$(wildcard src/*.c)) $(patsubst %.cpp,%.o,$(wildcard src/*.cpp))

DOCS = $(wildcard doc/*.md)
DATA = sql/$(EXTENSION)--$(EXTVERSION).sql
EXTRA_CLEAN = sql/$(EXTENSION)--$(EXTVERSION).sql

# We want REGRESS to be empty, since we are going to provide a schedule
# of tests. But we want REGRESS to be defined, otherwise installcheck
# does nothing. So we use the following Makefile trick to have a defined
# variable with empty value
EMPTY = 
REGRESS = $(EMPTY) 
REGRESS_OPTS = --inputdir=test --load-language=plpgsql --outputdir=$(shell mktemp -d /tmp/tmp.provsqlXXXX) --schedule test/schedule

all: $(DATA) $(MODULE_big).so test/schedule

$(OBJS): $(wildcard src/*.h)

sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
	cp $< $@

LDFLAGS_SL = -lstdc++

ifdef DEBUG
PG_CPPFLAGS += -O0 -g
endif

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

%.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

%.o : %.cpp
	$(CXX) -std=c++14 -fPIC $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

VERSION     = $(shell $(PG_CONFIG) --version | awk '{print $$2}')
PGVER_MAJOR = $(shell echo $(VERSION) | awk -F. '{ print ($$1 + 0) }')
PGVER_MINOR = $(shell echo $(VERSION) | awk -F. '{ print ($$2 + 0) }')

# Temporary fix for PostgreSQL compilation chain / llvm bug, see 
# https://github.com/rdkit/rdkit/issues/2192
COMPILE.cxx.bc = $(CLANG) -xc++ -Wno-ignored-attributes $(BITCODE_CPPFLAGS) $(CPPFLAGS) -emit-llvm -c
%.bc : %.cpp
	$(COMPILE.cxx.bc) -o $@ $<
	$(LLVM_BINPATH)/opt -module-summary -f $@ -o $@
