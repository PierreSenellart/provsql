"""End-to-end tests for the browser (PGlite + Pyodide) build of Studio.

These run the real, unmodified Studio frontend and Python backend in a headless
Chromium (JSPI is on by default in current Chromium), against an in-page PGlite
cluster -- no PostgreSQL. They exercise the boot, the core query/circuit path,
the in-browser API surface, database switching, and the Reset button.
"""
from __future__ import annotations

from urllib.parse import quote, unquote_plus

from playwright.sync_api import Page, expect


def _api(page: Page, method: str, path: str, body=None):
    """Call an /api/* endpoint through the page's own fetch (which the boot
    script routes into the in-page Flask backend) and return parsed JSON."""
    return page.evaluate(
        """async ({method, path, body}) => {
            const r = await fetch(path, body ? {method, headers:{'Content-Type':'application/json'}, body: JSON.stringify(body)} : {method});
            const text = await r.text();
            try { return {status: r.status, json: JSON.parse(text)}; }
            catch { return {status: r.status, text}; }
        }""",
        {"method": method, "path": path, "body": body},
    )


def test_fully_self_hosted(browser, web_server) -> None:
    """Boot with every off-origin request blocked: the build must load nothing
    from a CDN (Pyodide, wheels, Graphviz, Font Awesome are all vendored)."""
    ctx = browser.new_context()
    blocked: list[str] = []

    def handle(route):
        url = route.request.url
        if url.startswith("http") and not url.startswith(web_server):
            blocked.append(url)
            route.abort()
        else:
            route.continue_()

    ctx.route("**/*", handle)
    page = ctx.new_page()
    try:
        page.goto(web_server + "/", wait_until="domcontentloaded")
        page.wait_for_selector("#studio-boot-status", state="hidden", timeout=240000)
        assert page.locator("#request").count() == 1   # the UI booted
        assert not blocked, f"unexpected off-origin requests: {blocked[:8]}"
    finally:
        ctx.close()


def test_jspi_available(cs7_page: Page) -> None:
    """The whole approach relies on JSPI (Pyodide run_sync over async PGlite)."""
    assert cs7_page.evaluate("typeof WebAssembly.Suspending") == "function"


def test_core_query_and_circuit(cs7_page: Page) -> None:
    """Run a grouped provenance query, see rows, click a provenance token, and
    watch the circuit DAG render -- then evaluate a semiring on it."""
    expect(cs7_page.locator("body")).to_have_class("mode-circuit", timeout=5000)
    cs7_page.locator("#request").fill(
        "SELECT paper, count(*) FROM bid GROUP BY paper ORDER BY paper LIMIT 3;")
    cs7_page.locator("#run-btn").click()
    expect(cs7_page.locator("#result-count")).to_have_text("3", timeout=20000)

    # The provsql token is the last cell; clicking it loads the DAG.
    cs7_page.locator("#result-body tr").first.locator("td").last.click()
    expect(cs7_page.locator("#sidebar-body svg").first).to_be_visible(timeout=20000)

    # Eval strip: a polymorphic semiring needs no mapping.
    expect(cs7_page.locator("#eval-strip")).to_be_visible(timeout=8000)
    cs7_page.locator("#eval-semiring").select_option(value="boolexpr")
    cs7_page.locator("#eval-run").click()
    expect(cs7_page.locator("#eval-result")).not_to_be_empty(timeout=20000)


def test_database_list_is_the_case_studies(cs7_page: Page) -> None:
    out = _api(cs7_page, "GET", "/api/databases")
    assert out["status"] == 200
    assert out["json"] == ["tutorial", "cs1", "cs2", "cs4", "cs5", "cs6", "cs7"]


def test_where_provenance_traces_cells(cs7_page: Page) -> None:
    """Where mode wraps the query with where_provenance(); the response must
    carry source-cell locators back to the bid relation."""
    out = _api(cs7_page, "POST", "/api/exec", {"sql": "SELECT * FROM bid", "mode": "where"})
    assert out["status"] == 200
    blob = out.get("text") or str(out.get("json"))
    assert "bid:" in blob or "wprov" in blob


