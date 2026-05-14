"""End-to-end test for continuous-distribution support in Circuit mode.

Loads a `sensors` fixture with two random_variable distributions plus one
RV expression built via the `+` operator (so the resulting circuit
exercises both new gate types: `gate_rv` *and* `gate_arith`).  Then drives
the live UI: run `WHERE reading > 2`, click into the resulting circuit,
verify the rv / arith nodes render with their specific labels, and run
probability monte-carlo to confirm the strip reports `p ∈ (0, 1)`.

A second group of tests exercises the analytical-curve overlay produced
by `provsql.rv_analytical_curves` and rendered by Studio's Distribution
profile panel: bare gate_rv, simplifier-folded `c * Exp(λ)`, truncated
normal (overlay's x-range collapses to the truncation bounds), and a
heterogeneous-rate `Exp + Exp` composite where no closed form applies
and the overlay must be absent.  Plus a categorical row that exercises
the discrete-stems channel."""
from __future__ import annotations

import re

import psycopg
import pytest
from playwright.sync_api import Page, expect


@pytest.fixture(scope="session", autouse=True)
def _load_sensors(test_dsn: str) -> None:
    """Add a `sensors` table to the e2e database so Circuit mode has
    something with random_variable columns to query.

    The first three rows feed the gate-rendering / probability test:
      * s1 is a bare normal -> gate_rv
      * s2 is a bare uniform -> gate_rv
      * s3 sums two uniforms (Irwin-Hall) -> gate_arith over two
        gate_rvs.  Two uniforms are deliberately chosen over a normal
        sum because the simplifier's normal-family closure would fold
        `N(0,1) + N(1,1)` to a single `gate_rv`, hiding the arith node
        from the rendered circuit -- but uniform + uniform isn't a
        standard family, so the simplifier leaves the arith intact.

    The next four rows feed the distribution-profile overlay tests:
      * s4 is `2 * Exp(0.4)`, persisted as a gate_arith composite that
        the load-time simplifier folds to a single Exp(0.2) gate_rv,
        so the analytical-curve payload appears even though the scene
        root is gate_arith.
      * s5 is a bare standard Normal, used by the truncated-overlay
        test (we WHERE-clause its tail in the query itself).
      * s6 is `Exp(0.4) + Exp(0.3)`, a heterogeneous-rate sum that
        admits no closed form -- the negative control for "overlay
        absent".
      * s7 is a 3-outcome categorical, exercising the discrete-stems
        channel (gate_mixture in N-wire form).

    Runs once per session, before studio_url boots, so the persistent
    Studio backend sees the table on first /api/exec."""
    with psycopg.connect(test_dsn, autocommit=True) as conn:
        conn.execute("SET search_path TO provsql_test, provsql, public")
        conn.execute("DROP TABLE IF EXISTS sensors CASCADE")
        conn.execute(
            "CREATE TABLE sensors(id text, reading provsql.random_variable)"
        )
        conn.execute(
            "INSERT INTO sensors VALUES "
            "('s1', provsql.normal(2.5, 0.5)), "
            "('s2', provsql.uniform(1, 3)), "
            "('s3', provsql.uniform(0, 1) + provsql.uniform(0, 2)), "
            "('s4', 2 * provsql.exponential(0.4)), "
            "('s5', provsql.normal(0, 1)), "
            "('s6', provsql.exponential(0.4) + provsql.exponential(0.3)), "
            "('s7', provsql.categorical(ARRAY[0.3, 0.5, 0.2], "
            "                           ARRAY[-1.0, 0.0, 2.0]))"
        )
        conn.execute("SELECT provsql.add_provenance('provsql_test.sensors')")


def _run_query_and_wait(page: Page, sql: str, expected_rows: int) -> None:
    page.locator("#request").fill(sql)
    page.locator("#run-btn").click()
    expect(page.locator("#result-count")).to_have_text(
        str(expected_rows), timeout=8000
    )


