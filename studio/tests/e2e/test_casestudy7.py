"""End-to-end walkthrough of Case Study 7 (doc/source/user/casestudy7.rst)
against the live Studio UI.

Each step of the case study makes a concrete claim -- a toggle position, a
method, a probability, an error message -- and these tests drive the real UI
on the `cs7_dsn` fixture and assert each one, so the prose and the product
cannot silently drift apart. The values mirror those written into the
chapter; if you change a probability here, change it there too.

Backed by the `cs7_studio_url` session fixture (Studio on the Case Study 7
database) and pytest-playwright's `page`."""
from __future__ import annotations

import re

from playwright.sync_api import Page, expect


# --- helpers ---------------------------------------------------------------

def _set_scheme(page: Page, value: str) -> None:
    """Pick a Provenance-scheme toggle position (boolean / absorptive /
    semiring / where) before running the next query."""
    page.locator(f'.wp-prov-scheme__opt[data-scheme="{value}"]').click()


def _run(page: Page, sql: str, scheme: str = "boolean") -> None:
    _set_scheme(page, scheme)
    page.locator("#request").fill(sql)
    page.locator("#run-btn").click()
    page.wait_for_selector("#result-body tr", timeout=20000)


def _pin(page: Page, contains: str | None = None) -> None:
    """Click a result row's provsql cell to pin it for evaluation; `contains`
    selects the row holding that text (an id / name), else the first row."""
    rows = page.locator("#result-body tr")
    tr = rows.filter(has_text=contains).first if contains else rows.first
    tr.locator("td").last.click()
    page.wait_for_selector("#eval-strip", state="visible", timeout=8000)


def _marg(page: Page, method: str) -> str:
    """Run Marginal probability with `method`, return the result strip text
    (a number on success, an error message otherwise)."""
    page.locator("#eval-semiring").select_option("probability")
    page.locator("#eval-method").select_option(method)
    page.locator("#eval-run").click()
    page.wait_for_function(
        "() => {const e=document.querySelector('#eval-result');"
        "return e && (/[0-9]/.test(e.textContent) || "
        "/not |error|fail|treewidth|greater/i.test(e.textContent));}",
        timeout=25000,
    )
    return page.locator("#eval-result").inner_text()


def _num(text: str) -> float | None:
    m = re.search(r"-?[0-9]*\.[0-9]+", (text or "").replace(",", "."))
    return float(m.group()) if m else None


def _approx(text: str, expected: float, tol: float = 2e-3) -> bool:
    v = _num(text)
    return v is not None and abs(v - expected) < tol


# --- Part A: the query is safe --------------------------------------------

def test_part_a_query_safe(page: Page, cs7_studio_url: str) -> None:
    page.goto(cs7_studio_url + "/circuit")
    expect(page.locator("body")).to_have_class(re.compile(r"\bmode-circuit\b"),
                                               timeout=8000)

    # The provenance toggle is four-way (Setup section).
    values = page.eval_on_selector_all(
        'input[name="prov-scheme"]', "els => els.map(e => e.value)")
    assert set(values) == {"boolean", "absorptive", "semiring", "where"}, values

    # A1 safe by shape (hierarchical): independent ~ 0.666 on p1.
    _run(page, "SELECT p.id, p.title FROM bid b, expertise e, papers p "
               "WHERE b.reviewer=e.reviewer AND b.paper=p.id "
               "GROUP BY p.id, p.title ORDER BY p.id")
    _pin(page, "p1")
    assert _approx(_marg(page, "independent"), 0.666, 5e-3)

    # A2 safe by a key: independent succeeds under Boolean (0.4259)…
    cov_p1 = ("SELECT DISTINCT 1 FROM bid b, expertise e, topic_of t "
              "WHERE b.reviewer=e.reviewer AND e.topic=t.topic "
              "AND b.paper='p1' AND t.paper='p1'")
    _run(page, cov_p1, scheme="boolean")
    _pin(page)
    assert _approx(_marg(page, "independent"), 0.4259, 5e-3)
    # … and errors under Semiring (literal circuit is not read-once).
    _run(page, cov_p1, scheme="semiring")
    _pin(page)
    assert "not an independent" in _marg(page, "independent").lower()

    # A3 inversion-free (Olga self-join): the default chooser takes the
    # inversion-free rung (0.975314) where tree-decomposition gives up.
    _run(page, "SELECT r.id, r.name FROM bid b1, recommend a, bid b2, "
               "champion c, reviewers r WHERE b1.reviewer=a.reviewer "
               "AND b1.paper=a.paper AND b1.reviewer=b2.reviewer "
               "AND b2.reviewer=c.reviewer AND b2.paper=c.paper "
               "AND b1.reviewer=r.id GROUP BY r.id, r.name")
    _pin(page, "Olga")
    assert _approx(_marg(page, "exact"), 0.975314, 1e-4)
    assert "treewidth" in _marg(page, "tree-decomposition").lower()


