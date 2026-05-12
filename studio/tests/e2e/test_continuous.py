"""End-to-end test for continuous-distribution support in Circuit mode.

Loads a `sensors` fixture with two random_variable distributions plus one
RV expression built via the `+` operator (so the resulting circuit
exercises both new gate types: `gate_rv` *and* `gate_arith`).  Then drives
the live UI: run `WHERE reading > 2`, click into the resulting circuit,
verify the rv / arith nodes render with their specific labels, and run
probability monte-carlo to confirm the strip reports `p ∈ (0, 1)`.

Mirrors the verification example anchored in TODO.md / the operation plan."""
from __future__ import annotations

import re

import psycopg
import pytest
from playwright.sync_api import Page, expect


@pytest.fixture(scope="session", autouse=True)
def _load_sensors(test_dsn: str) -> None:
    """Add a `sensors` table to the e2e database so Circuit mode has
    something with random_variable columns to query.

    Three rows so the circuit produced by `WHERE reading > 2` exercises
    every new gate type:
      * s1 is a bare normal -> gate_rv
      * s2 is a bare uniform -> gate_rv
      * s3 sums two uniforms (Irwin-Hall) -> gate_arith over two
        gate_rvs.  Two uniforms are deliberately chosen over a normal
        sum because the simplifier's normal-family closure would fold
        `N(0,1) + N(1,1)` to a single `gate_rv`, hiding the arith node
        from the rendered circuit -- but uniform + uniform isn't a
        standard family, so the simplifier leaves the arith intact.

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
            "('s3', provsql.uniform(0, 1) + provsql.uniform(0, 2))"
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
        "SELECT id, reading FROM sensors WHERE reading > 2 ORDER BY id;",
        3,
    )

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
