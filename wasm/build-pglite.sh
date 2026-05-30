#!/bin/bash
# Relink PGlite's main module so it exports the PG internals ProvSQL needs,
# producing a matched pglite.wasm that can load provsql.so.
#
# Runs INSIDE the electricsql/pglite-builder container, from the
# postgres-pglite tree root, AFTER:
#   - the WASM Postgres core has been built+installed into dist/ (the
#     configure+make+install steps of the upstream build-pglite.sh), and
#   - wasm/build-extension.sh has produced provsql-wasm/provsql.so.
#
# Output: dist/bin/pglite.{wasm,data,js} exporting provsql's symbols.
set -u
NM=/emsdk/upstream/bin/llvm-nm

# (1) provsql.imports = symbols undefined across provsql's objects but not
# defined by them.  Drop C++ mangled (_Z*) names: the pure-C main module
# cannot export them, and the leftover undefined ones are provsql-internal,
# never-called template instantiations (libc++ is bundled in provsql.so).
IMPDIR=dist/include/postgresql/emscripten/extension/imports
( cd provsql-wasm
  $NM --undefined-only src/*.o | awk '{print $2}' | sed '/^$/d' | sort -u > provsql.undef.txt
  $NM --defined-only src/*.o provsql.so | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > provsql.defs.txt
  comm -23 provsql.undef.txt provsql.defs.txt | grep -v '^_Z' > "../$IMPDIR/provsql.imports" )
echo "provsql.imports: $(wc -l < $IMPDIR/provsql.imports) symbols"

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

# (2) regenerate exported_functions.txt (now includes provsql.imports).
emmake make PORTNAME=emscripten -C src/backend pglite-exported-functions 2>&1 | tail -2
echo "exported_functions.txt: $(wc -l < /install/pglite/exported_functions.txt) symbols"

# (3) relink exactly as upstream: MAIN_MODULE=2 (NOT 1 – that breaks
# initdb), the full EXPORTED_FUNCTIONS list, the same preloads.
PGROOT=/pglite; W=$(pwd)
PGPRELOAD="--preload-file $W/pglite/static/PGPASSFILE@/home/postgres/.pgpass \
--preload-file $W/pglite/static/empty@/pglite/bin/initdb \
--preload-file $W/pglite/static/empty@/pglite/bin/pg_dump \
--preload-file $W/pglite/static/empty@/pglite/bin/postgres \
--preload-file $PGROOT/share/postgresql@/pglite/share/postgresql \
--preload-file $PGROOT/lib/postgresql@/pglite/lib/postgresql \
--preload-file $W/pglite/static/password@/pglite/password \
--preload-file $W/pglite/static/empty@/pglite/pgstdin \
--preload-file $W/pglite/static/empty@/pglite/pgstdout \
--preload-file $W/pglite/static/locale-a@/pglite/locale-a \
--preload-file $W/pglite/static/minimal-icu/76.1@/pglite/icu"
RT="MEMFS,IDBFS,FS,PROXYFS,setValue,getValue,UTF8ToString,stringToNewUTF8,stringToUTF8OnStack,addFunction,removeFunction,callMain,ENV"
export POSTGRES_PGLITE_FLAGS="$PGLITE_CFLAGS -sSTACK_SIZE=8MB -sINITIAL_MEMORY=128MB -sIMPORTED_MEMORY=1 \
-sEXPORTED_RUNTIME_METHODS=$RT -sEXPORTED_FUNCTIONS=@/install/pglite/exported_functions.txt \
$PGPRELOAD -lnodefs.js -lidbfs.js"
emmake make PORTNAME=emscripten -C src/backend -j pglite || { echo "pglite link failed"; exit 1; }
emmake make PORTNAME=emscripten -C src/backend install-pglite
echo "built dist/bin/pglite.wasm: $(stat -c%s dist/bin/pglite.wasm) bytes"
