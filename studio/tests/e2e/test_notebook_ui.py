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
    page.evaluate("localStorage.removeItem('ps.nb.autosave');"
                  "localStorage.removeItem('ps.nb.tabs')")
    page.reload()
    expect(page.locator("#notebook-pane")).to_be_visible(timeout=5000)


def _sql_cell_ta(page: Page, idx: int = 0):
    return page.locator(".nb-cell--sql .nb-cell__ta").nth(idx)


def _run_focused(page: Page) -> None:
    page.locator("#nb-run").click()


def _download_notebook(page: Page):
    """Drive the save path deterministically: disable the
    showSaveFilePicker branch (headless Chromium cannot show it) so the
    classic download fallback fires."""
    page.evaluate("window.showSaveFilePicker = undefined")
    with page.expect_download() as dl:
        page.locator("#nb-save").click()
    return dl.value


def test_notebook_mode_boots_with_default_cells(page: Page, studio_url: str) -> None:
    _goto_notebook(page, studio_url)
    # Default notebook: a single empty SQL cell, no boilerplate
    # markdown, so the fresh tab stays "Untitled".
    expect(page.locator(".nb-cell--sql")).to_have_count(1)
    expect(page.locator(".nb-cell--markdown")).to_have_count(0)
    expect(page.locator(".nb__tab--active")).to_contain_text("Untitled")
    expect(page.locator("#nb-kernel-label")).to_have_text("no kernel")


def _add_markdown_cell(page: Page, text: str) -> None:
    """Append a markdown cell via the toolbar and give it `text`. A
    fresh empty markdown cell drops straight into edit mode."""
    page.locator("#nb-add-md").click()
    ta = page.locator(".nb-cell__mdta:not([hidden])").last
    expect(ta).to_be_visible()
    ta.fill(text)
    ta.blur()


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
    _add_markdown_cell(page, "## Hello *notebook*")
    md = page.locator(".nb-cell--markdown").first
    expect(md.locator(".nb-cell__md h2")).to_contain_text("Hello", timeout=8000)
    expect(md.locator(".nb-cell__md em")).to_have_text("notebook")
    # Double-click re-enters edit mode.
    md.locator(".nb-cell__md").dblclick()
    expect(md.locator(".nb-cell__mdta")).to_be_visible()


def test_markdown_render_is_sanitized(page: Page, studio_url: str) -> None:
    _goto_notebook(page, studio_url)
    _add_markdown_cell(page,
                       'hi <img src=x onerror="window.__pwned = true"> there')
    md = page.locator(".nb-cell--markdown").first
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

    # Markdown headings show up in the outline.
    _add_markdown_cell(page, "# My analysis")
    outline = page.locator(".nb-side__outline .nb-side__h")
    expect(outline.first).to_have_text("My analysis", timeout=8000)

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
    page.evaluate("localStorage.removeItem('ps.nb.autosave');"
                  "localStorage.removeItem('ps.nb.tabs')")
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

    dl = _download_notebook(page)
    doc = json.loads(open(dl.path()).read())
    assert doc["nbformat"] == 4
    assert doc["metadata"]["kernelspec"]["name"] == "provsql-studio"
    types = [c["cell_type"] for c in doc["cells"]]
    assert types == ["code"]
    code = doc["cells"][0]
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


