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
cd "$HERE"

# The WASM artifacts (the matched PGlite dist + provsql.tar.gz) are the only
# heavy inputs. Pass both --pglite/--provsql to (re)import them; pass neither to
# reuse the copies a previous build already placed here -- so re-assembling
# after a Studio-source change needs no WASM rebuild.
if [ -n "$PGLITE_DIST" ] || [ -n "$PROVSQL_TARGZ" ]; then
  [ -n "$PGLITE_DIST" ] && [ -n "$PROVSQL_TARGZ" ] || \
    die "pass both --pglite and --provsql, or neither (to reuse the in-place artifacts)"
  [ -f "$PGLITE_DIST/index.js" ] || die "no index.js under $PGLITE_DIST -- not a pglite dist"
  [ -f "$PROVSQL_TARGZ" ]        || die "no such file: $PROVSQL_TARGZ"
elif [ ! -f pglite/index.js ] || [ ! -f provsql.tar.gz ]; then
  die "no in-place WASM artifacts; first run with --pglite <dist> --provsql <provsql.tar.gz> (see ../../wasm/README.md)"
fi

# 1. Frontend assets, copied unmodified. -L dereferences the fonts /
#    fonts-face.css symlinks (they point into ../../branding) so the doc-root
#    is self-contained and servable by a dumb static host.
for f in app.js circuit.js app.css colors_and_type.css fonts-face.css; do
  cp -L "$STATIC/$f" "./$f"
done
rm -rf fonts img
cp -RL "$STATIC/fonts" ./fonts
cp -RL "$STATIC/img"   ./img

# 1b. Make the copied frontend path-portable. The canonical app.js uses two
#     root-absolute paths that only resolve when the app is at the server
#     root; rewrite the copies to be relative to the page so the build works
#     unchanged under a sub-path (e.g. https://host/playground/) with no
#     server rewriting. (index.html's mode anchors are handled in step 2.)
python3 - app.js <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
subs = [("s.src = '/static/circuit.js'", "s.src = 'static/circuit.js'"),
        ("window.location.href = '/circuit'", "window.location.href = '?mode=circuit'")]
for old, new in subs:
    if old not in s:
        sys.exit("build.sh: app.js no longer contains %r (path-portability rewrite)" % old)
    s = s.replace(old, new)
open(p, "w", encoding="utf-8").write(s)
PY

# 2. Boot shell (app.html): swap the canonical direct <script src="app.js">
#    (which would run before the in-page backend exists) for the boot-status
#    bar plus the studio-boot module that brings PGlite + Pyodide up and then
#    injects app.js. Default the body to circuit mode (studio-boot overrides
#    it from ?mode= anyway, but this avoids a flash of the wrong mode). The
#    landing page (index.html, below) is what bare visits hit; it gates on JSPI
#    and links here.
python3 - "$STATIC/index.html" app.html <<'PY'
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
# Path-portability: the mode-switch anchors are root-absolute (/circuit,
# /where); make them relative ?mode= queries so they resolve under any base
# path and need no server redirect.
html = html.replace('href="/circuit"', 'href="?mode=circuit"')
html = html.replace('href="/where"', 'href="?mode=where"')
# The in-browser build is branded "ProvSQL Playground" (vs the installable
# "ProvSQL Studio"); rebrand the title and the nav wordmark.
html = html.replace("<title>ProvSQL Studio</title>", "<title>ProvSQL Playground</title>")
html = html.replace('<span class="wp-nav__title"><em>ProvSQL</em> Studio</span>',
                    '<span class="wp-nav__title"><em>ProvSQL</em> Playground</span>')
# Self-host Font Awesome instead of the CDN <link> (vendor.sh fetches it).
import re
html = re.sub(r'https://use\.fontawesome\.com/releases/v[0-9.]+/css/all\.css',
              'fontawesome/css/all.min.css', html)
open(dst, "w", encoding="utf-8").write(html)
PY

# 2b. The landing page (index.html, what bare visits hit): explains the JSPI
#     requirement + browser support, and forwards shared deep links to app.html
#     when JSPI is present. Tracked source, copied verbatim.
cp landing.html index.html

# 3. The two absolute /static/ paths the unmodified frontend hard-codes
#    (app.js -> /static/circuit.js, circuit.js -> /static/app.css).
mkdir -p static
cp ./circuit.js static/circuit.js
cp ./app.css    static/app.css

