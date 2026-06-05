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


def test_uuid_click_inserts_circuit_cell(page: Page, studio_url: str) -> None:
    """Clicking a provenance UUID in a result inserts a circuit cell
    right below the SQL cell, with the DAG painted as SVG; clicking a
    different UUID retargets the same cell instead of stacking new
    ones."""
    _goto_notebook(page, studio_url)
    ta = _sql_cell_ta(page)
    ta.fill("SELECT city, provsql.provenance() FROM "
            "(SELECT DISTINCT city FROM personnel) t WHERE city = 'Paris';")
    ta.focus()
    _run_focused(page)
    sql_cell = page.locator(".nb-cell--sql").first
    uuid_cell = sql_cell.locator(".nb-out [data-circuit-uuid]").first
    expect(uuid_cell).to_be_visible(timeout=8000)

    uuid_cell.click()
    circ = page.locator(".nb-cell--circuit")
    expect(circ).to_have_count(1, timeout=8000)
    # The DISTINCT-city Paris circuit is a + over 3 inputs.
    expect(circ.locator(".nb-circ__svg .node-group")).to_have_count(
        4, timeout=8000)
    expect(circ.locator(".node--plus")).to_have_count(1)
    expect(circ.locator(".node--input")).to_have_count(3)

    # Re-click the same UUID: still exactly one circuit cell.
    uuid_cell.click()
    expect(circ).to_have_count(1)


def test_circuit_cell_roundtrips_through_ipynb(page: Page, studio_url: str) -> None:
    """A saved circuit cell re-paints from the scene JSON on load,
    without a database fetch."""
    _goto_notebook(page, studio_url)
    ta = _sql_cell_ta(page)
    ta.fill("SELECT city, provsql.provenance() FROM "
            "(SELECT DISTINCT city FROM personnel) t WHERE city = 'Paris';")
    ta.focus()
    _run_focused(page)
    sql_cell = page.locator(".nb-cell--sql").first
    sql_cell.locator(".nb-out [data-circuit-uuid]").first.click(timeout=8000)
    circ = page.locator(".nb-cell--circuit")
    expect(circ.locator(".nb-circ__svg .node-group")).to_have_count(
        4, timeout=8000)

    with page.expect_download() as dl:
        page.locator("#nb-save").click()
    doc = json.loads(open(dl.value.path()).read())
    circ_cells = [c for c in doc["cells"]
                  if c.get("metadata", {}).get("provsql", {}).get("cell") == "circuit"]
    assert len(circ_cells) == 1
    saved = circ_cells[0]
    assert saved["metadata"]["provsql"]["token"]
    scene = saved["outputs"][0]["data"]["application/vnd.provsql.scene+json"]
    assert len(scene["nodes"]) == 4
    assert "image/svg+xml" in saved["outputs"][0]["data"]
    assert "-- circuit" in "".join(saved["source"])

    # Fresh page, load the file: the DAG re-paints without a kernel.
    page.evaluate("localStorage.removeItem('ps.nb.autosave')")
    page.reload()
    page.locator("#nb-load-input").set_input_files(str(dl.value.path()))
    circ2 = page.locator(".nb-cell--circuit")
    expect(circ2.locator(".nb-circ__svg .node-group")).to_have_count(
        4, timeout=8000)
    expect(page.locator("#nb-kernel-label")).to_have_text("no kernel")


def test_query_jump_to_circuit_mode(page: Page, studio_url: str) -> None:
    """The per-cell 'open in Circuit mode' action carries the cell's SQL
    through the standard mode-switch channel; an executed cell
    auto-replays there."""
    _goto_notebook(page, studio_url)
    ta = _sql_cell_ta(page)
    ta.fill("SELECT name FROM personnel ORDER BY id LIMIT 2;")
    ta.focus()
    _run_focused(page)
    cell = page.locator(".nb-cell--sql").first
    expect(cell.locator(".nb-out tbody tr")).to_have_count(2, timeout=8000)

    cell.hover()  # actions reveal on hover
    cell.locator("[data-act='to-circuit']").click()
    expect(page.locator("body")).to_have_class("mode-circuit", timeout=8000)
    expect(page.locator("#request")).to_have_value(
        "SELECT name FROM personnel ORDER BY id LIMIT 2;")
    expect(page.locator("#result-count")).to_have_text("2", timeout=8000)


def test_circuit_cell_jump_preloads_circuit_mode(page: Page, studio_url: str) -> None:
    """A circuit cell's 'Circuit mode' button lands on /circuit with the
    token's DAG preloaded (ps.preloadCircuit channel)."""
    _goto_notebook(page, studio_url)
    ta = _sql_cell_ta(page)
    ta.fill("SELECT city, provsql.provenance() FROM "
            "(SELECT DISTINCT city FROM personnel) t WHERE city = 'Paris';")
    ta.focus()
    _run_focused(page)
    page.locator(".nb-cell--sql .nb-out [data-circuit-uuid]").first.click(
        timeout=8000)
    circ = page.locator(".nb-cell--circuit")
    expect(circ.locator(".nb-circ__svg")).to_be_visible(timeout=8000)

    circ.locator(".nb-circ__jump").click()
    expect(page.locator("body")).to_have_class("mode-circuit", timeout=8000)
    # The DAG renders in the circuit-mode sidebar without further clicks.
    expect(page.locator("#sidebar-body svg .node-group").first)\
        .to_be_visible(timeout=8000)


