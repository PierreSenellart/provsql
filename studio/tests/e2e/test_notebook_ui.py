"""End-to-end tests for the notebook mode: cell editing, kernel-backed
execution with state across cells, markdown rendering, restart, and the
.ipynb save round-trip."""
from __future__ import annotations

import json
import re

from playwright.sync_api import Page, expect


def _goto_notebook(page: Page, studio_url: str) -> None:
    page.goto(studio_url + "/notebook")
    expect(page.locator("body")).to_have_class("mode-notebook", timeout=5000)
    # Each test starts from a pristine default notebook, not a previous
    # test's autosaved draft.
    page.evaluate("localStorage.removeItem('ps.nb.autosave')")
    page.reload()
    expect(page.locator("#notebook-pane")).to_be_visible(timeout=5000)


def _sql_cell_ta(page: Page, idx: int = 0):
    return page.locator(".nb-cell--sql .nb-cell__ta").nth(idx)


def _run_focused(page: Page) -> None:
    page.locator("#nb-run").click()


def test_notebook_mode_boots_with_default_cells(page: Page, studio_url: str) -> None:
    _goto_notebook(page, studio_url)
    # Default notebook: one rendered markdown cell + one empty SQL cell.
    expect(page.locator(".nb-cell--markdown .nb-cell__md h1")).to_contain_text(
        "ProvSQL notebook", timeout=8000)  # marked.js lazy-loads
    expect(page.locator(".nb-cell--sql")).to_have_count(1)
    expect(page.locator("#nb-kernel-label")).to_have_text("no kernel")


def test_run_cell_renders_rows_and_starts_kernel(page: Page, studio_url: str) -> None:
    _goto_notebook(page, studio_url)
    ta = _sql_cell_ta(page)
    ta.fill("SELECT name FROM personnel ORDER BY id LIMIT 3;")
    ta.focus()
    _run_focused(page)
    cell = page.locator(".nb-cell--sql").first
    expect(cell.locator(".nb-out tbody tr")).to_have_count(3, timeout=8000)
    expect(cell.locator(".nb-cell__count")).to_have_text("[1]")
    expect(page.locator("#nb-kernel-label")).to_contain_text("pid", timeout=8000)


def test_state_persists_across_cells_and_restart_clears_it(
        page: Page, studio_url: str) -> None:
    _goto_notebook(page, studio_url)
    ta = _sql_cell_ta(page)
    ta.fill("CREATE TEMP TABLE nb_e2e(x int); INSERT INTO nb_e2e VALUES (1),(2);")
    ta.focus()
    _run_focused(page)
    cell0 = page.locator(".nb-cell--sql").nth(0)
    expect(cell0.locator(".nb-cell__count")).to_have_text("[1]", timeout=8000)

    # Second cell sees the temp table (the kernel's session state).
    cell0.locator("[data-act='add-sql']").click()
    ta2 = _sql_cell_ta(page, 1)
    ta2.fill("SELECT count(*) AS n FROM nb_e2e;")
    ta2.focus()
    _run_focused(page)
    cell1 = page.locator(".nb-cell--sql").nth(1)
    expect(cell1.locator(".nb-out tbody td").first).to_have_text("2", timeout=8000)

    # Restart the kernel: counters reset, temp table is gone.
    page.on("dialog", lambda d: d.accept())
    page.locator("#nb-restart").click()
    expect(cell1.locator(".nb-cell__count")).to_have_text("[ ]", timeout=8000)
    ta2.focus()
    _run_focused(page)
    expect(cell1.locator(".nb-out .wp-error")).to_contain_text(
        "nb_e2e", timeout=8000)


def test_error_cell_shows_banner_kernel_survives(page: Page, studio_url: str) -> None:
    _goto_notebook(page, studio_url)
    ta = _sql_cell_ta(page)
    ta.fill("SELECT 1/0;")
    ta.focus()
    _run_focused(page)
    cell = page.locator(".nb-cell--sql").first
    expect(cell.locator(".nb-out .wp-error")).to_contain_text(
        "division by zero", timeout=8000)
    # Kernel survives an error cell.
    expect(page.locator("#nb-kernel-chip")).to_have_class(
        "nb__kernel nb__kernel--alive")


