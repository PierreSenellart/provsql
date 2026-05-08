"""End-to-end smoke tests: one scenario per Studio mode.

Each test drives the live UI for a few seconds against a fresh PG database
prepared by `studio/tests/conftest.py`. They prove the URLs route correctly,
the JS bootstraps without raising, and the request → result → render path
works for each mode."""
from __future__ import annotations

from playwright.sync_api import Page, expect


def _run_query_and_wait(page: Page, sql: str, expected_rows: int) -> None:
    """Type into the SQL textarea, click Send query, wait until the result
    count chip shows the expected row count -- this is the most precise
    proxy for 'the request succeeded and was rendered' (the body's
    Running... placeholder is itself a <tr>, so a generic row check would
    pass on the loading state too)."""
    page.locator("#request").fill(sql)
    page.locator("#run-btn").click()
    expect(page.locator("#result-count")).to_have_text(
        str(expected_rows), timeout=8000
    )


def test_where_mode_runs_a_query(page: Page, studio_url: str) -> None:
    """Where mode: navigate to /where, run a SELECT against the personnel
    fixture, see exactly 3 rows in the result table."""
    page.goto(studio_url + "/where")
    expect(page.locator("body")).to_have_class("mode-where", timeout=5000)
    _run_query_and_wait(
        page, "SELECT name, classification FROM personnel LIMIT 3;", 3
    )


def test_circuit_mode_renders_a_dag(page: Page, studio_url: str) -> None:
    """Circuit mode: navigate to /circuit, run a SELECT against the personnel
    fixture, click the first UUID cell, see the DAG canvas populate."""
    page.goto(studio_url + "/circuit")
    expect(page.locator("body")).to_have_class("mode-circuit", timeout=5000)
    _run_query_and_wait(page, "SELECT name FROM personnel LIMIT 3;", 3)
    # The provsql UUID is the last <td> in each row. Click it to load the DAG.
    page.locator("#result-body tr").first.locator("td").last.click()
    # circuit.js renders the DAG into the sidebar as an SVG.
    expect(page.locator("#sidebar-body svg").first).to_be_visible(timeout=8000)


def test_query_history_records_runs(page: Page, studio_url: str) -> None:
    """History: run two distinct queries, open the History menu, see both
    entries listed."""
    page.goto(studio_url + "/where")
    _run_query_and_wait(page, "SELECT 1 AS first_marker;", 1)
    _run_query_and_wait(page, "SELECT 2 AS second_marker;", 1)

    page.locator("#history-btn").click()
    menu = page.locator("#history-menu")
    expect(menu).to_be_visible()
    expect(menu).to_contain_text("second_marker")
    expect(menu).to_contain_text("first_marker")


def test_connection_editor_opens_and_validates(
    page: Page, studio_url: str
) -> None:
    """DSN editor: click the plug icon, paste a deliberately bad DSN, click
    Connect, see the inline error -- and confirm the page never reloaded
    (i.e. the existing connection stayed up)."""
    page.goto(studio_url + "/where")
    page.locator("#conn-dot").click()

    panel = page.locator("#dsn-panel")
    expect(panel).to_be_visible()

    page.locator("#dsn-input").fill(
        "host=127.0.0.1 port=1 user=nope dbname=nope"
    )
    page.locator("#dsn-apply").click()

    status = page.locator("#dsn-status")
    expect(status).to_be_visible(timeout=8000)
    # Page must not have navigated away on failure.
    assert page.url.startswith(studio_url + "/where")


def test_mode_switch_carries_query_forward(
    page: Page, studio_url: str
) -> None:
    """Mode switcher stashes the current SQL in sessionStorage before
    navigating, so switching from Where to Circuit lands the same query in
    the textarea on the other page."""
    page.goto(studio_url + "/where")
    page.locator("#request").fill("SELECT 42 AS carry_me;")
    # The mode-switcher anchor: clicking it triggers a normal navigation
    # plus the JS-side stash. Use a click rather than a goto so the JS path
    # is exercised end-to-end.
    page.locator("#modeswitch [data-mode='circuit']").click()
    expect(page.locator("body")).to_have_class("mode-circuit", timeout=5000)
    expect(page.locator("#request")).to_have_value(
        "SELECT 42 AS carry_me;", timeout=5000
    )


def test_circuit_eval_strip_runs_boolexpr(
    page: Page, studio_url: str
) -> None:
    """In Circuit mode, render a circuit, then run the eval strip's
    `boolexpr` semiring against the root token. boolexpr is polymorphic
    (no mapping required), so the result should land regardless of the
    test fixture's mapping shape."""
    page.goto(studio_url + "/circuit")
    _run_query_and_wait(page, "SELECT name FROM personnel LIMIT 3;", 3)
    page.locator("#result-body tr").first.locator("td").last.click()
    # Eval strip materialises once a circuit is loaded.
    expect(page.locator("#eval-strip")).to_be_visible(timeout=8000)
    page.locator("#eval-semiring").select_option(value="boolexpr")
    page.locator("#eval-run").click()
    # Result span is non-empty once the request returns.
    expect(page.locator("#eval-result")).not_to_be_empty(timeout=8000)


def test_schema_panel_column_click_prefills_query(
    page: Page, studio_url: str
) -> None:
    """Open the schema panel, click a `personnel` column, see the query
    box pre-filled with the corresponding `create_provenance_mapping` SQL."""
    page.goto(studio_url + "/where")
    page.locator("#schema-btn").click()
    expect(page.locator("#schema-panel")).to_be_visible()
    # `personnel` is provenance-tracked in the fixture, so its columns are
    # rendered as `[data-action='create-mapping']` spans.
    name_col = page.locator(
        "#schema-body [data-action='create-mapping']"
        "[data-table='personnel'][data-col='name']"
    )
    expect(name_col).to_be_visible(timeout=5000)
    name_col.click()
    expect(page.locator("#request")).to_have_value(
        "SELECT create_provenance_mapping('personnel_name_mapping', "
        "'provsql_test.personnel', 'name');",
        timeout=5000,
    )


def test_circuit_where_provenance_toggle_is_interactive(
    page: Page, studio_url: str
) -> None:
    """In Circuit mode, the per-query `where_provenance` toggle is free
    (not locked, unlike Where mode). Toggling it on must not break the
    next query."""
    page.goto(studio_url + "/circuit")
    expect(page.locator("body")).to_have_class("mode-circuit", timeout=5000)

    toggle = page.locator("#opt-where-prov")
    expect(toggle).not_to_be_checked()
    toggle.check()
    expect(toggle).to_be_checked()

    _run_query_and_wait(page, "SELECT name FROM personnel LIMIT 3;", 3)