def test_circuit_and_tree_decomposition_endpoints(cs7_page: Page) -> None:
    """/api/circuit returns a laid-out DAG and /api/kc/td a tree decomposition
    (the latter exercises the WASM-Graphviz path for the SVG/scene). A plain
    tracked SELECT gives an input-gate token (a bare UUID the KC endpoints
    accept directly)."""
    cs7_page.locator("#request").fill(
        "SELECT reviewer, paper FROM bid ORDER BY reviewer, paper LIMIT 1;")
    cs7_page.locator("#run-btn").click()
    expect(cs7_page.locator("#result-count")).to_have_text("1", timeout=20000)
    token = (cs7_page.locator("#result-body tr").first
             .locator("td[data-circuit-uuid]").first.get_attribute("data-circuit-uuid"))
    assert token, "no provenance token in the result row"

    enc = quote(token, safe="")
    circ = _api(cs7_page, "GET", f"/api/circuit/{enc}")
    assert circ["status"] == 200 and len(circ["json"]["nodes"]) > 0

    td = _api(cs7_page, "GET", f"/api/kc/td?token={enc}")
    assert td["status"] == 200
    assert "scene" in td["json"] and "treewidth" in td["json"]


def test_copy_link_captures_db_mode_query(cs7_page: Page) -> None:
    """The Copy-link button puts a ?mode=&db=&q= URL on the clipboard."""
    cs7_page.locator("#request").fill("SELECT reviewer FROM bid LIMIT 1;")
    cs7_page.locator("#share-link-btn").click()
    expect(cs7_page.locator("#share-link-btn")).to_contain_text("Copied", timeout=5000)
    link = cs7_page.evaluate("navigator.clipboard.readText()")
    assert "mode=circuit" in link and "db=cs7" in link
    assert "bid" in unquote_plus(link)  # the query round-trips


def test_deep_link_applies_db_mode_and_query(open_studio) -> None:
    """Opening a ?mode=&db=&q= link lands on that database and mode with the
    query pre-filled and auto-run."""
    sql = "SELECT name, classification FROM personnel ORDER BY id LIMIT 2"
    page = open_studio(path="/?mode=circuit&db=cs1&q=" + quote(sql))
    expect(page.locator("body")).to_have_class("mode-circuit", timeout=5000)
    assert _api(page, "GET", "/api/conn")["json"]["database"] == "cs1"
    expect(page.locator("#request")).to_have_value(sql, timeout=10000)
    # auto-ran: the result table shows the two rows.
    expect(page.locator("#result-count")).to_have_text("2", timeout=20000)


def test_database_switch(open_studio) -> None:
    """Switch from the default tutorial DB to cs1 via the switcher and confirm
    the active database actually changed (survives the reload the UI does)."""
    page = open_studio()  # default: tutorial
    assert _api(page, "GET", "/api/conn")["json"]["database"] == "tutorial"

    page.locator("#conn-info").click()
    page.locator("#dbmenu li[data-db='cs1']").wait_for(timeout=10000)
    # The switcher POSTs then reloads; wait for that navigation, then for the
    # fresh boot to finish before talking to the page again.
    with page.expect_navigation(wait_until="domcontentloaded", timeout=60000):
        page.locator("#dbmenu li[data-db='cs1']").click()
    page.wait_for_selector("#studio-boot-status", state="hidden", timeout=240000)

    assert _api(page, "GET", "/api/conn")["json"]["database"] == "cs1"
    # cs1's signature table is present.
    chk = _api(page, "POST", "/api/exec",
               {"sql": "SELECT count(*) FROM personnel", "mode": "circuit", "prov_scheme": "semiring"})
    assert chk["status"] == 200


def test_reset_restores_pristine_data(open_studio) -> None:
    """Edit a seeded database, hit Reset, and confirm the change is gone."""
    page = open_studio()  # tutorial
    # Insert an intruder row, then confirm it's there.
    _api(page, "POST", "/api/exec",
         {"sql": "INSERT INTO person(id, name) VALUES (999, 'Intruder')",
          "mode": "circuit", "prov_scheme": "semiring"})
    before = _api(page, "POST", "/api/exec",
                  {"sql": "SELECT name FROM person WHERE name = 'Intruder'",
                   "mode": "circuit", "prov_scheme": "semiring"})
    assert "Intruder" in (before.get("text") or str(before.get("json")))

    page.on("dialog", lambda d: d.accept())
    # Reset drops every database and reloads; boot re-seeds tutorial.
    with page.expect_navigation(wait_until="domcontentloaded", timeout=60000):
        page.locator("#reset-data-btn").click()
    page.wait_for_selector("#studio-boot-status", state="hidden", timeout=240000)

    assert _api(page, "GET", "/api/conn")["json"]["database"] == "tutorial"
    after = _api(page, "POST", "/api/exec",
                 {"sql": "SELECT name FROM person WHERE name = 'Intruder'",
                  "mode": "circuit", "prov_scheme": "semiring"})
    assert after["status"] == 200
    assert "Intruder" not in (after.get("text") or str(after.get("json")))