def test_load_sql_file_appends_sql_cell(page: Page, studio_url: str,
                                        tmp_path) -> None:
    """The Load button also accepts a .sql file: its content lands in the
    current notebook as one appended SQL cell (cleaned of CRLF and invisible
    Unicode like a paste), ready to run."""
    sql_file = tmp_path / "fixture.sql"
    sql_file.write_text("SELECT\u00a01 AS a;\r\nSELECT 2 AS b;\u200b\n")

    _goto_notebook(page, studio_url)
    page.locator("#nb-load-input").set_input_files(str(sql_file))
    ta = _sql_cell_ta(page, 1)   # appended after the pristine empty cell
    expect(ta).to_have_value("SELECT 1 AS a;\nSELECT 2 AS b;\n", timeout=8000)
    ta.focus()
    _run_focused(page)
    cell = page.locator(".nb-cell--sql").nth(1)
    expect(cell.locator(".nb-out tbody td").first).to_have_text("2", timeout=8000)
    assert cell.locator(".nb-out .wp-error").count() == 0


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

    dl = _download_notebook(page)
    doc = json.loads(open(dl.path()).read())
    circ_cells = [c for c in doc["cells"]
                  if c.get("metadata", {}).get("provsql", {}).get("cell") == "circuit"]
    assert len(circ_cells) == 1
    saved = circ_cells[0]
    assert saved["metadata"]["provsql"]["token"]
    scene = saved["outputs"][0]["data"]["application/vnd.provsql.scene+json"]
    assert len(scene["nodes"]) == 4
    assert "image/svg+xml" in saved["outputs"][0]["data"]
    # The snapshot must be a SELF-SUFFICIENT SVG: external viewers
    # (nbviewer, GitHub) see neither app.css nor an HTML context, so the
    # painter inlines xmlns plus the presentation attributes that the
    # stylesheet provides live (white node fill, purple strokes, centred
    # labels, fill-less edges -- without which paths render as black
    # blobs under SVG defaults).
    svg = "".join(saved["outputs"][0]["data"]["image/svg+xml"])
    assert svg.startswith("<svg")
    assert 'xmlns="http://www.w3.org/2000/svg"' in svg
    assert 'fill="none"' in svg          # edges
    assert 'fill="#fff"' in svg          # node disks
    assert 'text-anchor="middle"' in svg  # labels
    assert "-- circuit" in "".join(saved["source"])

    # Fresh page, load the file: the DAG re-paints without a kernel.
    page.evaluate("localStorage.removeItem('ps.nb.autosave');"
                  "localStorage.removeItem('ps.nb.tabs')")
    page.reload()
    page.locator("#nb-load-input").set_input_files(str(dl.path()))
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
    expect(page.locator(".nb-cell--markdown")).to_have_count(1)
    page.keyboard.press("y")
    expect(page.locator(".nb-cell--markdown")).to_have_count(0)

    # m on a non-empty SQL cell wraps the content in a ```sql fence
    # (rendered as a code block); y unwraps it, round-tripping the SQL.
    # Click the gutter (not the editor: that would enter edit mode and
    # swallow the keystroke).
    page.locator(".nb-cell--sql").first.locator(".nb-cell__gutter").click()
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


def test_shift_enter_selects_next_without_editing(page: Page, studio_url: str) -> None:
    """Shift+Enter follows Jupyter: run, then *select* the next cell in
    command mode -- a following markdown cell stays rendered with its
    editor closed. At the last cell, a fresh SQL cell is created below
    and opened in edit mode (Jupyter's last-cell exception)."""
    _goto_notebook(page, studio_url)
    ta = _sql_cell_ta(page)
    ta.fill("SELECT 1 AS one;")
    _add_markdown_cell(page, "*a note*")
    ta.focus()
    page.keyboard.press("Shift+Enter")
    # The SQL cell ran...
    expect(page.locator(".nb-cell--sql").first.locator(".nb-out tbody tr"))\
        .to_have_count(1, timeout=8000)
    # ...and the markdown cell below is selected but NOT in edit mode:
    # rendered view shown, source editor hidden, no focused textarea.
    md = page.locator(".nb-cell--markdown").first
    expect(md).to_have_class(re.compile(r"nb-cell--selected"))
    expect(md.locator(".nb-cell__md")).to_be_visible()
    expect(md.locator(".nb-cell__mdta")).to_be_hidden()
    assert page.evaluate("document.activeElement.tagName") != "TEXTAREA"
    # Shift+Enter again (command mode, last cell): a fresh SQL cell is
    # created below and opened in edit mode.
    page.keyboard.press("Shift+Enter")
    expect(page.locator(".nb-cell--sql")).to_have_count(2)
    expect(page.locator(".nb-cell--sql").last.locator(".nb-cell__ta"))\
        .to_be_focused()


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


