"""Regenerate the notebook-mode documentation screenshots.

Boots Studio on a database named `tutorial` (which must exist with the
provsql extension installed; the bundled tutorial notebook is
self-establishing, so an empty provsql-enabled database is enough),
opens the bundled tutorial example, runs it end to end, and captures
into doc/source/_static/studio/:

  notebook-mode.png           the executed notebook from the top
                              (tab bar, toolbar + kernel chip, title)
  notebook-circuit-cell.png   a circuit cell + its evaluation cell
  notebook-binding-banner.png the database-binding banner (opens the
                              cs1 example while connected to tutorial)

Run from studio/ with the dev venv (playwright + chromium installed):

    .venv/bin/python scripts/nb_doc_shots.py

The viewport (1920x976, scale 1) matches the other full-page Studio
shots (circuit-mode.png, where-mode.png).
"""
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

from playwright.sync_api import expect, sync_playwright

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "doc" / "source" / "_static" / "studio"
PORT = 8077
URL = f"http://127.0.0.1:{PORT}"

server = subprocess.Popen(
    [sys.executable, "-m", "provsql_studio",
     "--host", "127.0.0.1", "--port", str(PORT),
     "--dsn", "dbname=tutorial", "--search-path", "public",
     "--ignore-version"],
    stdout=open("/tmp/nb_doc_shots_server.log", "wb"),
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
        sys.exit("server did not come up; see /tmp/nb_doc_shots_server.log")

    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": 1920, "height": 976})
        page.goto(URL + "/notebook")
        page.wait_for_selector("#notebook-pane", timeout=15000)
        # Pristine state, then open the bundled tutorial example.
        page.evaluate("localStorage.removeItem('ps.nb.autosave');"
                      "localStorage.removeItem('ps.nb.tabs')")
        page.reload()
        page.wait_for_selector("#notebook-pane", timeout=15000)
        page.locator("#nb-example").select_option("tutorial")
        expect(page.locator(".nb__tab--active")).to_contain_text(
            "Tutorial", timeout=15000)
        cells = page.locator(".nb-cell--sql")
        expect(cells.nth(9)).to_be_attached(timeout=15000)

        # Run the whole notebook (native PG: fast).
        page.locator("#nb-run-all").click()
        expect(cells.last.locator(".nb-cell__count")).to_have_text(
            re.compile(r"\[\d+\]"), timeout=180000)
        n_err = page.locator(".nb-out .wp-error").count()
        print("run-all done; error banners:", n_err)
        if n_err:
            sys.exit("notebook run produced error banners; not capturing")

        # --- Shot 1: top of the executed notebook (tab bar + toolbar +
        # title + first cells with their execution counts). ---
        page.evaluate("window.scrollTo(0, 0)")
        page.wait_for_timeout(400)
        page.screenshot(path=str(OUT / "notebook-mode.png"))
        print("notebook-mode.png")

        # --- Shot 2: circuit + eval cells from a suspects UUID. ---
        # The Step-7 suspects table is a DISTINCT, so its tokens are
        # plus-gates over the matching sightings.
        suspects = page.locator(".nb-cell--sql",
                                has_text="CREATE TABLE suspects").first
        uuid_cell = suspects.locator(".nb-out [data-circuit-uuid]").first
        uuid_cell.scroll_into_view_if_needed()
        uuid_cell.click()
        circ = page.locator(".nb-cell--circuit").first
        expect(circ.locator(".nb-circ__svg .node-group").first)\
            .to_be_visible(timeout=15000)
        # Insert + run the evaluation cell (defaults: probability/exact).
        circ.locator(".nb-circ__eval").click()
        ev = page.locator(".nb-cell--eval").first
        ev.locator(".nb-cell__run").click()
        expect(ev.locator(".nb-eval__value")).not_to_have_text(
            "", timeout=30000)
        page.wait_for_timeout(300)
        # One clipped shot covering the circuit cell and the eval cell.
        circ.scroll_into_view_if_needed()
        page.wait_for_timeout(200)
        box1 = circ.bounding_box()
        box2 = ev.bounding_box()
        x = min(box1["x"], box2["x"]) - 40   # include the gutter icons
        y = box1["y"] - 8
        w = max(box1["width"], box2["width"]) + 56
        h = box2["y"] + box2["height"] - y + 16
        page.screenshot(path=str(OUT / "notebook-circuit-cell.png"),
                        clip={"x": x, "y": y, "width": w, "height": h})
        print("notebook-circuit-cell.png")

        # --- Shot 3: the database-binding banner. ---
        # Open a second example bound to a different database (cs1)
        # while connected to tutorial.
        page.locator("#nb-example").select_option("cs1")
        banner = page.locator("#nb-binding-banner")
        expect(banner).to_be_visible(timeout=15000)
        page.wait_for_timeout(300)
        b = banner.bounding_box()
        page.screenshot(path=str(OUT / "notebook-binding-banner.png"),
                        clip={"x": b["x"] - 8, "y": b["y"] - 8,
                              "width": b["width"] + 16,
                              "height": b["height"] + 16})
        print("notebook-binding-banner.png")

        browser.close()
finally:
    server.terminate()
    server.wait()