# 4. The unmodified Studio backend, mounted into Pyodide by studio-boot.js.
rm -rf pkg
mkdir pkg
cp "$PKGSRC"/*.py pkg/

# 5. The WASM artifacts from ../../wasm/ (only when (re)imported; otherwise the
#    in-place pglite/ + provsql.tar.gz from a previous build are kept).
if [ -n "$PGLITE_DIST" ]; then
  rm -rf pglite
  cp -RL "$PGLITE_DIST" pglite
  cp "$PROVSQL_TARGZ" provsql.tar.gz
fi

# 6. The tutorial / case-study databases (converted from doc/*/setup.sql to
#    statement lists studio-boot.js loads into one PGlite database each).
python3 build-casestudies.py

# 7. Vendor every runtime dependency (Pyodide, wheels, Graphviz, Font Awesome,
#    license texts) so nothing loads from a CDN at run time.
./vendor.sh

# 8. Third-party notices. We now redistribute these, so their licenses must
#    travel with the build. Copy the project-specific ones (PGlite's NOTICE is
#    not a generic SPDX text) next to the SPDX texts vendor.sh fetched, and
#    list every component (shipped + the data sources) in THIRD-PARTY.md.
for f in LICENSE LICENSE.md NOTICE NOTICE.md; do
  for d in "$PGLITE_DIST" "$PGLITE_DIST/.." "$PGLITE_DIST/../.." "$PGLITE_DIST/../../.."; do
    [ -f "$d/$f" ] && cp "$d/$f" "licenses/PGlite-$f" && break
  done
done
python3 - <<'PY'
import json, os
wheels = json.load(open("wheels/manifest.json"))
def ver(prefix):
    for w in wheels:
        if w.lower().startswith(prefix + "-"):
            return w.split("-")[1]
    return "?"
