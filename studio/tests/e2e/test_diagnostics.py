"""How ProvSQL's diagnostics are shown: the provsql-reason tag of the
DETAIL as a badge on the first line, and the HINT on a line of its own,
in the result pane and in Circuit mode's evaluation strip."""
from __future__ import annotations

from playwright.sync_api import Page, expect


def test_warning_shows_reason_badge_and_hint(page: Page, studio_url: str) -> None:
    """A LIMIT without ORDER BY over tracked data warns: the banner shows
    the tag as a badge and the hint, with nothing left to unfold."""
    page.goto(studio_url + "/circuit")
    page.locator("#request").fill("SELECT name FROM personnel LIMIT 2")
    page.locator("#run-btn").click()
    banner = page.locator("#result-banners .wp-warning")
    expect(banner).to_be_visible(timeout=8000)
    expect(banner.locator(".wp-reason")).to_have_text("limit-without-order-by")
    expect(banner.locator(".wp-reason")).to_have_attribute(
        "title", "provsql-reason: limit-without-order-by "
                 "(deliberate: ProvSQL behaves so by design)")
    expect(banner.locator(".wp-diag__hint")).to_contain_text("LIMIT plain(k)")
    expect(banner).not_to_contain_text("DETAIL")
    assert banner.evaluate("e => e.tagName") == "DIV"  # not folded


def test_evaluation_strip_error_shows_reason_badge_and_hint(
    page: Page, studio_url: str
) -> None:
    """An evaluation stopped by provsql.max_worlds shows, in the strip, the
    world-limit badge and the hint naming the setting to raise."""
    page.goto(studio_url + "/circuit")
    set_worlds = (
        "v => fetch('/api/config', {method: 'POST', headers: "
        "{'Content-Type': 'application/json'}, body: JSON.stringify("
        "{key: 'provsql.max_worlds', value: v})})"
    )
    page.evaluate(set_worlds, "2")
    try:
        page.locator("#request").fill(
            "SELECT city FROM personnel GROUP BY city "
            "HAVING SUM(id) > 3 AND COUNT(*) > 1")
        page.locator("#run-btn").click()
        expect(page.locator("#result-count")).not_to_have_text(
            "...", timeout=8000)
        page.locator("#result-body tr").first.locator("td").last.click()
        page.wait_for_selector("#eval-strip", state="visible", timeout=8000)
        page.locator("#eval-semiring").select_option("probability")
        page.locator("#eval-run").click()
        err = page.locator("#eval-result .wp-error")
        expect(err).to_be_visible(timeout=8000)
        expect(err.locator(".wp-reason")).to_have_text("world-limit")
        expect(err.locator(".wp-diag__hint")).to_contain_text(
            "Raise provsql.max_worlds")
    finally:
        page.evaluate(set_worlds, "1048576")
