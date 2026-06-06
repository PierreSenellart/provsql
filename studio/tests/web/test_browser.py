"""End-to-end tests for the browser (PGlite + Pyodide) build of Studio.

These run the real, unmodified Studio frontend and Python backend in a headless
Chromium (JSPI is on by default in current Chromium), against an in-page PGlite
cluster -- no PostgreSQL. They exercise the boot, the core query/circuit path,
the in-browser API surface, database switching, and the Reset button.
"""
from __future__ import annotations

import re
from urllib.parse import quote, unquote_plus

from playwright.sync_api import Frame, Page, expect


def ui(page: Page) -> Frame:
    """The Studio UI runs inside the shell's iframe (#studio-ui); return its
    Frame. Waits for #share-link-btn, the last element child-boot.js adds before
    it signals ready -- so the /api/* -> shell fetch bridge is installed. This
    makes the helper safe to call right after a mode / database switch reloads
    the iframe (when the shell's boot bar is not a barrier)."""
    page.frame_locator("#studio-ui").locator("#share-link-btn").wait_for(
        state="attached", timeout=60000)
    fr = page.frame(name="studio-ui")
    assert fr is not None, "studio-ui iframe not found"
    return fr


def _api(page: Page, method: str, path: str, body=None):
    """Call an /api/* endpoint through the UI frame's own fetch (which the child
    boot script bridges to the backend in the shell) and return parsed JSON."""
    return ui(page).evaluate(
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
        page.goto(web_server + "/app.html", wait_until="domcontentloaded")
        page.wait_for_selector("#studio-boot-status", state="hidden", timeout=240000)
        assert ui(page).locator("#request").count() == 1   # the UI booted
        assert not blocked, f"unexpected off-origin requests: {blocked[:8]}"
    finally:
        ctx.close()


def test_landing_gates_on_jspi(browser, web_server) -> None:
    """A bare visit hits the landing (not the app): it explains the JSPI
    requirement and links to app.html."""
    ctx = browser.new_context()
    page = ctx.new_page()
    try:
        page.goto(web_server + "/", wait_until="domcontentloaded")
        assert page.locator("#studio-boot-status").count() == 0   # not the app
        assert page.locator("#launch").get_attribute("href") == "app.html"
        body = page.locator("body").inner_text()
        assert "JSPI" in body and "Firefox" in body
        assert not page.locator("#jspi-warn").is_visible()        # headless Chromium has JSPI
    finally:
        ctx.close()


def test_portable_under_subpath(browser, subpath_server) -> None:
    """The build works unchanged under a sub-path (provsql.org/playground/):
    boots with the root served as 404, loads circuit.js via its relative path,
    and the mode anchors resolve under the sub-path."""
    ctx = browser.new_context()
    page = ctx.new_page()
    responses: dict[str, int] = {}
    page.on("response", lambda r: responses.__setitem__(r.url, r.status))
    try:
        page.goto(subpath_server + "/app.html", wait_until="domcontentloaded")
        page.wait_for_selector("#studio-boot-status", state="hidden", timeout=240000)
        # circuit.js was loaded from /playground/static/ (the relativised path),
        # by app.js running inside the /playground/ui.html iframe.
        circ = [u for u, s in responses.items()
                if u.endswith("/static/circuit.js") and s == 200]
        assert circ and "/playground/static/circuit.js" in circ[0], \
            f"circuit.js not loaded under the sub-path: {circ}"
        # The mode anchor is relative and resolves under /playground/ (against
        # the iframe's ui.html base).
        where = ui(page).locator("#modeswitch [data-mode='where']")
        assert where.get_attribute("href") == "?mode=where"
        assert "/playground/ui.html?mode=where" in where.evaluate("e => e.href")
    finally:
        ctx.close()


def test_jspi_available(cs7_page: Page) -> None:
    """The whole approach relies on JSPI (Pyodide run_sync over async PGlite)."""
    assert cs7_page.evaluate("typeof WebAssembly.Suspending") == "function"


def test_core_query_and_circuit(cs7_page: Page) -> None:
    """Run a grouped provenance query, see rows, click a provenance token, and
    watch the circuit DAG render -- then evaluate a semiring on it."""
    f = ui(cs7_page)
    expect(f.locator("body")).to_have_class("mode-circuit", timeout=5000)
    f.locator("#request").fill(
        "SELECT paper, count(*) FROM bid GROUP BY paper ORDER BY paper LIMIT 3;")
    f.locator("#run-btn").click()
    expect(f.locator("#result-count")).to_have_text("3", timeout=20000)

    # The provsql token is the last cell; clicking it loads the DAG.
    f.locator("#result-body tr").first.locator("td").last.click()
    expect(f.locator("#sidebar-body svg").first).to_be_visible(timeout=20000)

    # Eval strip: a polymorphic semiring needs no mapping.
    expect(f.locator("#eval-strip")).to_be_visible(timeout=8000)
    f.locator("#eval-semiring").select_option(value="boolexpr")
    f.locator("#eval-run").click()
    expect(f.locator("#eval-result")).not_to_be_empty(timeout=20000)


def test_agg_token_cells_carry_uuid_after_db_switch(cs7_page: Page) -> None:
    """agg_token result cells must expose their circuit UUID (the
    click-through handle) even though opening a database reopens the one
    PGlite session: db.py's per-connection configure SETs
    provsql.aggtoken_text_as_uuid on Python connection objects that
    outlive the session, so the shell's per-open PREP (and the shim's
    close()) must re-apply it -- without that, agg cells render as
    "value (*)" and are not clickable."""
    f = ui(cs7_page)
    f.locator("#request").fill(
        "SELECT paper, count(*) FROM bid GROUP BY paper ORDER BY paper LIMIT 1;")
    f.locator("#run-btn").click()
    expect(f.locator("#result-count")).to_have_text("1", timeout=20000)
    cell = f.locator("#result-body tr").first \
        .locator("td[data-token-kind='agg_token']")
    uuid = cell.get_attribute("data-circuit-uuid") or ""
    assert re.fullmatch(r"[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}", uuid), uuid


def test_kernel_ddl_lands_in_public_schema(cs7_page: Page) -> None:
    """An unqualified CREATE TABLE in a notebook cell must create the
    table in `public`, where the Schema panel sees it. The app-level
    search_path override reaches every cell as SET LOCAL verbatim, so
    with `provsql` listed first the table would silently land in the
    provsql schema instead (the Schema panel excludes provsql, so a
    seeded notebook's DROP+CREATE setup would leave the database
    looking empty)."""
    info = _api(cs7_page, "GET", "/api/conn")["json"]
    assert info["search_path"].split(",")[0].strip() == "public", info

    ses = _api(cs7_page, "POST", "/api/nb/session", {})["json"]
    sid = ses["session_id"]
    try:
        out = _api(cs7_page, "POST", "/api/nb/exec", {
            "session_id": sid,
            "sql": "CREATE TABLE web_e2e_nbddl (i int)"})
        assert out["status"] == 200 and not out["json"].get("kernel_dead"), out
        schema = _api(cs7_page, "GET", "/api/schema")["json"]
        match = [t for t in schema if t["table"] == "web_e2e_nbddl"]
        assert match and match[0]["schema"] == "public", \
            [f"{t['schema']}.{t['table']}" for t in schema]
    finally:
        _api(cs7_page, "POST", "/api/nb/exec", {
            "session_id": sid,
            "sql": "DROP TABLE IF EXISTS web_e2e_nbddl"})
        _api(cs7_page, "POST", f"/api/nb/session/{sid}/close", {})


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
    f = ui(cs7_page)
    f.locator("#request").fill(
        "SELECT reviewer, paper FROM bid ORDER BY reviewer, paper LIMIT 1;")
    f.locator("#run-btn").click()
    expect(f.locator("#result-count")).to_have_text("1", timeout=20000)
    token = (f.locator("#result-body tr").first
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
    f = ui(cs7_page)
    f.locator("#request").fill("SELECT reviewer FROM bid LIMIT 1;")
    f.locator("#share-link-btn").click()
    expect(f.locator("#share-link-btn")).to_contain_text("Copied", timeout=5000)
    link = f.evaluate("navigator.clipboard.readText()")
    assert "mode=circuit" in link and "db=cs7" in link
    # The link targets the shell (app.html), not the inner UI document.
    assert "app.html" in link
    assert "bid" in unquote_plus(link)  # the query round-trips


def test_deep_link_applies_db_mode_and_query(open_studio) -> None:
    """Opening a ?mode=&db=&q= link lands on that database and mode with the
    query pre-filled and auto-run."""
    sql = "SELECT name, classification FROM personnel ORDER BY id LIMIT 2"
    page = open_studio(path="/?mode=circuit&db=cs1&q=" + quote(sql))
    f = ui(page)
    expect(f.locator("body")).to_have_class("mode-circuit", timeout=5000)
    assert _api(page, "GET", "/api/conn")["json"]["database"] == "cs1"
    expect(f.locator("#request")).to_have_value(sql, timeout=10000)
    # auto-ran: the result table shows the two rows.
    expect(f.locator("#result-count")).to_have_text("2", timeout=20000)


def test_mode_switch_keeps_backend_warm(open_studio) -> None:
    """The point of the shell + iframe split: a mode switch reloads only the
    iframe, not the WASM backend. Mark the shell's window, switch circuit ->
    where, and the marker must survive (a full-page reload would clear it),
    proving Pyodide + PGlite were not re-initialised."""
    page = open_studio()  # tutorial, circuit mode
    page.evaluate("window.__shell_warm = 'kept'")
    f = ui(page)
    expect(f.locator("body")).to_have_class("mode-circuit", timeout=5000)

    # The Where tab is a ?mode=where anchor; clicking it reloads the iframe.
    with page.expect_event("framenavigated", lambda fr: fr.name == "studio-ui", timeout=60000):
        f.locator("#modeswitch [data-mode='where']").click()
    expect(ui(page).locator("body")).to_have_class("mode-where", timeout=10000)

    # The shell never navigated, so its marker -- and the warm backend it holds
    # -- are still here, and the bridged backend answers without a re-boot.
    assert page.evaluate("window.__shell_warm") == "kept"
    assert _api(page, "GET", "/api/conn")["json"]["database"] == "tutorial"


def test_database_switch(open_studio) -> None:
    """Switch from the default tutorial DB to cs1 via the switcher and confirm
    the active database actually changed (survives the reload the UI does)."""
    page = open_studio()  # default: tutorial
    assert _api(page, "GET", "/api/conn")["json"]["database"] == "tutorial"

    f = ui(page)
    f.locator("#conn-info").click()
    f.locator("#dbmenu li[data-db='cs1']").wait_for(timeout=10000)
    # The switcher POSTs /api/conn (the shell reopens PGlite on cs1 in place,
    # keeping Pyodide warm) then reloads only the iframe. Wait for that iframe
    # navigation, then re-resolve the frame.
    with page.expect_event("framenavigated", lambda fr: fr.name == "studio-ui", timeout=60000):
        f.locator("#dbmenu li[data-db='cs1']").click()

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
    # Reset asks the shell to drop every database and re-seed tutorial, then the
    # shell reloads the iframe. Wait for that iframe navigation.
    with page.expect_event("framenavigated", lambda fr: fr.name == "studio-ui", timeout=120000):
        ui(page).locator("#reset-data-btn").click()

    assert _api(page, "GET", "/api/conn")["json"]["database"] == "tutorial"
    after = _api(page, "POST", "/api/exec",
                 {"sql": "SELECT name FROM person WHERE name = 'Intruder'",
                  "mode": "circuit", "prov_scheme": "semiring"})
    assert after["status"] == 200
    assert "Intruder" not in (after.get("text") or str(after.get("json")))