def test_continuous_circuit_renders_rv_arith_and_runs_probability(
    page: Page, studio_url: str
) -> None:
    """Drive the full continuous-distribution Circuit-mode round-trip:
    query a tracked RV table with a strict-inequality predicate, render
    the resulting provenance circuit, verify the new gate types
    (`gate_rv`, `gate_arith`) render with their specific in-circle
    labels (kind initial + parenthesised parameters for rv; the
    PROVSQL_ARITH_* glyph for arith), then run Monte-Carlo probability
    against the root token and check the result lies strictly in (0, 1)
    -- the canonical 'the planner hook lifted reading > 2 into
    provenance and the sampler returned a non-trivial probability'
    proof."""
    page.goto(studio_url + "/circuit")
    expect(page.locator("body")).to_have_class("mode-circuit", timeout=5000)

    _run_query_and_wait(
        page,
        "SELECT id, reading FROM sensors "
        "WHERE id IN ('s1', 's2', 's3') AND reading > 2 ORDER BY id;",
        3,
    )

    # Header decoration: every <th> carries a `title` attribute with the
    # Postgres type name, and ProvSQL-significant columns additionally
    # surface a small pill so users can tell at a glance which columns
    # have non-plain-SQL semantics:
    #   - terracotta `rv` for random_variable
    #   - terracotta `agg` for agg_token
    #   - purple `prov` for the provsql uuid column itself
    # The `id` column is plain `text`, so only the tooltip applies.
    id_header = (
        page.locator("#result-head th")
        .filter(has=page.locator(".wp-result__col-name", has_text="id"))
        .first
    )
    expect(id_header).to_have_attribute("title", "text")
    reading_header = (
        page.locator("#result-head th")
        .filter(has=page.locator(".wp-result__col-name", has_text="reading"))
        .first
    )
    expect(reading_header).to_have_attribute("title", "random_variable")
    expect(reading_header.locator(".wp-result__col-rv")).to_be_visible()
    # `provsql` is the row's provenance UUID column. The renderer keeps it
    # in circuit mode (this test runs in circuit mode), so the prov pill
    # should appear on the rightmost header.
    provsql_header = (
        page.locator("#result-head th")
        .filter(
            has=page.locator(".wp-result__col-name", has_text="provsql")
        )
        .first
    )
    expect(provsql_header).to_have_attribute("title", "uuid")
    expect(provsql_header.locator(".wp-result__col-prov")).to_be_visible()

    # Pin the s3 row: it's the one that contains a gate_arith on top of a
    # gate_rv, so its sub-DAG is the most informative for the new-gate
    # rendering check.  The provsql column is rendered as the last <td>
    # in each row; pick the row whose `id` cell reads "s3".
    s3_uuid_cell = (
        page.locator("#result-body tr")
        .filter(has_text="s3")
        .first.locator("td")
        .last
    )
    s3_uuid_cell.click()

    # Wait for the SVG to populate (frontier expansion is async).
    expect(page.locator("#sidebar-body svg").first).to_be_visible(timeout=8000)

    # gate_rv: the circle group carries class node--rv, and the in-circle
    # label is the kind initial + parenthesised parameters (e.g. "N(0,1)"
    # or "U(1,3)") -- mirrors the server-side _format_rv_label in
    # circuit.py.  At least one such node must be present.
    rv_nodes = page.locator(".node-group.node--rv")
    expect(rv_nodes.first).to_be_visible(timeout=8000)
    rv_labels = rv_nodes.locator(".node-label").all_text_contents()
    assert any(re.match(r"^[NUE]\w*\(", lbl) for lbl in rv_labels), (
        f"Expected at least one rv label of the form 'N(...)' / 'U(...)' "
        f"/ 'Exp(...)'; got {rv_labels!r}"
    )

    # gate_arith: the s3 row's `reading` is `N(0, 1) + as_random(2.0)`,
    # which is stored as gate_arith(PLUS, gate_rv, gate_value).  The
    # circle is labelled with the PROVSQL_ARITH_* glyph -- "+" for PLUS
    # (see _ARITH_OP_GLYPH in circuit.py).
    arith_nodes = page.locator(".node-group.node--arith")
    expect(arith_nodes.first).to_be_visible(timeout=8000)
    arith_labels = arith_nodes.locator(".node-label").all_text_contents()
    assert any(lbl in {"+", "−", "×", "÷"} for lbl in arith_labels), (
        f"Expected at least one arith label among the operator glyphs; "
        f"got {arith_labels!r}"
    )

    # Probability eval: the strip drives /api/evaluate with the pinned
    # node (s3's root cmp).  Pin a Monte-Carlo run with a small sample
    # count -- we're checking that the returned value lies strictly in
    # (0, 1), not asserting any specific number, so 1k samples are
    # ample.
    expect(page.locator("#eval-strip")).to_be_visible(timeout=8000)
    page.locator("#eval-semiring").select_option(value="probability")
    page.locator("#eval-method").select_option(value="monte-carlo")
    page.locator("#eval-args-mc").fill("1000")
    page.locator("#eval-run").click()

    result = page.locator("#eval-result")
    # The result chip prefixes successful evaluations with "= " and
    # writes the probability as a fixed-decimal string.  Wait for the
    # value to land, then parse and check the open interval.
    expect(result).to_have_attribute("data-kind", "ok", timeout=15000)
    text = result.text_content() or ""
    match = re.search(r"=\s*([0-9.]+)", text)
    assert match, f"Expected '= <number>' in result chip; got {text!r}"
    p = float(match.group(1))
    assert 0.0 < p < 1.0, (
        f"Expected p strictly in (0, 1) for `reading > 2` over s3; got {p}"
    )


