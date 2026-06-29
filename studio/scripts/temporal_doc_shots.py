"""Regenerate the Temporal-mode documentation screenshots.

Boots Studio on the case-study-4 databases (which must exist with the
provsql extension and the casestudy4 fixture loaded) and captures the
Temporal-mode shots used by the docs:

  doc/source/_static/studio/temporal-mode.png   (on cs4f, cs4_holds)
  doc/source/_static/casestudy4/cs4-asof-1981.png
  doc/source/_static/casestudy4/cs4-during-macron.png   (all three on cs4)
  doc/source/_static/casestudy4/cs4-full-chirac.png
  website/assets/images/temporal.png   (provsql.org hero, the During scene)

Each shot drives the live timeline through the same controls the e2e
suite exercises (tests/e2e/test_temporal_ui.py). Temporal mode locks the
:guilabel:`Provenance scheme` selector to Boolean, so every shot now
shows the locked Boolean pill.

Run from studio/ with the dev venv (playwright + chromium installed):

    .venv/bin/python scripts/temporal_doc_shots.py

The viewport is 1400 CSS px wide at deviceScaleFactor 2 (2800 px wide) with
a per-shot height so each viewport-sized (not full-page) capture frames the
controls + timeline + first result rows as the docs show; the browser
timezone is forced to UTC so the axis and window edges read as the docs show.
"""
import subprocess
import sys
import time
import urllib.request
from contextlib import contextmanager
from pathlib import Path

from playwright.sync_api import expect, sync_playwright

REPO = Path(__file__).resolve().parents[2]
STUDIO_OUT = REPO / "doc" / "source" / "_static" / "studio"
CS4_OUT = REPO / "doc" / "source" / "_static" / "casestudy4"
WEBSITE_OUT = REPO / "website" / "assets" / "images"
PORT = 8079
URL = f"http://127.0.0.1:{PORT}"


