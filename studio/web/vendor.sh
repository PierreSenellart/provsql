#!/usr/bin/env bash
#
# Vendor every runtime dependency into the doc-root so the browser build loads
# nothing from a third-party CDN at run time. Fetches (build-time, needs net):
#
#   pyodide/      Pyodide core + micropip + packaging  (loadPyodide indexURL)
#   wheels/       the Flask + sqlparse wheel closure    (micropip installs these)
#   vendor/graphviz/index.js   @hpcc-js/wasm-graphviz   (the `dot` replacement)
#   fontawesome/  Font Awesome 5 Free CSS + webfonts
#   licenses/     license texts for the redistributed third-party components
#
# Idempotent: skips a download whose target already exists. Pinned versions
# keep the build reproducible. Run via build.sh, or directly to refresh.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

PYODIDE_VER=v0.29.4
PYODIDE_CDN="https://cdn.jsdelivr.net/pyodide/${PYODIDE_VER}/full"
GRAPHVIZ_VER=1.22.0
FA_VER=5.15.4

# Wheels Pyodide ships (matched to its CPython/emscripten ABI). markupsafe is a
# C-extension built for emscripten, so it MUST come from Pyodide, not PyPI.
PYODIDE_WHEELS=(
  micropip-0.11.1-py3-none-any.whl
  packaging-26.2-py3-none-any.whl
  click-8.3.1-py3-none-any.whl
  jinja2-3.1.6-py3-none-any.whl
  markupsafe-3.0.2-cp313-cp313-pyemscripten_2025_0_wasm32.whl
)
# Pure-python (py3-none-any) wheels from PyPI completing the Flask closure.
PYPI_PKGS=(flask werkzeug itsdangerous blinker sqlparse)

fetch() { # url dest
  if [ -s "$2" ]; then echo "  have $2"; return; fi
  mkdir -p "$(dirname "$2")"
  echo "  get  $2"; curl -fsSL -m 120 -o "$2" "$1"
}

echo "vendor.sh: Pyodide core ($PYODIDE_VER)"
for f in pyodide.mjs pyodide.asm.js pyodide.asm.wasm python_stdlib.zip pyodide-lock.json; do
  fetch "$PYODIDE_CDN/$f" "pyodide/$f"
done
# micropip + packaging live in the indexURL dir (loadPackage reads the lock).
for w in micropip-0.11.1-py3-none-any.whl packaging-26.2-py3-none-any.whl; do
  fetch "$PYODIDE_CDN/$w" "pyodide/$w"
done

echo "vendor.sh: Flask + sqlparse wheel closure"
mkdir -p wheels
# click / jinja2 / markupsafe from Pyodide (ABI-matched).
for w in click-8.3.1-py3-none-any.whl jinja2-3.1.6-py3-none-any.whl \
         markupsafe-3.0.2-cp313-cp313-pyemscripten_2025_0_wasm32.whl; do
  fetch "$PYODIDE_CDN/$w" "wheels/$w"
done
# Flask + friends + sqlparse from PyPI (all pure-python py3-none-any).
PIP="$(command -v pip3 || command -v pip || echo "python3 -m pip")"
$PIP download --no-deps --only-binary=:all: -d wheels "${PYPI_PKGS[@]}" >/dev/null
# Manifest: every wheel micropip must install (order-independent; the closure
# is complete so micropip never reaches PyPI).
python3 - <<'PY'
import json, os
wheels = sorted(f for f in os.listdir("wheels") if f.endswith(".whl"))
json.dump(wheels, open("wheels/manifest.json", "w"))
print("  wheels/manifest.json:", len(wheels), "wheels")
PY

echo "vendor.sh: Graphviz (@hpcc-js/wasm-graphviz $GRAPHVIZ_VER)"
fetch "https://cdn.jsdelivr.net/npm/@hpcc-js/wasm-graphviz@${GRAPHVIZ_VER}/dist/index.js" \
      "vendor/graphviz/index.js"

echo "vendor.sh: Font Awesome $FA_VER Free"
if [ ! -s fontawesome/css/all.min.css ]; then
  tmp="$(mktemp -d)"
  curl -fsSL -m 180 -o "$tmp/fa.zip" \
    "https://use.fontawesome.com/releases/v${FA_VER}/fontawesome-free-${FA_VER}-web.zip"
  ( cd "$tmp" && unzip -q fa.zip )
  d="$tmp/fontawesome-free-${FA_VER}-web"
  mkdir -p fontawesome/css fontawesome/webfonts
  cp "$d/css/all.min.css" fontawesome/css/
  cp "$d"/webfonts/* fontawesome/webfonts/
  cp "$d/LICENSE.txt" fontawesome/LICENSE.txt
  rm -rf "$tmp"
  echo "  fontawesome/ populated"
else
  echo "  have fontawesome/"
fi

echo "vendor.sh: license texts (SPDX) for the redistributed components"
SPDX="https://raw.githubusercontent.com/spdx/license-list-data/main/text"
for id in Apache-2.0 MPL-2.0 EPL-1.0 PostgreSQL BSD-3-Clause MIT \
          PSF-2.0 BSL-1.0 OFL-1.1 CC-BY-4.0 CC0-1.0; do
  fetch "$SPDX/$id.txt" "licenses/$id.txt"
done

echo "vendor.sh: done"