def test_markdown_cell_edit_and_render(page: Page, studio_url: str) -> None:
    _goto_notebook(page, studio_url)
    md = page.locator(".nb-cell--markdown").first
    md.locator(".nb-cell__md").dblclick()
    ta = md.locator(".nb-cell__mdta")
    expect(ta).to_be_visible()
    ta.fill("## Hello *notebook*")
    ta.blur()
    expect(md.locator(".nb-cell__md h2")).to_contain_text("Hello", timeout=8000)
    expect(md.locator(".nb-cell__md em")).to_have_text("notebook")


def test_markdown_render_is_sanitized(page: Page, studio_url: str) -> None:
    _goto_notebook(page, studio_url)
    md = page.locator(".nb-cell--markdown").first
    md.locator(".nb-cell__md").dblclick()
    ta = md.locator(".nb-cell__mdta")
    ta.fill('hi <img src=x onerror="window.__pwned = true"> there')
    ta.blur()
    expect(md.locator(".nb-cell__md")).to_contain_text("hi", timeout=8000)
    assert page.evaluate("window.__pwned") is None


def test_sidebar_is_compact_outline_plus_relations(page: Page, studio_url: str) -> None:
    """The notebook sidebar is the compact outline + relations summary,
    not the where-mode full-table browser; clicking a relation inserts
    its name into the current SQL cell, clicking an outline entry
    scrolls to the cell."""
    _goto_notebook(page, studio_url)
    # No full-table browser (where mode renders relation tables).
    expect(page.locator("#sidebar-body .nb-side")).to_be_visible(timeout=8000)
    assert page.locator("#sidebar-body table").count() == 0

    # The default markdown cell's heading shows up in the outline.
    outline = page.locator(".nb-side__outline .nb-side__h")
    expect(outline.first).to_have_text("ProvSQL notebook", timeout=8000)

    # The personnel relation is listed with its prov pill and a compact
    # column line.
    rel = page.locator(".nb-side__rel", has_text="personnel").first
    expect(rel).to_be_visible(timeout=8000)
    expect(rel.locator(".wp-result__col-prov")).to_have_text("prov")
    expect(rel.locator(".nb-side__cols")).to_contain_text("name")

    # Click-to-insert into the focused SQL cell.
    ta = _sql_cell_ta(page)
    ta.fill("SELECT count(*) FROM ")
    ta.focus()
    rel.locator(".nb-side__relname").click()
    # The inserted name is schema-qualified when the bare name does not
    # resolve on the server's search_path.
    expect(ta).to_have_value(
        re.compile(r"SELECT count\(\*\) FROM (provsql_test\.)?personnel"))

    # Outline click scrolls to (and flashes) the markdown cell.
    outline.first.click()
    expect(page.locator(".nb-cell--flash")).to_have_count(1)


def test_schema_panel_fills_selected_cell_or_adds_one(
        page: Page, studio_url: str) -> None:
    """Schema-panel prefills route to the notebook: into the SQL cell
    that was selected when the panel was used, or into a fresh cell
    when none was."""
    _goto_notebook(page, studio_url)
    # Case 1: a cell is selected -> the prefill replaces its content.
    ta = _sql_cell_ta(page)
    ta.fill("-- draft")
    ta.focus()
    page.locator("#schema-btn").click()
    row = page.locator(".wp-schema__rel", has_text="personnel").first
    expect(row).to_be_visible(timeout=8000)
    row.click()
    expect(ta).to_have_value(re.compile(r"SELECT \* FROM .*personnel;"))
    expect(page.locator(".nb-cell--sql")).to_have_count(1)

    # Case 2: fresh page, no SQL cell ever focused -> the prefill lands
    # in a freshly appended cell (the never-touched default cell is
    # left alone).
    page.evaluate("localStorage.removeItem('ps.nb.autosave')")
    page.reload()
    expect(page.locator(".nb-cell--sql")).to_have_count(1, timeout=5000)
    page.locator("#schema-btn").click()
    row = page.locator(".wp-schema__rel", has_text="personnel").first
    expect(row).to_be_visible(timeout=8000)
    row.click()
    expect(page.locator(".nb-cell--sql")).to_have_count(2)
    expect(page.locator(".nb-cell--sql .nb-cell__ta").nth(1)).to_have_value(
        re.compile(r"SELECT \* FROM .*personnel;"))