def test_tabs_create_switch_and_persist(page: Page, studio_url: str) -> None:
    """The + button opens an independent notebook tab; switching tabs
    preserves each tab's cells and outputs; the tab set survives a
    reload."""
    _goto_notebook(page, studio_url)
    ta = _sql_cell_ta(page)
    ta.fill("SELECT 1 AS first_tab;")
    ta.focus()
    _run_focused(page)
    expect(page.locator(".nb-out tbody td").first).to_have_text("1", timeout=8000)

    page.locator("#nb-tab-add").click()
    expect(page.locator(".nb__tab")).to_have_count(2)
    # Fresh tab: default notebook, no outputs.
    expect(page.locator(".nb-out tbody td")).to_have_count(0)
    ta2 = _sql_cell_ta(page)
    ta2.fill("SELECT 2 AS second_tab;")

    # Back to tab 1: content and rendered output are intact.
    page.locator(".nb__tab").first.click()
    expect(_sql_cell_ta(page)).to_have_value("SELECT 1 AS first_tab;")
    expect(page.locator(".nb-out tbody td").first).to_have_text("1")

    # Reload: both tabs come back, tab 1 active with its output.
    page.reload()
    expect(page.locator(".nb__tab")).to_have_count(2, timeout=8000)
    expect(_sql_cell_ta(page)).to_have_value("SELECT 1 AS first_tab;")
    expect(page.locator(".nb-out tbody td").first).to_have_text("1")

    # Tabs name themselves from the first level-1 Markdown heading.
    expect(page.locator(".nb__tab--active")).to_contain_text("Untitled")
    _add_markdown_cell(page, "# Renamed via heading")
    page.wait_for_timeout(700)  # autosave tick re-renders the bar
    expect(page.locator(".nb__tab--active")).to_contain_text(
        "Renamed via heading")


def test_loaded_notebook_opens_new_tab_with_binding_banner(
        page: Page, studio_url: str, tmp_path) -> None:
    """Loading an .ipynb opens a new tab named after the file; a binding
    to a database other than the live one raises the banner, and
    'Rebind' adopts the current database."""
    doc = {
        "nbformat": 4, "nbformat_minor": 5,
        "metadata": {"provsql": {"scheme": "semiring",
                                 "database": "some_other_db"}},
        "cells": [{"cell_type": "code", "execution_count": None,
                   "metadata": {}, "source": ["SELECT 1;"], "outputs": []}],
    }
    nb = tmp_path / "myanalysis.ipynb"
    nb.write_text(json.dumps(doc))

    _goto_notebook(page, studio_url)
    page.locator("#nb-load-input").set_input_files(str(nb))
    # New tab named after the file, with the foreign-db chip. The
    # pristine Untitled tab the load started from is dropped.
    expect(page.locator(".nb__tab")).to_have_count(1)
    active = page.locator(".nb__tab--active")
    expect(active).to_contain_text("myanalysis")
    expect(active.locator(".nb__tab-db")).to_have_text("some_other_db")

    banner = page.locator("#nb-binding-banner")
    expect(banner).to_be_visible()
    expect(banner).to_contain_text("some_other_db")
    # The bound database does not exist: Create is offered, Switch is
    # not (offering both would be nonsense).
    expect(banner.locator("#nb-bind-create")).to_be_visible()
    expect(banner.locator("#nb-bind-switch")).to_have_count(0)
    banner.locator("#nb-bind-keep").click()
    expect(banner).to_be_hidden()
    expect(active.locator(".nb__tab-db")).to_have_count(0)


def test_saved_notebook_stamps_database_binding(page: Page, studio_url: str) -> None:
    _goto_notebook(page, studio_url)
    current_db = page.evaluate(
        "fetch('/api/conn').then(r => r.json()).then(c => c.database)")
    dl = _download_notebook(page)
    doc = json.loads(open(dl.path()).read())
    assert doc["metadata"]["provsql"]["database"] == current_db


