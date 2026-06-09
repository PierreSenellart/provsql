"""End-to-end smoke tests: one scenario per Studio mode.

Each test drives the live UI for a few seconds against a fresh PG database
prepared by `studio/tests/conftest.py`. They prove the URLs route correctly,
the JS bootstraps without raising, and the request → result → render path
works for each mode."""
from __future__ import annotations

import re

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


def test_contributions_mode_renders_bars(
    page: Page, studio_url: str, test_dsn: str
) -> None:
    """Contributions mode: navigate to /contributions, give the inputs
    probabilities, run a DISTINCT query whose single row is a ⊕ over the
    three Paris inputs, click its provsql cell, and see three contribution
    bars in the sidebar. Then flip the measure to Banzhaf and confirm the
    chart re-renders."""
    page.goto(studio_url + "/contributions")
    expect(page.locator("body")).to_have_class("mode-contributions", timeout=5000)
    try:
        _run_query_and_wait(
            page,
            "DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM personnel; END $$;"
            " SELECT DISTINCT city FROM personnel WHERE city = 'Paris';",
            1,
        )
        # The provsql UUID is the last <td>; clicking it pins the target.
        page.locator("#result-body tr").first.locator("td").last.click()
        bars = page.locator("#contrib-chart .cv-contrib__bar")
        expect(bars).to_have_count(3, timeout=8000)
        # With the default "source row" labels, each bar resolves (lazily, via
        # /api/leaf) to the full personnel row, not a truncated subset: the
        # label carries the relation and every column value.
        first_label = page.locator("#contrib-chart .cv-contrib__label").first
        expect(first_label).to_contain_text("personnel", timeout=8000)
        expect(first_label).to_contain_text("Paris")
        # The tooltip names every column so the row is readable on hover.
        expect(first_label).to_have_attribute("title", re.compile(r"city: Paris"))
        # Switching the measure re-fetches; the chart still has three bars.
        page.locator("#contrib-measure").select_option("banzhaf")
        expect(bars).to_have_count(3, timeout=8000)
    finally:
        # The set_prob above mutates the session-shared DB; restore the
        # implicit default (1.0) so later tests see untouched probabilities.
        import psycopg
        with psycopg.connect(
            f"{test_dsn} options='-c search_path=provsql_test,provsql,public'"
        ) as conn, conn.cursor() as cur:
            cur.execute("DO $$ BEGIN PERFORM set_prob(provsql, 1.0) FROM personnel; END $$;")


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


def test_load_sql_file_fills_query_box(
        page: Page, studio_url: str, tmp_path) -> None:
    """The folder icon under the eraser loads a local .sql file into the
    query box, replacing its content; the loaded query then runs."""
    sql_file = tmp_path / "loaded.sql"
    sql_file.write_text("SELECT name FROM personnel LIMIT 2;\n")

    page.goto(studio_url + "/where")
    page.locator("#request").fill("SELECT 'draft to be replaced';")
    # The visible button proxies a click to the hidden file input;
    # set_input_files drives the input directly, which fires the same
    # `change` event as a user pick.
    expect(page.locator("#load-sql-btn")).to_be_visible()
    page.locator("#load-sql-input").set_input_files(str(sql_file))
    expect(page.locator("#request")).to_have_value(
        "SELECT name FROM personnel LIMIT 2;\n"
    )
    page.locator("#run-btn").click()
    expect(page.locator("#result-count")).to_have_text("2", timeout=8000)


def test_invisible_unicode_is_sanitized(page: Page, studio_url: str,
                                        tmp_path) -> None:
    """Invisible Unicode smuggled in by rich-text copy (NBSP, zero-width
    space, BOM, object replacement character) is cleaned before the query
    runs -- on the paste hook, on the run path, and on a loaded file --
    instead of dying on PostgreSQL's `syntax error at or near "￼"`."""
    page.goto(studio_url + "/where")

    # Paste hook: dirty text arriving as insertFromPaste is rewritten at once.
    cleaned = page.evaluate(
        """() => {
            const ta = document.getElementById('request');
            ta.value = 'SELECT\\u00a0\\u00a042 AS n;\\ufffc';
            ta.setSelectionRange(ta.value.length, ta.value.length);
            ta.dispatchEvent(new InputEvent('input',
                {inputType: 'insertFromPaste', bubbles: true}));
            return ta.value;
        }""")
    assert cleaned == "SELECT  42 AS n;"

    # Run path (covers ?q= deep links and programmatic fills): cleaned in
    # place before execution, and the query succeeds.
    page.locator("#request").fill(
        "SELECT\u00a0name\u00a0FROM\u00a0personnel\u00a0LIMIT\u00a01;\u200b")
    page.locator("#run-btn").click()
    expect(page.locator("#result-count")).to_have_text("1", timeout=8000)
    expect(page.locator("#request")).to_have_value(
        "SELECT name FROM personnel LIMIT 1;")

    # Loaded file: a BOM-prefixed file is cleaned on its way in.
    sql_file = tmp_path / "bom.sql"
    sql_file.write_text("\ufeffSELECT 1;\n")
    page.locator("#load-sql-input").set_input_files(str(sql_file))
    expect(page.locator("#request")).to_have_value("SELECT 1;\n")


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
    # is exercised end-to-end. Modes live in a dropdown now, so open it first.
    page.locator("#modeswitch-btn").click()
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


