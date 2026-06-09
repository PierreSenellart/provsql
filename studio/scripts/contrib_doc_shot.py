"""Regenerate the Contributions-mode documentation screenshot.

Boots Studio on the `cs2` database (the Open Science case study, which
must exist with the provsql extension and the casestudy2 fixture loaded),
opens Contributions mode, runs the replication query for the
Exercise -> Cardiovascular Disease -> beneficial finding, labels inputs
with `study_mapping`, pins the result row, and captures into
doc/source/_static/studio/:

  contributions-mode.png   the ranked per-study Shapley bars for the
                           pinned replicated finding

Run from studio/ with the dev venv (playwright + chromium installed):

    .venv/bin/python scripts/contrib_doc_shot.py

The viewport (1920x976, scale 1) matches the other full-page Studio shots
(circuit-mode.png, where-mode.png).
"""
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

from playwright.sync_api import expect, sync_playwright

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "doc" / "source" / "_static" / "studio"
PORT = 8078
URL = f"http://127.0.0.1:{PORT}"

# The replication query whose single row is the Exercise -> CVD ->
# beneficial finding; preceded by a set_prob so expected Shapley is
# well-defined regardless of the fixture's persisted probabilities.
QUERY = (
    "DO $$ BEGIN PERFORM set_prob(provenance(), reliability) FROM f; END $$;\n"
    "SELECT exposure, outcome, effect FROM f_replicated\n"
    " WHERE exposure = 'Exercise' AND outcome = 'Cardiovascular Disease'\n"
    "   AND effect = 'beneficial';"
)

server = subprocess.Popen(
    [sys.executable, "-m", "provsql_studio",
     "--host", "127.0.0.1", "--port", str(PORT),
     "--dsn", "dbname=cs2", "--search-path", "public",
     "--ignore-version"],
    stdout=open("/tmp/contrib_doc_shot_server.log", "wb"),
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
        sys.exit("server did not come up; see /tmp/contrib_doc_shot_server.log")

    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": 1920, "height": 976})
        page.goto(URL + "/contributions")
        expect(page.locator("body")).to_have_class(
            "mode-contributions", timeout=15000)

        # Label inputs by study name.
        page.locator("#contrib-mapping").select_option(label="study_mapping")

        # Run the query, then pin the single result row's provsql cell.
        page.locator("#request").fill(QUERY)
        page.locator("#run-btn").click()
        expect(page.locator("#result-count")).to_have_text("1", timeout=15000)
        page.locator("#result-body tr").first.locator("td").last.click()

        bars = page.locator("#contrib-chart .cv-contrib__bar")
        expect(bars.first).to_be_visible(timeout=15000)
        print("contribution bars:", bars.count())
        page.wait_for_timeout(400)

        page.screenshot(path=str(OUT / "contributions-mode.png"))
        print("contributions-mode.png")
        browser.close()
finally:
    server.terminate()
    try:
        server.wait(timeout=5)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()
