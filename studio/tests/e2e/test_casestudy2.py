"""End-to-end walkthrough of Case Study 2's **Contributions mode** section
(doc/source/user/casestudy2.rst, "Seeing it in Studio: Contributions mode").

The chapter claims that running the replication query for the
Exercise → Cardiovascular Disease → beneficial finding, labelling inputs with
``study_mapping`` and pinning the result row, draws one ranked bar per study
-- the interactive twin of the Step 13-15 :sqlfunc:`shapley` / :sqlfunc:`banzhaf`
computations. This test drives that on the live UI and asserts the per-study
Shapley **and** Banzhaf values, so the figure's numbers cannot drift from the
SQL ones.

Backed by the `cs2_studio_url` session fixture (Studio on the Case Study 2
database) and pytest-playwright's `page`."""
from __future__ import annotations

import re

from playwright.sync_api import Page, expect


REPLICATION_QUERY = (
    "SELECT exposure, outcome, effect FROM f_replicated "
    "WHERE exposure = 'Exercise' AND outcome = 'Cardiovascular Disease' "
    "AND effect = 'beneficial';"
)

# The Step 13 / Step 14 values (test/expected/casestudy2.out): one entry per
# study, for each of the two measures the Contributions toggle offers.
SHAPLEY = {"Johnson2020": 0.3531, "Smith2018": 0.3267, "Williams2021": 0.3071}
BANZHAF = {"Johnson2020": 1.7640, "Smith2018": 1.7112, "Williams2021": 1.6720}


def _bar_value(page: Page, study: str) -> float:
    """The numeric value shown on the contribution bar whose (study_mapping)
    label is `study`."""
    bar = page.locator("#contrib-chart .cv-contrib__bar").filter(has_text=study).first
    txt = bar.locator(".cv-contrib__val").inner_text()
    return float(re.search(r"-?[0-9]*\.?[0-9]+", txt).group())


def test_contributions_shapley_then_banzhaf(page: Page, cs2_studio_url: str) -> None:
    page.goto(cs2_studio_url + "/contributions")
    expect(page.locator("body")).to_have_class(
        re.compile(r"\bmode-contributions\b"), timeout=8000)

    # Label inputs by study name, run the replication query, pin its one row.
    page.locator("#contrib-mapping").select_option(label="study_mapping")
    page.locator("#request").fill(REPLICATION_QUERY)
    page.locator("#run-btn").click()
    expect(page.locator("#result-count")).to_have_text("1", timeout=15000)
    page.locator("#result-body tr").first.locator("td").last.click()

    # One ranked bar per contributing study.
    bars = page.locator("#contrib-chart .cv-contrib__bar")
    expect(bars).to_have_count(3, timeout=15000)
    # Labels resolve lazily (via /api/leaf) to the study_mapping value.
    expect(page.locator("#contrib-chart").get_by_text("Johnson2020")).to_be_visible(
        timeout=15000)

    # Default measure: expected Shapley values, summing to the replication prob.
    for study, sv in SHAPLEY.items():
        assert abs(_bar_value(page, study) - sv) < 2e-3, study
    assert abs(sum(_bar_value(page, s) for s in SHAPLEY) - 0.9869) < 5e-3

    # Flip the Measure toggle to Banzhaf (Step 14); same studies, Banzhaf numbers.
    page.locator("#contrib-measure").select_option("banzhaf")
    expect(bars).to_have_count(3, timeout=8000)
    page.wait_for_function(
        "() => {const b=[...document.querySelectorAll('#contrib-chart "
        ".cv-contrib__val')]; return b.length && b.some(e => "
        "parseFloat(e.textContent) > 1.0);}", timeout=10000)
    for study, bv in BANZHAF.items():
        assert abs(_bar_value(page, study) - bv) < 2e-3, study