def test_part_a_mobius_step(page: Page, cs7_studio_url: str) -> None:
    """A4 safe by cancellation: q9 fires the Möbius route -- a μ-rooted
    circuit (too large to render directly; render at depth 1) whose marginal
    is exactly 0.056923."""
    page.goto(cs7_studio_url + "/circuit")
    expect(page.locator("body")).to_have_class(re.compile(r"\bmode-circuit\b"),
                                               timeout=8000)
    q9 = (
        "SELECT 1 FROM lead_chair r, prescreen a1, flag_pass a3, urgent_sub t3 "
        "WHERE r.chair=a1.chair AND a3.sub=t3.sub "
        "UNION SELECT 1 FROM prescreen b1, score_pass b2, flag_pass b3, urgent_sub tb "
        "WHERE b1.chair=b2.chair AND b1.sub=b2.sub AND b3.sub=tb.sub "
        "UNION SELECT 1 FROM score_pass c2, flag_pass c3, flag_pass c3b, urgent_sub tc "
        "WHERE c2.chair=c3.chair AND c2.sub=c3.sub AND c3b.sub=tc.sub "
        "UNION SELECT 1 FROM lead_chair d, prescreen d1, prescreen d1b, "
        "score_pass d2, score_pass d2b, flag_pass d3 "
        "WHERE d.chair=d1.chair AND d1b.chair=d2.chair AND d1b.sub=d2.sub "
        "AND d2b.chair=d3.chair AND d2b.sub=d3.sub")
    _run(page, q9)
    _pin(page)

    # The μ root carries the full literal lineage, so Studio shows a
    # "Circuit too large" card; render at depth 1 to reach the μ node.
    expect(page.get_by_text("Circuit too large to render")).to_be_visible(timeout=8000)
    page.get_by_text(re.compile("Render at depth 1")).first.click()
    # Wait for the render to finish (past the transient "Loading…" placeholder)
    # -- the μ root carries the signed coefficients on its child edges.
    page.wait_for_function(
        "() => [...document.querySelectorAll('#sidebar-body svg text')]"
        ".some(t => t.textContent.includes('\\u03bc'))", timeout=15000)
    glyphs = set(page.locator("#sidebar-body svg text").all_text_contents())
    assert any("μ" in g for g in glyphs), glyphs          # the Möbius root
    assert any(g.strip() in {"+1", "-1", "−1"} for g in glyphs), glyphs  # coeffs

    # Exact marginal via the default chooser.
    assert _approx(_marg(page, "exact"), 0.056923, 5e-4)


# --- Part B: the query is hard --------------------------------------------