def test_db_switch_opens_tab_bound_to_new_database(page: Page, studio_url: str) -> None:
    """Switching the connection to another database boots the notebook
    with a fresh tab bound to it; the old tab stays, showing its
    binding."""
    _goto_notebook(page, studio_url)
    original_db = page.evaluate(
        "fetch('/api/conn').then(r => r.json()).then(c => c.database)")
    ta = _sql_cell_ta(page)
    ta.fill("SELECT 'tab for ' || current_database();")
    page.wait_for_timeout(700)  # let the autosave flush

    try:
        # Switch to the always-present postgres maintenance DB.
        page.evaluate("""db => fetch('/api/conn', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({database: db})
        }).then(r => r.json())""", "postgres")
        page.reload()
        expect(page.locator(".nb__tab")).to_have_count(2, timeout=8000)
        # The new active tab is a fresh Untitled notebook bound to
        # postgres.
        active = page.locator(".nb__tab--active")
        expect(active).to_contain_text("Untitled")
        expect(page.locator("#nb-binding-banner")).to_be_hidden()
        # The old tab still lists its binding (now foreign).
        other = page.locator(".nb__tab").first
        expect(other.locator(".nb__tab-db")).to_have_text(original_db)
        # Activating the old tab raises the banner with a switch offer.
        other.click()
        banner = page.locator("#nb-binding-banner")
        expect(banner).to_be_visible()
        # That database exists: Switch is offered, Create is not.
        expect(banner.locator("#nb-bind-switch")).to_contain_text(original_db)
        expect(banner.locator("#nb-bind-create")).to_have_count(0)
    finally:
        page.evaluate("""db => fetch('/api/conn', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({database: db})
        }).then(r => r.json())""", original_db)


def test_scheme_carries_to_circuit_mode(page: Page, studio_url: str) -> None:
    """Picking a provenance scheme in the notebook carries over when
    switching to Circuit mode (shared ps.opt.provScheme channel), and
    a Circuit-mode pick seeds fresh notebooks on the way back."""
    _goto_notebook(page, studio_url)
    page.locator("#nb-scheme").select_option("boolean")
    page.locator(".ps-modeswitch__btn[data-mode='circuit']").click()
    expect(page.locator("body")).to_have_class("mode-circuit", timeout=8000)
    expect(page.locator(
        "input[name='prov-scheme'][value='boolean']")).to_be_checked()

    # And back: the notebook inherits the session's scheme.
    page.locator(".ps-modeswitch__btn[data-mode='notebook']").click()
    expect(page.locator("body")).to_have_class("mode-notebook", timeout=8000)
    expect(page.locator("#nb-scheme")).to_have_value("boolean")


def _make_circuit_cell(page: Page) -> None:
    """SQL cell -> run -> click the result UUID -> circuit cell below.

    Uses its own tiny table (not `personnel`: setting probabilities
    there would leak into the unit tests sharing the session DB)."""
    ta = _sql_cell_ta(page)
    ta.fill("DROP TABLE IF EXISTS nb_eval_pts;\n"
            "CREATE TABLE nb_eval_pts(city text);\n"
            "INSERT INTO nb_eval_pts VALUES ('Paris'), ('Paris'), ('Paris');\n"
            "SELECT provsql.add_provenance('nb_eval_pts');\n"
            "SELECT provsql.set_prob(provsql, 0.5) FROM nb_eval_pts;\n"
            "SELECT city, provsql.provenance() FROM "
            "(SELECT DISTINCT city FROM nb_eval_pts) t;")
    ta.focus()
    _run_focused(page)
    page.locator(".nb-cell--sql .nb-out [data-circuit-uuid]").first.click(
        timeout=8000)
    expect(page.locator(".nb-cell--circuit .nb-circ__svg")).to_be_visible(
        timeout=8000)


def test_eval_cell_probability_and_boolexpr(page: Page, studio_url: str) -> None:
    """The circuit cell's Evaluate button (offered for plain provenance
    tokens) inserts an evaluation cell; probability/exact on the 3-way
    Paris OR with p=0.5 inputs is 1 - 0.5^3 = 0.875; the mapping-free
    boolexpr semiring re-evaluates symbolically."""
    _goto_notebook(page, studio_url)
    _make_circuit_cell(page)
    expect(page.locator(".nb-circ__eval")).to_be_visible()
    page.locator(".nb-circ__eval").click()
    ev = page.locator(".nb-cell--eval")
    expect(ev).to_have_count(1)
    # Defaults: probability / exact. Run it (gutter play button).
    ev.locator(".nb-cell__run").click()
    expect(ev.locator(".nb-eval__value")).to_have_text("0.875", timeout=8000)
    expect(ev.locator(".nb-eval__meta").first).to_contain_text("via")
    expect(ev.locator(".nb-cell__count")).to_have_text(re.compile(r"\[\d+\]"))

    ev.locator(".nb-eval__semiring").select_option("boolexpr")
    ev.locator(".nb-cell__run").click()
    expect(ev.locator(".nb-eval__value")).to_contain_text("∨", timeout=8000)