def test_circuit_eval_strip_karp_luby_options_and_bound(
    page: Page, studio_url: str
) -> None:
    """Circuit mode, probability semiring, karp-luby: the dedicated
    approximate-options control appears (defaulting to the (eps, delta) mode
    for karp-luby), its samples/(eps,delta) toggle swaps the fields, and a run
    reports a result plus the guarantee bound, rendered as a value interval
    with the confidence and sample count.  A single-row personnel provenance is
    a trivial DNF, so karp-luby applies."""
    page.goto(studio_url + "/circuit")
    _run_query_and_wait(page, "SELECT name FROM personnel LIMIT 3;", 3)
    page.locator("#result-body tr").first.locator("td").last.click()
    expect(page.locator("#eval-strip")).to_be_visible(timeout=8000)

    page.locator("#eval-semiring").select_option(value="probability")
    page.locator("#eval-method").select_option(value="karp-luby")

    # karp-luby defaults to the (eps, delta) mode: epsilon shows, samples hidden.
    expect(page.locator("#eval-approx-eps")).to_be_visible(timeout=8000)
    expect(page.locator("#eval-args-mc")).to_be_hidden()
    # The mode toggle swaps to a fixed sample count and back.
    page.locator("#eval-approx-mode").select_option(value="samples")
    expect(page.locator("#eval-args-mc")).to_be_visible()
    expect(page.locator("#eval-approx-eps")).to_be_hidden()
    page.locator("#eval-approx-mode").select_option(value="epsdelta")
    expect(page.locator("#eval-approx-eps")).to_be_visible()

    page.locator("#eval-run").click()
    expect(page.locator("#eval-result")).to_have_attribute(
        "data-kind", "ok", timeout=15000
    )
    # The bound slot renders the guarantee as a value interval (Pr ∈ [lo, hi])
    # with the confidence and the sample count.
    bound = page.locator("#eval-bound")
    expect(bound).to_contain_text("Pr ∈", timeout=8000)
    expect(bound).to_contain_text("samples")


def test_circuit_eval_strip_dtree_epsilon_control(
    page: Page, studio_url: str
) -> None:
    """Circuit mode, probability semiring, d-tree: the dedicated optional
    epsilon control appears (and is the only arg control shown), and a run with
    a set epsilon succeeds.  A single-row personnel provenance is a trivial
    one-variable DNF, so the anytime interval collapses to the exact point
    regardless of epsilon -- this test just exercises the control wiring (the
    epsilon reaches buildProbArgs and the backend accepts it)."""
    page.goto(studio_url + "/circuit")
    _run_query_and_wait(page, "SELECT name FROM personnel LIMIT 3;", 3)
    page.locator("#result-body tr").first.locator("td").last.click()
    expect(page.locator("#eval-strip")).to_be_visible(timeout=8000)

    page.locator("#eval-semiring").select_option(value="probability")
    page.locator("#eval-method").select_option(value="d-tree")

    # The optional epsilon control is the only arg control shown for d-tree;
    # the approximate-options group and the compiler/wmc pickers stay hidden.
    eps = page.locator("#eval-args-dtree-eps")
    expect(eps).to_be_visible(timeout=8000)
    expect(page.locator("#eval-args-approx")).to_be_hidden()
    expect(page.locator("#eval-args-compiler")).to_be_hidden()

    eps.fill("0.2")
    page.locator("#eval-run").click()
    expect(page.locator("#eval-result")).to_have_attribute(
        "data-kind", "ok", timeout=15000
    )


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


def test_circuit_prov_scheme_selector_is_interactive(
    page: Page, studio_url: str
) -> None:
    """In Circuit mode, the per-query provenance-flavour selector is free
    (not locked, unlike Where mode). Switching to the `where` flavour
    must not break the next query.  The radio inputs are visually
    hidden (clip-rect trick) so the segmented control can be painted
    on the wrapping label ; click the label, not the input."""
    page.goto(studio_url + "/circuit")
    expect(page.locator("body")).to_have_class("mode-circuit", timeout=5000)

    fs = page.locator("#prov-scheme-fieldset")
    expect(fs).not_to_have_class("is-locked")
    where_radio = page.locator('input[name="prov-scheme"][value="where"]')
    page.locator('label[data-scheme="where"]').click()
    expect(where_radio).to_be_checked()

    _run_query_and_wait(page, "SELECT name FROM personnel LIMIT 3;", 3)


def test_circuit_prov_scheme_defaults_to_semiring(
    page: Page, studio_url: str
) -> None:
    """The three-way selector ships with Semiring as the default pick
    on Circuit mode : it matches the pre-rewriter behaviour, so
    existing users see no functional change until they opt in."""
    page.goto(studio_url + "/circuit")
    expect(page.locator("body")).to_have_class("mode-circuit", timeout=5000)
    semiring_radio = page.locator('input[name="prov-scheme"][value="semiring"]')
    expect(semiring_radio).to_be_checked()


def test_where_mode_locks_prov_scheme_to_where(
    page: Page, studio_url: str
) -> None:
    """The Where UI mode requires where-provenance (the cell-highlight
    wrap depends on it).  The selector must be marked locked, the
    Where segment must be the active pick, and the radio inputs must
    be disabled so the user can't override the lock from the keyboard.
    """
    page.goto(studio_url + "/where")
    expect(page.locator("body")).to_have_class("mode-where", timeout=5000)

    fs = page.locator("#prov-scheme-fieldset")
    expect(fs).to_have_class("wp-prov-scheme is-locked")
    where_radio = page.locator('input[name="prov-scheme"][value="where"]')
    expect(where_radio).to_be_checked()
    expect(where_radio).to_be_disabled()
    boolean_radio = page.locator('input[name="prov-scheme"][value="boolean"]')
    expect(boolean_radio).to_be_disabled()