def test_part_b_query_hard(page: Page, cs7_studio_url: str) -> None:
    page.goto(cs7_studio_url + "/circuit")
    expect(page.locator("body")).to_have_class(re.compile(r"\bmode-circuit\b"),
                                               timeout=8000)
    # B1 hard query under Semiring: tree-decomposition ~0.8818, independent errors.
    hard = ("SELECT DISTINCT 1 FROM bid b, expertise e, topic_of t "
            "WHERE b.reviewer=e.reviewer AND e.topic=t.topic AND t.paper=b.paper")
    _run(page, hard, scheme="semiring")
    _pin(page)
    assert _approx(_marg(page, "tree-decomposition"), 0.881791, 5e-3)
    assert "not an independent" in _marg(page, "independent").lower()

    # B4 HAVING count(*) >= 2: the Poisson-binomial pre-pass lets even
    # independent answer it (a valid probability) without a compiler.
    _run(page, "SELECT p.id, p.title FROM bid b, expertise e, papers p "
               "WHERE b.reviewer=e.reviewer AND b.paper=p.id "
               "GROUP BY p.id, p.title HAVING count(*)>=2 ORDER BY p.id")
    _pin(page, "p1")
    v = _num(_marg(page, "independent"))
    assert v is not None and 0.0 < v <= 1.0


# --- Part C: the data is well-structured ----------------------------------

def test_part_c_data_structured(page: Page, cs7_studio_url: str) -> None:
    page.goto(cs7_studio_url + "/circuit")
    expect(page.locator("body")).to_have_class(re.compile(r"\bmode-circuit\b"),
                                               timeout=8000)
    # C1 the hard query, now easy under Boolean (joint-width): ~0.8818.
    _run(page, "SELECT DISTINCT 1 FROM bid b, expertise e, topic_of t "
               "WHERE b.reviewer=e.reviewer AND e.topic=t.topic AND t.paper=b.paper")
    _pin(page)
    assert _approx(_marg(page, "independent"), 0.881791, 5e-3)

    # C2 repair_key correlation: independent agrees (0.875 / 0.75).
    _run(page, "SELECT p.id, p.title FROM assignment a JOIN papers p "
               "ON a.paper=p.id GROUP BY p.id, p.title ORDER BY p.id")
    _pin(page, "p1")
    assert _approx(_marg(page, "independent"), 0.875, 2e-3)
    _pin(page, "p2")
    assert _approx(_marg(page, "independent"), 0.75, 2e-3)

    # C3 hard AND correlated (joint-width over repair_key): exact 0.735868.
    _run(page, "SELECT DISTINCT 1 FROM assignment a, expertise e, topic_of t "
               "WHERE a.reviewer=e.reviewer AND e.topic=t.topic AND t.paper=a.paper")
    _pin(page)
    assert _approx(_marg(page, "independent"), 0.735868, 2e-3)


def test_part_c_recursion(page: Page, cs7_studio_url: str) -> None:
    page.goto(cs7_studio_url + "/circuit")
    expect(page.locator("body")).to_have_class(re.compile(r"\bmode-circuit\b"),
                                               timeout=8000)
    # C4 acyclic reachability (read-once per ancestor, any semiring): p1 = 0.72.
    _run(page, "WITH RECURSIVE anc(paper) AS (SELECT 'p6' UNION "
               "SELECT e.cited FROM extends e JOIN anc a ON e.citing=a.paper) "
               "SELECT p.id, p.title FROM anc JOIN papers p ON anc.paper=p.id "
               "WHERE anc.paper<>'p6' ORDER BY p.id")
    _pin(page, "p1")
    assert _approx(_marg(page, "possible-worlds"), 0.72, 2e-3)

    # C4 cyclic reliability needs the Absorptive scheme: r1 reaches r5 at 0.5496.
    _run(page, "WITH RECURSIVE conn(node) AS (SELECT 'r1' UNION "
               "SELECT e.b FROM coreview e JOIN conn c ON e.a=c.node) "
               "SELECT r.id, r.name FROM conn JOIN reviewers r "
               "ON conn.node=r.id WHERE conn.node<>'r1' ORDER BY r.id",
         scheme="absorptive")
    _pin(page, "r5")
    assert _approx(_marg(page, "exact"), 0.5496, 2e-3)