# ----------------------------------------------------------------------
# Distribution-profile analytical-curve overlay.
#
# Backend coverage already lives in
# test/sql/continuous_analytical_curves.sql (PDF / CDF / stems payload
# shape) and studio/tests/test_evaluate.py (Studio's /api/evaluate
# wiring).  The tests below close the loop at the SVG-DOM layer: the
# Distribution profile panel must actually inject the
# `<path class="cv-profile-overlay">` (or `<g class="cv-profile-stems">`)
# elements that renderProfilePanel produces, and must NOT inject them
# for composite shapes that lack a closed-form payload.
# ----------------------------------------------------------------------


def _open_circuit_and_run_profile(
    page: Page, studio_url: str, sql: str, click_column: str = "reading"
) -> None:
    """Helper: navigate to Circuit mode, run `sql` (expected to return
    exactly one row), click the requested cell to load the circuit, and
    run the distribution-profile evaluator.  Returns once the result
    chip carries `data-kind="distribution-profile"`.

    `click_column` picks which cell is clicked, which in turn picks
    which UUID becomes the scene root: `reading` (the random_variable
    column) makes the RV / arith / mixture gate the root; `provsql`
    (the row's provenance gate from add_provenance) makes the Boolean
    provenance circuit the root.  Conditioning is auto-filled from the
    row's `provsql` UUID either way -- to suppress it, the caller can
    clear `#eval-args-condition` before clicking Run."""
    page.goto(studio_url + "/circuit")
    expect(page.locator("body")).to_have_class("mode-circuit", timeout=5000)
    _run_query_and_wait(page, sql, 1)
    # Read the result-table header to find the column index of
    # `click_column` -- robust to future column-order changes in the
    # query.  The header is a flat list of <th>; the table body uses
    # the same column order.  Query the inner column-name span rather
    # than the <th> itself so trailing rv / agg / prov pills don't leak
    # into the column name string.
    headers = page.locator(
        "#result-head th .wp-result__col-name"
    ).all_text_contents()
    try:
        col_idx = headers.index(click_column)
    except ValueError as exc:
        raise AssertionError(
            f"Column {click_column!r} not in header {headers!r}"
        ) from exc
    page.locator("#result-body tr").first.locator("td").nth(col_idx).click()
    expect(page.locator("#sidebar-body svg").first).to_be_visible(timeout=8000)
    expect(page.locator("#eval-strip")).to_be_visible(timeout=8000)
    # Select Distribution profile.  The eval-args-bins control becomes
    # visible (syncControls dispatch); leave its default of 30.
    page.locator("#eval-semiring").select_option(value="distribution-profile")
    page.locator("#eval-run").click()
    result = page.locator("#eval-result")
    expect(result).to_have_attribute(
        "data-kind", "distribution-profile", timeout=20000
    )


def test_overlay_present_for_bare_normal(
    page: Page, studio_url: str
) -> None:
    """Bare Normal(2.5, 0.5): the closed-form PDF curve must overlay
    the histogram.  Asserts both the bars `<g>` and the analytical-curve
    `<path>` are present in the rendered SVG."""
    _open_circuit_and_run_profile(
        page, studio_url, "SELECT reading FROM sensors WHERE id = 's1';"
    )
    panel = page.locator(".cv-profile-panel")
    expect(panel).to_be_visible(timeout=5000)
    expect(panel.locator(".cv-profile-bars")).to_have_count(1)
    expect(panel.locator("path.cv-profile-overlay")).to_have_count(1)


