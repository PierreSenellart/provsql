#!/bin/bash
# Build the ProvSQL extension to a PGlite-loadable WASM side module.
#
# Runs INSIDE the electricsql/pglite-builder container (which provides
# emscripten + the WASM-built dependency libs).  Expects, relative to the
# postgres-pglite tree root used as the working directory:
#   - dist/                          the WASM Postgres install (configure+make+install)
#   - provsql-wasm/src/              a copy of ProvSQL's src/ (the extension sources)
#   - provsql-wasm/provsql.control, provsql-wasm/provsql--<ver>.sql
# and Boost headers mounted at /boostinc/boost.
#
# Output: provsql-wasm/provsql.so (WASM side module) and provsql.tar.gz
# (the PGlite extension bundle).  No libboost_serialization is needed: the
# Boost-serialized circuit round-trip is compiled out under
# PROVSQL_INPROCESS_STORE (see doc/TODO/wasm-browser-deployment.md §6).
set -u
emcc --clear-cache >/dev/null 2>&1 || true

# libc++ fix (see patches/0001-cxx-inline-libcxx.patch): scope PGlite's
# PG_FORCE_DISABLE_INLINE to C, else libc++'s inline namespace breaks and
# every C++ TU fails.  Idempotent.
sed -i 's/^#ifdef PG_FORCE_DISABLE_INLINE$/#if defined(PG_FORCE_DISABLE_INLINE) \&\& !defined(__cplusplus)/' \
  dist/include/postgresql/server/c.h

# ABI flags PGlite's Postgres core and PostGIS were built with.
PGLITE_CFLAGS="-m32 -sWASM_BIGINT -fpic -sENVIRONMENT=node,web,worker -sSUPPORT_LONGJMP=emscripten \
-Wno-declaration-after-statement -Wno-macro-redefined -Wno-unused-function -Wno-missing-prototypes \
-Wno-incompatible-pointer-types -O2 \
-D__PGLITE__ -Dsystem=pgl_system -Dpopen=pgl_popen -Dpclose=pgl_pclose \
-Dgeteuid=pgl_geteuid -Dgetuid=pgl_getuid -Dgetpwuid=pgl_getpwuid -Dexit=pgl_exit \
-Dmunmap=pgl_munmap -Dfcntl=pgl_fcntl -Datexit=pgl_atexit \
-Dsetsockopt=pgl_setsockopt -Dgetsockopt=pgl_getsockopt -Dgetsockname=pgl_getsockname \
-Drecv=pgl_recv -Dsend=pgl_send -Dconnect=pgl_connect -Dpoll=pgl_poll \
-Dshmget=pgl_shmget -Dshmat=pgl_shmat -Dshmdt=pgl_shmdt -Dshmctl=pgl_shmctl \
-Dlongjmp=pgl_longjmp -Dsiglongjmp=pgl_siglongjmp"

SRV="$PWD/dist/include/postgresql/server"
INT="$PWD/dist/include/postgresql/internal"
cd provsql-wasm || exit 99
INC="-I$SRV -I$INT -I/boostinc -I. -Isrc"  # -I. for the few <src/X.h> angle includes

# Everything in src/, minus the tdkc-only and migration sources (see
# Makefile.internal's OBJS filter).
CFILES=$(ls src/*.c)
CPPFILES=$(ls src/*.cpp | grep -vE 'provsql_migrate_mmap|TreeDecompositionKnowledgeCompiler|kcmcp_server|dimacs_cnf')

# Boost <= ~1.78 derives boost::hash_detail::hash_base from std::unary_function
# unless BOOST_NO_CXX98_FUNCTION_BASE is set, but only auto-defines that macro
# for the MSVC stdlib -- never for libc++.  emscripten's libc++ removed
# std::unary_function in C++17, so CircuitCache.h (boost/multi_index hashed
# index -> boost/functional/hash) fails to compile against an old host Boost.
# Define it ourselves to select Boost's C++17-safe hash_base; harmless on
# newer Boost (already set) and on the C files (they pull in no Boost).
BOOST_CXX17_COMPAT="-DBOOST_NO_CXX98_FUNCTION_BASE"

OBJS=""; FAIL=""
for f in $CFILES;   do o="${f%.c}.o";   emcc            $PGLITE_CFLAGS $INC -c "$f" -o "$o" 2>"$o.err" && OBJS="$OBJS $o" || { FAIL="$FAIL $f"; echo "CC FAIL $f"; head -5 "$o.err"; }; done
for f in $CPPFILES; do o="${f%.cpp}.o"; em++ -std=c++17 $BOOST_CXX17_COMPAT $PGLITE_CFLAGS $INC -c "$f" -o "$o" 2>"$o.err" && OBJS="$OBJS $o" || { FAIL="$FAIL $f"; echo "CXX FAIL $f"; head -5 "$o.err"; }; done
[ -n "$FAIL" ] && { echo "COMPILE FAILURES:$FAIL"; exit 1; }

# Whole-archive libc++/libc++abi: the PGlite main module is pure C and
# cannot supply the C++ runtime to the side module.
em++ $PGLITE_CFLAGS -sSIDE_MODULE=1 -sSUPPORT_LONGJMP=emscripten \
  -Wl,--whole-archive -lc++ -lc++abi -Wl,--no-whole-archive \
  $OBJS -o provsql.so 2>link.err || { echo "LINK FAILED"; tail -15 link.err; exit 1; }
echo "built provsql.so: $(stat -c%s provsql.so) bytes"

# Package the PGlite bundle (layout relative to WASM_PREFIX=/pglite).  Ship ALL
# provsql--*.sql: the base install script for the control's default_version plus
# the cross-version upgrade paths.  (Packaging only the first alphabetically --
# provsql--1.0.0.sql -- left CREATE EXTENSION with no script for the default
# version, e.g. 1.9.0-dev.)
rm -rf stage && mkdir -p stage/lib/postgresql stage/share/postgresql/extension
cp provsql.so stage/lib/postgresql/provsql.so
cp provsql.control provsql--*.sql stage/share/postgresql/extension/
( cd stage && tar -czf ../provsql.tar.gz $(find . -type f | sed 's|^\./||') )
echo "packaged provsql.tar.gz:"; tar tzf provsql.tar.gz