def test_eval_cell_roundtrips_through_ipynb(page: Page, studio_url: str) -> None:
    """Saved evaluation cells record the invocation in metadata and the
    result in outputs; loading re-renders without re-evaluating."""
    _goto_notebook(page, studio_url)
    _make_circuit_cell(page)
    page.locator(".nb-circ__eval").click()
    ev = page.locator(".nb-cell--eval")
    ev.locator(".nb-cell__run").click()
    expect(ev.locator(".nb-eval__value")).to_have_text("0.875", timeout=8000)

    dl = _download_notebook(page)
    doc = json.loads(open(dl.path()).read())
    evals = [c for c in doc["cells"]
             if c.get("metadata", {}).get("provsql", {}).get("cell") == "eval"]
    assert len(evals) == 1
    meta = evals[0]["metadata"]["provsql"]
    assert meta["semiring"] == "probability" and meta["method"] == "exact"
    assert meta["token"]
    out = evals[0]["outputs"][0]["data"]
    assert out["application/vnd.provsql.eval+json"]["result"] == 0.875
    assert "".join(out["text/plain"]) == "0.875"
    assert "-- evaluate probability/exact" in "".join(evals[0]["source"])

    # Fresh page, load: result renders with no kernel started.
    page.evaluate("localStorage.removeItem('ps.nb.autosave');"
                  "localStorage.removeItem('ps.nb.tabs')")
    page.reload()
    page.locator("#nb-load-input").set_input_files(str(dl.path()))
    ev2 = page.locator(".nb-cell--eval")
    expect(ev2.locator(".nb-eval__value")).to_have_text("0.875", timeout=8000)
    expect(page.locator("#nb-kernel-label")).to_have_text("no kernel")


def test_open_example_menu_and_deep_link(page: Page, studio_url: str) -> None:
    """The Open-example menu loads a bundled notebook into a new tab
    bound to its database; /notebook?nb=<name> deep-links to it."""
    _goto_notebook(page, studio_url)
    sel = page.locator("#nb-example")
    expect(sel.locator("option[value='tutorial']")).to_have_count(1, timeout=8000)
    sel.select_option("tutorial")
    # New tab named from the notebook's H1, bound to `tutorial` (foreign
    # db -> chip + banner offering to create it).
    active = page.locator(".nb__tab--active")
    expect(active).to_contain_text("Who Killed Daphine", timeout=8000)
    expect(active.locator(".nb__tab-db")).to_have_text("tutorial")
    expect(page.locator("#nb-binding-banner")).to_be_visible()
    # The setup cells came along, COPY data included.
    expect(page.locator(".nb-cell--sql").first).to_be_visible()
    assert page.locator(".nb-cell--sql .nb-cell__ta").evaluate_all(
        "tas => tas.some(t => t.value.includes('FROM stdin;'))")

    # Deep link in a fresh state: opens the example directly.
    page.evaluate("localStorage.removeItem('ps.nb.autosave');"
                  "localStorage.removeItem('ps.nb.tabs')")
    page.goto(studio_url + "/notebook?nb=cs1")
    expect(page.locator(".nb__tab--active")).to_contain_text(
        "Intelligence Agency", timeout=8000)