def test_overlay_present_and_decaying_for_folded_exp(
    page: Page, studio_url: str
) -> None:
    """`2 * Exp(0.4)` is persisted as gate_arith(TIMES, value:2, Exp(0.4))
    but the load-time simplifier folds it to Exp(0.2).  The overlay
    payload then carries an exponential PDF over the simplifier's
    `pdf_window` heuristic: pdf(x_lo) is the leftmost sample, and
    `Exp(0.2)`'s density decays monotonically, so pdf(leftmost) must
    strictly exceed pdf(rightmost).  Read the analytical-curves payload
    off `#eval-result.__profile` (parked there by wireProfileInteractions
    so the PDF/CDF toggle can re-render without a server round-trip)."""
    _open_circuit_and_run_profile(
        page, studio_url, "SELECT reading FROM sensors WHERE id = 's4';"
    )
    panel = page.locator(".cv-profile-panel")
    expect(panel).to_be_visible(timeout=5000)
    expect(panel.locator("path.cv-profile-overlay")).to_have_count(1)
    pdf = page.evaluate(
        "() => document.getElementById('eval-result').__profile"
        ".analytical_curves.pdf"
    )
    assert isinstance(pdf, list) and len(pdf) >= 2, (
        f"Expected analytical_curves.pdf to be a non-trivial array; got {pdf!r}"
    )
    p_first = float(pdf[0]["p"])
    p_last = float(pdf[-1]["p"])
    assert p_first > p_last, (
        f"Expected exponential decay (PDF leftmost > PDF rightmost) for "
        f"folded 2*Exp(0.4); got {p_first} vs {p_last}"
    )


def test_overlay_bounded_for_truncated_normal(
    page: Page, studio_url: str
) -> None:
    """Normal(0, 1) | -2 < X < 2: clicking the `reading` cell roots the
    scene at the bare gate_rv, while the condition auto-fills with the
    row's provenance gate (the conjunction of cmp_gt and cmp_lt
    introduced by the planner-hook lift of `reading > -2 AND reading <
    2`).  truncateShape extracts those cmps and clips the curve to
    [-2, 2]; the overlay's leftmost and rightmost sample x-coordinates
    must sit at the truncation bounds (matching the
    `truncated_normal` SQL test)."""
    _open_circuit_and_run_profile(
        page,
        studio_url,
        "SELECT reading FROM sensors "
        "WHERE id = 's5' AND reading > -2 AND reading < 2;",
    )
    panel = page.locator(".cv-profile-panel")
    expect(panel).to_be_visible(timeout=5000)
    expect(panel.locator("path.cv-profile-overlay")).to_have_count(1)
    pdf = page.evaluate(
        "() => document.getElementById('eval-result').__profile"
        ".analytical_curves.pdf"
    )
    assert isinstance(pdf, list) and len(pdf) >= 2, (
        f"Expected analytical_curves.pdf to be a non-trivial array; got {pdf!r}"
    )
    x_first = float(pdf[0]["x"])
    x_last = float(pdf[-1]["x"])
    assert abs(x_first - (-2.0)) < 1e-6, (
        f"Expected leftmost truncated-PDF x at -2.0; got {x_first}"
    )
    assert abs(x_last - 2.0) < 1e-6, (
        f"Expected rightmost truncated-PDF x at 2.0; got {x_last}"
    )


def test_overlay_absent_for_heterogeneous_exp_sum(
    page: Page, studio_url: str
) -> None:
    """`Exp(0.4) + Exp(0.3)` is NOT a closed-form family (the sum-of-
    independent-Erlangs fast path requires equal rates).  The simplifier
    leaves the gate_arith composite intact; rv_analytical_curves
    returns NULL; renderProfilePanel falls back to histogram-only
    rendering.  The bars must still be there, but the analytical-curve
    overlay `<path>` must NOT be."""
    _open_circuit_and_run_profile(
        page, studio_url, "SELECT reading FROM sensors WHERE id = 's6';"
    )
    panel = page.locator(".cv-profile-panel")
    expect(panel).to_be_visible(timeout=5000)
    expect(panel.locator(".cv-profile-bars")).to_have_count(1)
    expect(panel.locator("path.cv-profile-overlay")).to_have_count(0)


def test_stems_rendered_for_categorical(
    page: Page, studio_url: str
) -> None:
    """3-outcome categorical: the analytical curves payload carries a
    `stems` channel rather than a smooth PDF, and renderProfilePanel
    injects a `<g class="cv-profile-stems">` containing one `<line>` +
    one `<circle>` per outcome.  This covers Item 6b's end-to-end
    rendering."""
    _open_circuit_and_run_profile(
        page, studio_url, "SELECT reading FROM sensors WHERE id = 's7';"
    )
    panel = page.locator(".cv-profile-panel")
    expect(panel).to_be_visible(timeout=5000)
    stems = panel.locator("g.cv-profile-stems")
    expect(stems).to_have_count(1)
    # One vertical line + one disc per outcome.  The categorical has
    # three outcomes; the SQL test asserts the same masses but we only
    # care about element count at the DOM layer (the masses are tested
    # by continuous_analytical_curves.sql).
    expect(stems.locator("line")).to_have_count(3)
    expect(stems.locator("circle")).to_have_count(3)