def test_fingerprint_toggle_expands_uuids(page: Page, studio_url: str) -> None:
    """The toolbar fingerprint toggle flips result-table UUID cells
    between the abbreviated and full form (body.show-uuids switch,
    same mechanism as circuit mode)."""
    _goto_notebook(page, studio_url)
    ta = _sql_cell_ta(page)
    ta.fill("SELECT provsql.provenance() AS p FROM personnel LIMIT 1;")
    ta.focus()
    _run_focused(page)
    cell = page.locator(".nb-cell--sql").first
    short = cell.locator(".nb-out .wp-uuid__short").first
    full = cell.locator(".nb-out .wp-uuid__full").first
    expect(short).to_be_visible(timeout=8000)
    expect(short).to_have_text(re.compile(r"^[0-9a-f]{4}…$"))
    expect(full).to_be_hidden()

    page.locator("#nb-show-uuids").click()
    expect(full).to_be_visible()
    expect(full).to_have_text(re.compile(r"^[0-9a-f-]{36}$"))
    expect(short).to_be_hidden()

    page.locator("#nb-show-uuids").click()
    expect(short).to_be_visible()


def test_save_produces_valid_ipynb(page: Page, studio_url: str) -> None:
    _goto_notebook(page, studio_url)
    ta = _sql_cell_ta(page)
    ta.fill("SELECT 42 AS answer;")
    ta.focus()
    _run_focused(page)
    cell = page.locator(".nb-cell--sql").first
    expect(cell.locator(".nb-out tbody tr")).to_have_count(1, timeout=8000)

    with page.expect_download() as dl:
        page.locator("#nb-save").click()
    doc = json.loads(open(dl.value.path()).read())
    assert doc["nbformat"] == 4
    assert doc["metadata"]["kernelspec"]["name"] == "provsql-studio"
    types = [c["cell_type"] for c in doc["cells"]]
    assert types == ["markdown", "code"]
    code = doc["cells"][1]
    assert "SELECT 42 AS answer;" in "".join(code["source"])
    assert code["execution_count"] == 1
    out = code["outputs"][0]
    blocks = out["data"]["application/vnd.provsql.blocks+json"]["blocks"]
    assert blocks[-1]["kind"] == "rows"
    assert blocks[-1]["rows"][0][0] == 42
    assert "text/html" in out["data"]


def test_load_ipynb_rerenders_saved_outputs(page: Page, studio_url: str, tmp_path) -> None:
    """Loading a notebook re-renders saved outputs from the JSON payload
    without executing anything (no kernel is started)."""
    doc = {
        "nbformat": 4, "nbformat_minor": 5,
        "metadata": {"kernelspec": {"name": "provsql-studio",
                                    "display_name": "ProvSQL (SQL)",
                                    "language": "sql"},
                     "provsql": {"scheme": "semiring"}},
        "cells": [
            {"cell_type": "markdown", "metadata": {},
             "source": ["# Loaded notebook\n"]},
            {"cell_type": "code", "execution_count": 7,
             "metadata": {}, "source": ["SELECT 'saved' AS v;"],
             "outputs": [{
                 "output_type": "execute_result", "execution_count": 7,
                 "metadata": {},
                 "data": {"application/vnd.provsql.blocks+json": {
                     "blocks": [{"kind": "rows",
                                 "columns": [{"name": "v", "type_name": "text"}],
                                 "rows": [["saved"]]}],
                     "wrapped": False, "notices": []}},
             }]},
        ],
    }
    nb_file = tmp_path / "loaded.ipynb"
    nb_file.write_text(json.dumps(doc))

    _goto_notebook(page, studio_url)
    page.locator("#nb-load-input").set_input_files(str(nb_file))
    expect(page.locator(".nb-cell--markdown .nb-cell__md h1")).to_contain_text(
        "Loaded notebook", timeout=8000)
    cell = page.locator(".nb-cell--sql").first
    expect(cell.locator(".nb-cell__count")).to_have_text("[7]")
    expect(cell.locator(".nb-out tbody td").first).to_have_text("saved")
    expect(page.locator("#nb-kernel-label")).to_have_text("no kernel")
