#!/usr/bin/env bash
#
# Reproduce the WASM build of the GitHub `wasm` workflow locally (driven by
# `make wasm`): build the PGlite WASM core and the ProvSQL extension against the
# Emscripten builder image, producing the matched pglite dist + provsql.tar.gz,
# then assemble the ProvSQL Playground doc-root from them.  The browser e2e is
# left to run separately (`cd studio && python3 -m pytest tests/web`).
#
# Requirements:
#   * a container runtime: podman (rootless, no daemon) or docker.  Set
#     CONTAINER=podman / =docker to force one; auto-detected otherwise.
#   * Node + corepack (for the PGlite TS package build).
#   * Boost headers (BOOSTINC, default /usr/include/boost), mounted into the
#     extension compile.
#
# Env overrides: CONTAINER, PGLITE_BUILDER_IMG, WASM_BUILD_DIR, BOOSTINC.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${PGLITE_BUILDER_IMG:-docker.io/electricsql/pglite-builder:3.1.74-5-postgis-libicu-min}"
BUILD="${WASM_BUILD_DIR:-$REPO/wasm/.build}"
BOOSTINC="${BOOSTINC:-/usr/include/boost}"
if [ -z "${CONTAINER:-}" ]; then
  CONTAINER="$(command -v podman >/dev/null 2>&1 && echo podman || echo docker)"
fi

echo "==> container=$CONTAINER  image=$IMG"
echo "==> build dir=$BUILD"

# 1. Generate the extension SQL the WASM build packages (native make).
make -C "$REPO" sql/provsql.sql

# 2. Pull the pinned Emscripten builder image and the matched PGlite tree.
"$CONTAINER" pull "$IMG"
mkdir -p "$BUILD"
if [ ! -d "$BUILD/pglite/.git" ]; then
  git clone --recurse-submodules https://github.com/electric-sql/pglite "$BUILD/pglite"
fi
cd "$BUILD/pglite"
git submodule update --init --depth 1 postgres-pglite
PG="$PWD/postgres-pglite"

# The `dist/` install tree is the bind-mount target shared into every
# container run (it is what the extension links against).  Pre-create it on
# the host: rootless podman `statfs`-checks a bind source and aborts if it is
# missing, whereas the directory is only created inside the container.
mkdir -p "$PG/dist"

# 3. Build the PGlite Postgres tree, compile the extension against it, then
#    relink pglite.wasm so it exports ProvSQL's symbols (MAIN_MODULE=2).
"$CONTAINER" run --rm -w "$PG" -v "$PG:$PG" -v "$PG/dist:/pglite" "$IMG" ./build-pglite.sh
mkdir -p "$PG/provsql-wasm"
cp -r "$REPO/src" "$PG/provsql-wasm/src"
cp "$REPO"/sql/provsql--*.sql "$PG/provsql-wasm/"
cp "$REPO/provsql.control" "$PG/provsql-wasm/"
cp "$REPO/wasm/build-extension.sh" "$REPO/wasm/build-pglite.sh" "$PG/"
"$CONTAINER" run --rm -w "$PG" -v "$PG:$PG" -v "$PG/dist:/pglite" \
  -v "$BOOSTINC:/boostinc/boost:ro" "$IMG" ./build-extension.sh
"$CONTAINER" run --rm -w "$PG" -v "$PG:$PG" -v "$PG/dist:/pglite" "$IMG" ./build-pglite.sh

# 4. Build the matched PGlite TS package and run the headless Node smoke test.
cp "$PG"/dist/bin/pglite.wasm "$PG"/dist/bin/pglite.data "$PG"/dist/bin/pglite.js \
   packages/pglite/release/
cp "$PG"/dist/extensions/*.tar.gz packages/pglite/release/ 2>/dev/null || true
corepack pnpm install --filter "@electric-sql/pglite..."
corepack pnpm --filter @electric-sql/pglite-utils run build
corepack pnpm --filter @electric-sql/pglite      run build:js
PGLITE_DIST="$PWD/packages/pglite/dist" \
PROVSQL_TARBALL="$PG/provsql-wasm/provsql.tar.gz" \
  node "$REPO/wasm/test-node.mjs"

# 5. Assemble the Playground doc-root from the freshly built artifacts.
"$REPO/studio/web/build.sh" \
  --pglite  "$PWD/packages/pglite/dist" \
  --provsql "$PG/provsql-wasm/provsql.tar.gz"

echo "==> WASM build done; Playground assembled in studio/web/"
echo "    browser e2e:  cd studio && python3 -m pytest tests/web"