def test_empty_db_button_confirms_and_wipes(
        page: Page, studio_url: str, test_dsn: str) -> None:
    """The nav broom asks for confirmation naming the database, then
    drops every user object; dismissing the confirm does nothing.

    The wipe hits the session-shared fixture database, so the fixture
    schema (personnel & co.) is rebuilt in the finally block for the
    tests that follow."""
    _goto_notebook(page, studio_url)
    ta = _sql_cell_ta(page)
    ta.fill("CREATE TABLE empty_btn_probe(x int);")
    ta.focus()
    _run_focused(page)
    cell = page.locator(".nb-cell--sql").first
    expect(cell.locator(".nb-out")).to_contain_text("CREATE", timeout=8000)

    # Dismissed confirm -> nothing happens.
    page.once("dialog", lambda d: d.dismiss())
    page.locator("#empty-db-btn").click()
    page.wait_for_timeout(400)
    ta.fill("SELECT count(*) FROM empty_btn_probe;")
    _run_focused(page)
    expect(cell.locator(".nb-out tbody td").first).to_have_text("0", timeout=8000)

    # Accepted confirm -> wipe + reload; the probe table is gone.
    try:
        page.once("dialog", lambda d: d.accept())
        running_pid = page.locator("#nb-kernel-label").inner_text()
        page.locator("#empty-db-btn").click()
        # The wipe reloads the page; wait for the post-reload state (the
        # kernel chip resets) rather than racing the old page, whose
        # kernel the server just closed.
        expect(page.locator("#nb-kernel-label")).not_to_have_text(
            running_pid, timeout=10000)
        expect(page.locator("#nb-kernel-label")).to_have_text(
            "no kernel", timeout=10000)
        ta2 = _sql_cell_ta(page)
        ta2.fill("SELECT count(*) FROM empty_btn_probe;")
        ta2.focus()
        _run_focused(page)
        expect(page.locator(".nb-cell--sql").first.locator(".nb-out .wp-error"))\
            .to_contain_text("empty_btn_probe", timeout=8000)
    finally:
        # Rebuild the shared fixture schema the wipe destroyed.
        import psycopg
        from pathlib import Path
        repo = Path(__file__).resolve().parents[3]
        with psycopg.connect(test_dsn, autocommit=True) as conn:
            # The fixture script expects a virgin database (plain
            # CREATE EXTENSION); the wipe reinstalled provsql, so shed
            # it first.
            conn.execute("DROP EXTENSION IF EXISTS provsql CASCADE")
            conn.execute("DROP SCHEMA IF EXISTS provsql CASCADE")
            for fname in ("setup.sql", "add_provenance.sql"):
                sql_text = "\n".join(
                    line for line in
                    (repo / "test" / "sql" / fname).read_text().splitlines()
                    if not line.startswith("\\"))
                conn.execute(sql_text)
        # The restore re-created the extension behind the server's back;
        # bounce its pool (same-database connection switch) so later
        # tests don't hit backends with stale extension caches.
        import psycopg.conninfo
        dbname = psycopg.conninfo.conninfo_to_dict(test_dsn).get("dbname")
        page.request.post(f"{studio_url}/api/conn",
                          data=json.dumps({"database": dbname}),
                          headers={"Content-Type": "application/json"})


def test_per_cell_scheme_cycles_and_persists(page: Page, studio_url: str) -> None:
    """The cell-actions scheme button cycles default -> semiring ->
    absorptive -> where -> boolean; the chip reflects it and the override
    round-trips through the .ipynb metadata."""
    _goto_notebook(page, studio_url)
    cell = page.locator(".nb-cell--sql").first
    _sql_cell_ta(page).fill("SELECT 1;")
    cell.hover()
    btn = cell.locator("[data-act='scheme']")
    expect(cell.locator(".nb-cell__scheme")).to_have_count(0)
    btn.click()
    expect(cell.locator(".nb-cell__scheme")).to_have_text("semiring")
    btn.click()
    expect(cell.locator(".nb-cell__scheme")).to_have_text("absorptive")
    btn.click()
    expect(cell.locator(".nb-cell__scheme")).to_have_text("where")
    btn.click()
    expect(cell.locator(".nb-cell__scheme")).to_have_text("boolean")

    dl = _download_notebook(page)
    doc = json.loads(open(dl.path()).read())
    code = [c for c in doc["cells"] if c["cell_type"] == "code"]
    assert code[0]["metadata"]["provsql"]["scheme"] == "boolean"

    btn.click()  # back to default
    expect(cell.locator(".nb-cell__scheme")).to_have_count(0)