@contextmanager
def studio_server(dsn):
    """Boot a Studio server on PORT against `dsn`, tear it down on exit."""
    server = subprocess.Popen(
        [sys.executable, "-m", "provsql_studio",
         "--host", "127.0.0.1", "--port", str(PORT),
         "--dsn", dsn, "--search-path", "public",
         "--ignore-version"],
        stdout=open("/tmp/temporal_doc_shots_server.log", "wb"),
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
            sys.exit("server did not come up; see "
                     "/tmp/temporal_doc_shots_server.log")
        yield
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()


def new_temporal_page(browser, height, width=1400, scale=2):
    """A UTC-timezoned page at the temporal-shot viewport, in Temporal mode.

    The shot is the viewport (not full-page): `height` is chosen per shot so
    the top-anchored controls + timeline + first result rows are framed as the
    target shows. The doc shots use 1400 x height at deviceScaleFactor 2 (so
    2800 px wide); the website hero uses its own 1920 x height at scale 1.
    """
    ctx = browser.new_context(
        viewport={"width": width, "height": height},
        device_scale_factor=scale,
        timezone_id="UTC",
    )
    page = ctx.new_page()
    page.goto(URL + "/temporal")
    expect(page.locator("body")).to_have_class("mode-temporal", timeout=15000)
    # The Boolean lock is what these shots document; assert it is present so a
    # regression in the lock fails the capture rather than shipping a stale shot.
    expect(page.locator("#prov-scheme-fieldset")).to_have_class(
        "wp-prov-scheme is-locked", timeout=15000)
    # Temporal mode auto-fetches the default relation on load. That request is
    # slow for a big relation and, if still in flight when we drive our own
    # query, resolves late and clobbers it. Let it settle before driving so our
    # fetch is the last one standing.
    page.wait_for_load_state("networkidle")
    return page


MAPPING = "provsql.time_validity_view"


def settle(page):
    """Wait for any (possibly debounced) timeline fetch to fire and finish.

    Every control change re-fetches the timeline; some fetches are debounced
    and a fetch over a large default relation is slow. Serialising on this
    after each step guarantees no stale fetch is still in flight to clobber a
    later one -- the bug that put the default `holds` relation's rows under a
    `person_position` query. The 500 ms pre-wait clears the debounce window
    before networkidle waits for the request itself.
    """
    page.wait_for_timeout(500)
    page.wait_for_load_state("networkidle")


def use_query_source(page, query):
    """Switch to Query source and type `query` (each step serialised)."""
    page.locator('.cv-temporal__src[data-source="query"]').click()
    settle(page)
    page.locator("#request").fill(query)
    page.locator("#request").dispatch_event("change")
    settle(page)


def pick_relation(page, relation, box_sql):
    """Pick a Relation source target; the box shows its effective SELECT."""
    page.locator("#temporal-relation").select_option(relation)
    settle(page)
    page.locator("#request").fill(box_sql)


def pick_mapping(page):
    page.locator("#temporal-mapping").select_option(MAPPING)
    settle(page)


def set_timeop(page, op):
    page.locator(f'.cv-temporal__op[data-timeop="{op}"]').click()
    settle(page)


def set_instant(page, field, value):
    page.locator(f"#temporal-{field}").fill(value)
    page.locator(f"#temporal-{field}").dispatch_event("change")
    settle(page)


def set_sort(page, value):
    page.locator("#temporal-sort").select_option(value)
    settle(page)


def capture(page, out, expected_count):
    # Everything is serialised, so the on-screen result is our driven query.
    # The result-count assertion (set from the same single response that fills
    # the timeline) fails the capture loudly rather than shipping a raced shot.
    expect(page.locator("#result-count")).to_have_text(
        str(expected_count), timeout=15000)
    page.wait_for_timeout(300)
    page.screenshot(path=str(out))
    print(out.name, "->", out)


with sync_playwright() as p:
    browser = p.chromium.launch()

    # --- temporal-mode.png : cs4f, a During window over two PM terms --------
    with studio_server("dbname=cs4f"):
        page = new_temporal_page(browser, 760)
        pick_relation(page, "cs4_holds", "SELECT * FROM cs4_holds")
        pick_mapping(page)
        set_timeop(page, "during")
        set_instant(page, "from", "2014-01-01T00:00")
        set_instant(page, "to", "2018-01-01T00:00")
        capture(page, STUDIO_OUT / "temporal-mode.png", 2)
        page.context.close()

    # --- the three case-study-4 shots : cs4, person_position ---------------
    with studio_server("dbname=cs4"):
        # cs4-asof-1981 : Relation source, As of a single instant.
        page = new_temporal_page(browser, 900)
        pick_relation(page, "person_position", "SELECT * FROM person_position")
        pick_mapping(page)
        set_timeop(page, "asof")
        set_instant(page, "at", "1981-07-01T00:00")
        capture(page, CS4_OUT / "cs4-asof-1981.png", 10)
        page.context.close()

        # cs4-during-macron : Query source, During Macron's first term.
        page = new_temporal_page(browser, 760)
        use_query_source(page, "SELECT name, position FROM person_position\n"
                                " WHERE position = 'Prime Minister of France'")
        pick_mapping(page)
        set_timeop(page, "during")
        set_instant(page, "from", "2017-05-16T00:00")
        set_instant(page, "to", "2022-05-13T00:00")
        capture(page, CS4_OUT / "cs4-during-macron.png", 2)
        page.context.close()

        # cs4-full-chirac : Query source, Full validity, sorted by start.
        page = new_temporal_page(browser, 820)
        use_query_source(page, "SELECT position FROM person_position\n"
                                " WHERE name = 'Jacques Chirac'")
        pick_mapping(page)
        set_timeop(page, "full")
        set_sort(page, "start")
        capture(page, CS4_OUT / "cs4-full-chirac.png", 4)
        page.context.close()

        # website hero (provsql.org landing page): the During-Macron scene at
        # the site's own 1920 x 1042 scale-1 framing.
        page = new_temporal_page(browser, 1042, width=1920, scale=1)
        use_query_source(page, "SELECT name, position FROM person_position\n"
                                " WHERE position = 'Prime Minister of France'")
        pick_mapping(page)
        set_timeop(page, "during")
        set_instant(page, "from", "2017-05-16T00:00")
        set_instant(page, "to", "2022-05-13T00:00")
        capture(page, WEBSITE_OUT / "temporal.png", 2)
        page.context.close()

    browser.close()
