"""Notebook-mode e2e for the browser (PGlite + Pyodide) build.

The notebook front-end and the kernel endpoints run unmodified; what is
browser-specific -- and covered here -- is the single-session mapping:
the pinned "kernel connection" and the request pool share the one PGlite
session, kernel restart is expressed as DISCARD ALL by the psycopg shim,
the pagehide kernel-close beacon rides the postMessage bridge, and the
bundled example notebooks are served from the Pyodide FS.
"""
from __future__ import annotations

import re

from playwright.sync_api import Page, expect

from test_browser import ui

RUN_ALL_TIMEOUT = 300_000  # ms; a whole seeded notebook on WASM Postgres


def _nb_frame(page: Page):
    fr = ui(page)
    expect(fr.locator("#notebook-pane")).to_be_visible(timeout=30000)
    return fr


def test_notebook_mode_boots_and_keeps_state_across_cells(open_studio) -> None:
    """?mode=notebook boots the notebook UI in the Playground; a temp
    table created by one cell is visible to the next (the pinned-kernel
    Jupyter-state model, here mapped onto the shared PGlite session)."""
    page = open_studio(path="/app.html?mode=notebook")
    fr = _nb_frame(page)
    expect(fr.locator("#nb-kernel-label")).to_have_text("no kernel")

    ta = fr.locator(".nb-cell--sql .nb-cell__ta").first
    ta.fill("CREATE TEMP TABLE nbweb(x int);\n"
            "INSERT INTO nbweb VALUES (1),(2);\n"
            "SELECT count(*) AS n FROM nbweb;")
    fr.locator("#nb-run").click()
    expect(fr.locator(".nb-cell--sql").first.locator(".nb-out tbody td").first)\
        .to_have_text("2", timeout=60000)

    fr.locator("#nb-add-sql").click()
    ta2 = fr.locator(".nb-cell--sql .nb-cell__ta").nth(1)
    ta2.fill("SELECT sum(x) AS s FROM nbweb;")
    fr.locator("#nb-run").click()
    expect(fr.locator(".nb-cell--sql").nth(1).locator(".nb-out tbody td").first)\
        .to_have_text("3", timeout=60000)


def test_kernel_restart_discards_session_state(open_studio) -> None:
    """Restart maps onto DISCARD ALL in the psycopg shim (PGlite has one
    backend session, so close+reopen cannot mean a fresh process): the
    kernel's temp tables are gone, and the session keeps working
    afterwards (search_path is restored by the shim)."""
    page = open_studio(path="/app.html?mode=notebook")
    fr = _nb_frame(page)

    ta = fr.locator(".nb-cell--sql .nb-cell__ta").first
    ta.fill("CREATE TEMP TABLE nbrestart(x int);\n"
            "SELECT count(*) AS n FROM nbrestart;")
    fr.locator("#nb-run").click()
    expect(fr.locator(".nb-cell--sql").first.locator(".nb-out tbody td").first)\
        .to_have_text("0", timeout=60000)

    page.on("dialog", lambda d: d.accept())
    fr.locator("#nb-restart").click()
    expect(fr.locator("#nb-kernel-label")).to_have_text(
        re.compile(r"pid \d+"), timeout=60000)

    ta.fill("SELECT count(*) AS n FROM nbrestart;")
    fr.locator("#nb-run").click()
    cell = fr.locator(".nb-cell--sql").first
    expect(cell.locator(".nb-out .wp-error")).to_be_visible(timeout=60000)
    assert "nbrestart" in cell.locator(".nb-out .wp-error").inner_text()

    # The session survived the DISCARD: an unqualified provsql call still
    # resolves and the kernel runs further cells.
    ta.fill("SELECT 41 + 1 AS answer;")
    fr.locator("#nb-run").click()
    expect(cell.locator(".nb-out tbody td").first).to_have_text("42", timeout=60000)


def test_seeded_tutorial_notebook_runs_end_to_end(open_studio) -> None:
    """The ?nb= deep link opens the bundled tutorial notebook (served
    from the Pyodide FS) bound to the tutorial database, and Run all
    completes every SQL cell without an error banner -- the whole
    user-guide tutorial, provenance and probability computations
    included, on WASM Postgres."""
    page = open_studio(db="tutorial", path="/app.html?nb=tutorial")
    fr = _nb_frame(page)

    # The deep link opens the example in its own tab once the fetch
    # completes; wait for the tab to take the notebook's name, then for
    # the cell list to be substantial (the default Untitled tab shows a
    # single empty SQL cell first).
    expect(fr.locator(".nb__tab--active")).to_contain_text(
        re.compile("tutorial", re.I), timeout=60000)
    cells = fr.locator(".nb-cell--sql")
    expect(cells.nth(9)).to_be_attached(timeout=60000)

    fr.locator("#nb-run-all").click()
    # Completion: the last SQL cell gets an execution count.
    expect(cells.last.locator(".nb-cell__count"))\
        .to_have_text(re.compile(r"\[\d+\]"), timeout=RUN_ALL_TIMEOUT)
    assert fr.locator(".nb-out .wp-error").count() == 0
