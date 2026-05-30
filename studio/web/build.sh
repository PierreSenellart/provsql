#!/usr/bin/env bash
#
# Assemble the static doc-root for the browser (PGlite) build of ProvSQL
# Studio. Reproducible from two sources:
#
#   1. the in-repo Studio frontend + backend (studio/provsql_studio/), copied
#      unmodified -- the browser build shares the canonical assets; only
#      index.html is transformed into the boot shell, and
#   2. the two WebAssembly artifacts produced by ../../wasm/: the matched
#      @electric-sql/pglite dist (carrying the provsql-aware pglite.wasm) and
#      provsql.tar.gz (the extension bundle).
#
# Everything this writes is git-ignored (see .gitignore); the tracked inputs
# are studio-boot.js, psycopg_pglite.py, serve.py and this script.
#
# Usage:
#   ./build.sh --pglite <pglite-dist-dir> --provsql <provsql.tar.gz>
# or via the environment:
#   PGLITE_DIST=<dir> PROVSQL_TARGZ=<file> ./build.sh
#
# See ../../wasm/README.md for how to build the two WASM artifacts, and
# ./README.md for the resulting doc-root layout and how to serve it.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"   # studio/web
STATIC="$HERE/../provsql_studio/static"
PKGSRC="$HERE/../provsql_studio"

PGLITE_DIST="${PGLITE_DIST:-}"
PROVSQL_TARGZ="${PROVSQL_TARGZ:-}"
while [ $# -gt 0 ]; do
  case "$1" in
    --pglite)  PGLITE_DIST="$2";  shift 2 ;;
    --provsql) PROVSQL_TARGZ="$2"; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

die() { echo "build.sh: $*" >&2; exit 2; }
[ -n "$PGLITE_DIST" ]   || die "set --pglite <dir> (the built @electric-sql/pglite dist with index.js)"
[ -n "$PROVSQL_TARGZ" ] || die "set --provsql <file> (provsql.tar.gz from ../../wasm/)"
[ -f "$PGLITE_DIST/index.js" ] || die "no index.js under $PGLITE_DIST -- not a pglite dist"
[ -f "$PROVSQL_TARGZ" ]        || die "no such file: $PROVSQL_TARGZ"

cd "$HERE"

# 1. Frontend assets, copied unmodified. -L dereferences the fonts /
#    fonts-face.css symlinks (they point into ../../branding) so the doc-root
#    is self-contained and servable by a dumb static host.
for f in app.js circuit.js app.css colors_and_type.css fonts-face.css; do
  cp -L "$STATIC/$f" "./$f"
done
rm -rf fonts img
cp -RL "$STATIC/fonts" ./fonts
cp -RL "$STATIC/img"   ./img

# 2. Boot-shell index.html: swap the canonical direct <script src="app.js">
#    (which would run before the in-page backend exists) for the boot-status
#    bar plus the studio-boot module that brings PGlite + Pyodide up and then
#    injects app.js. Default the body to circuit mode (studio-boot overrides
#    it from ?mode= anyway, but this avoids a flash of the wrong mode).
python3 - "$STATIC/index.html" index.html <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
html = open(src, encoding="utf-8").read()
needle = '<script src="app.js"></script>'
if needle not in html:
    sys.exit("build.sh: canonical index.html no longer contains %r" % needle)
boot = (
    '<div id="studio-boot-status" style="position:fixed;top:0;left:0;right:0;'
    'background:#0f1115;color:#d7dae0;font:13px ui-monospace,monospace;'
    'padding:6px 12px;z-index:99999">loading ProvSQL Studio (WASM)…</div>\n'
    '<script type="module" src="studio-boot.js"></script>'
)
html = html.replace(needle, boot)
html = html.replace('<body class="mode-where">', '<body class="mode-circuit">')
open(dst, "w", encoding="utf-8").write(html)
PY

# 3. The two absolute /static/ paths the unmodified frontend hard-codes
#    (app.js -> /static/circuit.js, circuit.js -> /static/app.css).
mkdir -p static
cp ./circuit.js static/circuit.js
cp ./app.css    static/app.css

# 4. The unmodified Studio backend, mounted into Pyodide by studio-boot.js.
rm -rf pkg
mkdir pkg
cp "$PKGSRC"/*.py pkg/

# 5. The WASM artifacts from ../../wasm/.
rm -rf pglite
cp -RL "$PGLITE_DIST" pglite
cp "$PROVSQL_TARGZ" provsql.tar.gz

# 6. The tutorial / case-study databases (converted from doc/*/setup.sql to
#    statement lists studio-boot.js loads into one PGlite database each).
python3 build-casestudies.py

echo "build.sh: assembled doc-root in $HERE"
echo "  serve with:  python3 serve.py    (then open http://localhost:8089/)"