# (component, [SPDX ids], role, source HTML with real links). The license ids
# link to the bundled licenses/<id>.txt; the source column carries real links.
REPO = "https://github.com/PierreSenellart/provsql"
PALLETS = '<a href="https://palletsprojects.com/">Pallets</a>'
rows = [
 ("ProvSQL + ProvSQL Studio", ["MIT"], "shipped",
  f'Pierre Senellart – <a href="{REPO}">repository</a>'),
 ("PostgreSQL (compiled to WASM)", ["PostgreSQL"], "shipped",
  '<a href="https://www.postgresql.org/">PostgreSQL Global Development Group</a>'),
 ("@electric-sql/pglite", ["Apache-2.0"], "shipped",
  '<a href="https://github.com/electric-sql/pglite">ElectricSQL</a> – <a href="licenses/PGlite-LICENSE">PGlite-LICENSE</a>'),
 ("Boost (linked into provsql.so)", ["BSL-1.0"], "shipped",
  '<a href="https://www.boost.org/">Boost authors</a>'),
 ("Pyodide", ["MPL-2.0"], "shipped (vendored)",
  '<a href="https://pyodide.org/">Pyodide contributors</a>'),
 ("CPython (in Pyodide)", ["PSF-2.0"], "shipped (vendored)",
  '<a href="https://www.python.org/">Python Software Foundation</a>'),
 ("Graphviz (via @hpcc-js/wasm-graphviz)", ["EPL-1.0"], "shipped (vendored)",
  'Graphviz authors – source: <a href="https://gitlab.com/graphviz/graphviz">gitlab.com/graphviz/graphviz</a>'),
 ("@hpcc-js/wasm-graphviz wrapper", ["Apache-2.0"], "shipped (vendored)",
  '<a href="https://github.com/hpcc-systems/hpcc-js-wasm">HPCC Systems</a>'),
 ("Flask " + ver("flask"), ["BSD-3-Clause"], "shipped (wheel)", PALLETS),
 ("Werkzeug " + ver("werkzeug"), ["BSD-3-Clause"], "shipped (wheel)", PALLETS),
 ("Jinja2 " + ver("jinja2"), ["BSD-3-Clause"], "shipped (wheel)", PALLETS),
 ("MarkupSafe " + ver("markupsafe"), ["BSD-3-Clause"], "shipped (wheel)", PALLETS),
 ("click " + ver("click"), ["BSD-3-Clause"], "shipped (wheel)", PALLETS),
 ("itsdangerous " + ver("itsdangerous"), ["BSD-3-Clause"], "shipped (wheel)", PALLETS),
 ("blinker " + ver("blinker"), ["MIT"], "shipped (wheel)",
  '<a href="https://github.com/pallets-eco/blinker">Jason Kirtland and contributors</a>'),
 ("sqlparse " + ver("sqlparse"), ["BSD-3-Clause"], "shipped (wheel)",
  '<a href="https://github.com/andialbrecht/sqlparse">Andi Albrecht</a>'),
 ("Font Awesome 5 Free", ["MIT", "CC-BY-4.0", "OFL-1.1"], "shipped (vendored)",
  '<a href="https://fontawesome.com/">Fonticons, Inc.</a> – <a href="fontawesome/LICENSE.txt">fontawesome/LICENSE.txt</a>'),
 ("EB Garamond, Jost, Fira Code (brand fonts)", ["OFL-1.1"], "shipped",
  '<a href="fonts/OFL-EBGaramond.txt">EBGaramond</a>, '
  '<a href="fonts/OFL-Jost.txt">Jost</a>, '
  '<a href="fonts/OFL-FiraCode.txt">FiraCode</a>'),
 ("Case-study data: cs4 (ministers)", ["CC0-1.0"], "shipped",
  '<a href="https://www.wikidata.org/">Wikidata</a>'),
 ("Case-study data: tutorial, cs1, cs2, cs5, cs6, cs7", ["MIT"], "shipped",
  f'<a href="{REPO}">this repository</a>'),
]
import html as _h
licenses = sorted(f for f in os.listdir("licenses"))
with open("THIRD-PARTY.html", "w", encoding="utf-8") as f:
    f.write("""<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ProvSQL Playground: third-party components</title>
<link rel="icon" href="img/favicon.ico">
<link rel="stylesheet" href="/assets/css/main.css">
</head><body class="layout--single">
<div class="initial-content"><div id="main" role="main"><article class="page"><div class="page__inner-wrap">
<header><h1 class="page__title">Third-party components</h1></header>
<section class="page__content">
<p><a class="btn btn--small" href="./" onclick="if(history.length>1){history.back();return false}">&larr; Back to ProvSQL Playground</a></p>
<p>ProvSQL Playground is fully self-hosted: it loads nothing from a CDN at run
time. The components below are redistributed with it; full license texts are in
<code>licenses/</code> (and <code>fontawesome/</code>, <code>fonts/</code>).</p>
<table><thead><tr><th>Component</th><th>License</th><th>Role</th><th>Copyright / source</th></tr></thead><tbody>
""")
    for name, lic_ids, role, who_html in rows:
        lic = " / ".join('<a href="licenses/%s.txt">%s</a>' % (i, _h.escape(i))
                         for i in lic_ids)
        f.write("<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>\n"
                % (_h.escape(name), lic, _h.escape(role), who_html))
    f.write("</tbody></table>\n<p><small>License texts: "
            + ", ".join('<a href="licenses/%s">%s</a>' % (_h.escape(x), _h.escape(x)) for x in licenses)
            + ".</small></p>\n")
    f.write("<p><small>The optional external knowledge-compiler tools "
            "(d4, c2d, miniC2D, dsharp, weightmc…) are <strong>not</strong> "
            "bundled: the browser cannot spawn subprocesses, so the tool "
            "registry is disabled and none of their (often research-only) "
            "licenses apply.</small></p>\n"
            "</section></div></article></div></div>\n</body></html>\n")
print("  THIRD-PARTY.html +", len(licenses), "license texts")
PY

# 9. .htaccess for Apache static hosting (e.g. provsql.org/playground/). The
#    build is path-portable and uses only relative URLs, so no rewriting is
#    needed; this only supplies the WASM MIME type Apache does not serve by
#    default (a wrong type breaks WebAssembly instantiation) and a couple of
#    related types. Needs AllowOverride FileInfo to take effect.
cat > .htaccess <<'HT'
AddType application/wasm .wasm
AddType text/javascript .mjs
AddType application/octet-stream .data
AddType application/gzip .tar.gz
HT

echo "build.sh: assembled doc-root in $HERE"
echo "  serve with:  python3 serve.py    (then open http://localhost:8089/)"
