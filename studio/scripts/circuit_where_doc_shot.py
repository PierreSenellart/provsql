"""Regenerate the Circuit-mode and Where-mode documentation screenshots.

Boots Studio on a database named `demo` (which must exist with the
provsql extension and the upstream `personnel` fixture loaded -- i.e.
test/sql/setup.sql + test/sql/add_provenance.sql), runs the Paris
personnel query in each mode and captures into
doc/source/_static/studio/:

  circuit-mode.png   the provenance circuit of a pinned result row with
                     the input-gate inspector open
  where-mode.png     the where-provenance highlight of a pinned result
                     cell against the source table

Run from studio/ with the dev venv (playwright + chromium installed):

    .venv/bin/python scripts/circuit_where_doc_shot.py

The viewport (1920x976, scale 1) matches the other full-page Studio
shots (contributions-mode.png, notebook-mode.png).
"""
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

from playwright.sync_api import expect, sync_playwright

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "doc" / "source" / "_static" / "studio"
PORT = 8076
URL = f"http://127.0.0.1:{PORT}"

# A DISTINCT query collapses the three Paris rows into one answer whose
# provenance is a plus (the caption's "DISTINCT-circuit / plus-rooted DAG").
CIRCUIT_Q = "SELECT DISTINCT city FROM personnel WHERE city = 'Paris';"
WHERE_Q = ("SELECT name, position, city, classification\n"
           "FROM personnel WHERE city = 'Paris';")

server = subprocess.Popen(
    [sys.executable, "-m", "provsql_studio",
     "--host", "127.0.0.1", "--port", str(PORT),
     "--dsn", "dbname=demo", "--search-path", "public",
     "--ignore-version"],
    stdout=open("/tmp/circuit_where_doc_shot_server.log", "wb"),
    stderr=subprocess.STDOUT,
)
try:
    for _ in range(100):
        try:
            urllib.request.urlopen(URL, timeout=1)
            break
        except Exception:
            time.sleep(0.2)
    else:
        sys.exit("server did not come up; "
                 "see /tmp/circuit_where_doc_shot_server.log")

    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": 1920, "height": 976})

        # --- Circuit mode ---
        page.goto(URL + "/circuit")
        expect(page.locator("body")).to_have_class(
            "mode-circuit", timeout=15000)
        page.locator("#request").fill(CIRCUIT_Q)
        page.locator("#run-btn").click()
        expect(page.locator("#result-count")).to_have_text("1", timeout=15000)
        # Pin the result row's provsql cell to render its plus-rooted DAG.
        page.locator("#result-body tr").first.locator("td").last.click()
        svg = page.locator("#sidebar-body svg").first
        expect(svg).to_be_visible(timeout=15000)
        # Pin an input gate (leaf) to open the inspector on a base tuple.
        page.locator("#sidebar-body svg .node-group.node--input").first.click()
        expect(page.locator("#inspector")).to_be_visible(timeout=8000)
        page.wait_for_timeout(400)
        page.screenshot(path=str(OUT / "circuit-mode.png"))
        print("circuit-mode.png")

        # --- Where mode ---
        page.goto(URL + "/where")
        expect(page.locator("body")).to_have_class("mode-where", timeout=15000)
        page.locator("#request").fill(WHERE_Q)
        page.locator("#run-btn").click()
        expect(page.locator("#result-count")).to_have_text("3", timeout=15000)
        # Pin the first result cell to highlight its where-provenance.
        first_cell = page.locator("#result-body tr").first.locator("td").first
        first_cell.click()
        page.wait_for_timeout(400)
        page.screenshot(path=str(OUT / "where-mode.png"))
        print("where-mode.png")

        browser.close()
finally:
    server.terminate()
    try:
        server.wait(timeout=5)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()
