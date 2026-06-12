"""End-to-end run of Case Study 8 in **notebook mode**.

CS8 ("ProvSQL as a Probability Calculator") is the notebook-first case study:
a self-contained sequence of one-line probability queries. This test opens the
bundled ``cs8`` example in Studio's notebook mode and runs it end to end
against a live kernel, asserting that every cell executes and none produces an
error banner -- a regression guard that the shipped notebook still runs on the
current ProvSQL.

Backed by the `cs8_studio_url` session fixture (Studio on a fresh provsql
database) and pytest-playwright's `page`."""
from __future__ import annotations

import re

from playwright.sync_api import Page, expect


def test_cs8_notebook_runs_end_to_end(page: Page, cs8_studio_url: str) -> None:
    page.goto(cs8_studio_url + "/notebook")
    expect(page.locator("body")).to_have_class(
        re.compile(r"\bmode-notebook\b"), timeout=8000)
    # Start from a pristine notebook, not a previous run's autosaved draft.
    page.evaluate("localStorage.removeItem('ps.nb.autosave');"
                  "localStorage.removeItem('ps.nb.tabs')")
    page.reload()
    page.wait_for_selector("#notebook-pane", timeout=8000)

    # Open the bundled CS8 example; it loads its ~27 code cells.
    page.locator("#nb-example").select_option("cs8")
    cells = page.locator(".nb-cell--sql")
    expect(cells.nth(20)).to_be_attached(timeout=15000)
    total = cells.count()
    assert total >= 20, total

    # Run the whole notebook against a live kernel (binds + creates its own
    # tables); wait until the last cell carries an execution count.
    page.locator("#nb-run-all").click()
    expect(cells.last.locator(".nb-cell__count")).to_have_text(
        re.compile(r"\[\d+\]"), timeout=180000)

    # A real kernel served the run (not a cached render).
    expect(page.locator("#nb-kernel-label")).to_contain_text("pid", timeout=8000)
    # Every code cell ran (each shows an execution count) and none errored.
    counted = cells.locator(".nb-cell__count").filter(
        has_text=re.compile(r"\[\d+\]")).count()
    assert counted == total, f"{counted}/{total} cells executed"
    assert page.locator(".nb-out .wp-error").count() == 0, "error banner(s)"
    # Sanity that real probabilities were computed: the base-rate example's
    # unconditional P(positive) = 0.0585 appears in the executed output.
    expect(page.locator("#notebook-pane")).to_contain_text("0.0585", timeout=8000)

    # --- Exercise the interactive affordances CS8 showcases ---------------
    # CS8's headline is the `|` ("given") operator.  Click the conditioned
    # provenance token it produces (the insulin_resistance-given-obesity row)
    # to render its circuit inline, then Evaluate it.
    cond_cell = page.locator(
        ".nb-cell--sql", has_text="factor = 'insulin_resistance'").first
    token = cond_cell.locator(".nb-out [data-circuit-uuid]").first
    expect(token).to_be_visible(timeout=8000)
    token.scroll_into_view_if_needed()
    token.click()
    circ = page.locator(".nb-cell--circuit").first
    expect(circ.locator(".nb-circ__svg .node-group").first).to_be_visible(
        timeout=15000)

    # The circuit cell's Evaluate button inserts an evaluation cell; run it
    # (defaults to marginal probability / exact) and read back a value.
    circ.locator(".nb-circ__eval").click()
    ev = page.locator(".nb-cell--eval").first
    ev.locator(".nb-cell__run").click()
    value = ev.locator(".nb-eval__value")
    expect(value).not_to_have_text("", timeout=30000)
    # P(insulin_resistance | obesity) = 0.5 * 0.6 = 0.3 (casestudy8.rst).
    got = float(re.search(r"[0-9]*\.?[0-9]+", value.inner_text()).group())
    assert abs(got - 0.3) < 5e-3, value.inner_text()