def test_jupyter_keymap(page: Page, studio_url: str) -> None:
    """Jupyter-style command mode: Esc selects, b/a insert below/above,
    dd deletes, z undoes, m converts to markdown; Alt+Enter runs and
    inserts a cell below from edit mode."""
    _goto_notebook(page, studio_url)
    ta = _sql_cell_ta(page)
    ta.fill("SELECT 1 AS one;")
    ta.focus()
    # Esc -> command mode with this cell selected.
    page.keyboard.press("Escape")
    sql_cell = page.locator(".nb-cell--sql").first
    expect(sql_cell).to_have_class(re.compile(r"nb-cell--selected"))

    # b inserts below (selected), a inserts above the new selection.
    page.keyboard.press("b")
    expect(page.locator(".nb-cell--sql")).to_have_count(2)
    page.keyboard.press("a")
    expect(page.locator(".nb-cell--sql")).to_have_count(3)
    # The middle cell (index 1 among sql cells) is the a-inserted one.
    expect(page.locator(".nb-cell--selected")).to_have_count(1)

    # dd deletes the selected cell, z restores it.
    page.keyboard.press("d")
    page.keyboard.press("d")
    expect(page.locator(".nb-cell--sql")).to_have_count(2)
    page.keyboard.press("z")
    expect(page.locator(".nb-cell--sql")).to_have_count(3)

    # m converts the selected SQL cell to markdown, y back to SQL.
    page.keyboard.press("m")
    expect(page.locator(".nb-cell--markdown")).to_have_count(2)  # + default md
    page.keyboard.press("y")
    expect(page.locator(".nb-cell--markdown")).to_have_count(1)

    # m on a non-empty SQL cell wraps the content in a ```sql fence
    # (rendered as a code block); y unwraps it, round-tripping the SQL.
    page.locator(".nb-cell--sql").first.click()
    page.keyboard.press("m")
    converted = page.locator(".nb-cell--selected")
    expect(converted.locator(".nb-cell__md pre code")).to_contain_text(
        "SELECT 1 AS one;", timeout=8000)
    # The fenced block is syntax-highlighted with the editor tokenizer.
    expect(converted.locator(".nb-cell__md pre code .hl-kw").first)\
        .to_have_text("SELECT")
    page.keyboard.press("y")
    expect(page.locator(".nb-cell--selected .nb-cell__ta")).to_have_value(
        "SELECT 1 AS one;")

    # Alt+Enter from edit mode: runs the cell and opens a fresh one.
    n_before = page.locator(".nb-cell--sql").count()
    ta.focus()
    page.keyboard.press("Alt+Enter")
    expect(sql_cell.locator(".nb-out tbody tr")).to_have_count(1, timeout=8000)
    expect(page.locator(".nb-cell--sql")).to_have_count(n_before + 1)


def test_mode_roundtrip_returns_to_same_place(page: Page, studio_url: str) -> None:
    """Leaving for Circuit mode and coming back restores the notebook
    content (autosave), the selected cell, and the scroll position."""
    _goto_notebook(page, studio_url)
    ta = _sql_cell_ta(page)
    ta.fill("SELECT name FROM personnel;")
    ta.focus()
    # Build a tall notebook so there is something to scroll.
    page.keyboard.press("Escape")
    for _ in range(14):
        page.keyboard.press("b")
    expect(page.locator(".nb-cell--sql")).to_have_count(15)
    # Select the last cell (b leaves it selected) and scroll to it.
    page.evaluate("window.scrollTo(0, document.body.scrollHeight)")
    page.wait_for_timeout(150)
    scroll_before = page.evaluate("window.scrollY")
    assert scroll_before > 0

    # Round-trip through Circuit mode via the mode tabs.
    page.locator(".ps-modeswitch__btn[data-mode='circuit']").click()
    expect(page.locator("body")).to_have_class("mode-circuit", timeout=8000)
    page.locator(".ps-modeswitch__btn[data-mode='notebook']").click()
    expect(page.locator("body")).to_have_class("mode-notebook", timeout=8000)

    # Same cells, same selection (the last cell), same scroll offset.
    expect(page.locator(".nb-cell--sql")).to_have_count(15, timeout=8000)
    last = page.locator(".nb-cell--sql").nth(14)
    expect(last).to_have_class(re.compile(r"nb-cell--selected"))
    page.wait_for_timeout(300)  # double-raf scroll restore
    scroll_after = page.evaluate("window.scrollY")
    assert abs(scroll_after - scroll_before) < 60, (scroll_before, scroll_after)
