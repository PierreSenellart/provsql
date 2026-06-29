"""End-to-end smoke tests for Temporal mode.

Drive the live timeline against the Temporal fixture DB (see conftest's
TEMPORAL_SETUP): the source x timeop controls, the During window frame, the
axis date caption, and the empty-union marker."""
from __future__ import annotations

import re

from playwright.sync_api import Page, expect


def _goto_temporal(page: Page, url: str) -> None:
    page.goto(url + "/temporal")
    expect(page.locator("body")).to_have_class("mode-temporal", timeout=5000)


def test_relation_full_renders_all_lanes(page: Page, temporal_studio_url: str) -> None:
    """Relation source, Full: the three Prime-Minister terms each get a lane
    and a bar."""
    _goto_temporal(page, temporal_studio_url)
    page.locator("#temporal-relation").select_option("cs4_holds")
    page.locator('.cv-temporal__op[data-timeop="full"]').click()
    expect(page.locator(".cv-temporal__lane")).to_have_count(3, timeout=8000)
    expect(page.locator(".cv-temporal__bar")).to_have_count(3, timeout=8000)


def test_relation_asof_filters_to_one(page: Page, temporal_studio_url: str) -> None:
    """As of 2018-06-15 only the 2016-2022 term is valid: a single lane."""
    _goto_temporal(page, temporal_studio_url)
    page.locator("#temporal-relation").select_option("cs4_holds")
    page.locator('.cv-temporal__op[data-timeop="asof"]').click()
    page.locator("#temporal-at").fill("2018-06-15T00:00")
    page.locator("#temporal-at").dispatch_event("change")
    expect(page.locator(".cv-temporal__lane")).to_have_count(1, timeout=8000)


def test_during_draws_window_frame(page: Page, temporal_studio_url: str) -> None:
    """During [2014, 2017): the two overlapping terms show (full, unclipped
    bars) and the window frame draws two dimming masks and two edges."""
    _goto_temporal(page, temporal_studio_url)
    page.locator("#temporal-relation").select_option("cs4_holds")
    page.locator('.cv-temporal__op[data-timeop="during"]').click()
    page.locator("#temporal-from").fill("2014-01-01T00:00")
    page.locator("#temporal-from").dispatch_event("change")
    page.locator("#temporal-to").fill("2017-01-01T00:00")
    page.locator("#temporal-to").dispatch_event("change")
    expect(page.locator(".cv-temporal__lane")).to_have_count(2, timeout=8000)
    expect(page.locator(".cv-temporal__wmask")).to_have_count(2, timeout=8000)
    expect(page.locator(".cv-temporal__wedge")).to_have_count(2)


def test_query_source_shows_axis_date_caption(
    page: Page, temporal_studio_url: str
) -> None:
    """Query source over the minute-scale sensor data: the axis caption
    surfaces the date the clock-time ticks omit."""
    _goto_temporal(page, temporal_studio_url)
    page.locator('.cv-temporal__src[data-source="query"]').click()
    page.locator('.cv-temporal__op[data-timeop="full"]').click()
    page.locator("#request").fill("SELECT * FROM sensor")
    # Selecting the mapping triggers the fetch (reading the query box).
    page.locator("#temporal-mapping").select_option("public.sensor_validity")
    expect(page.locator(".cv-temporal__lane")).to_have_count(3, timeout=8000)
    expect(page.locator(".cv-temporal__axisctx")).to_have_text("2024-03-15", timeout=8000)


def test_order_by_start_resorts_lanes(
    page: Page, temporal_studio_url: str
) -> None:
    """The Order control re-sorts the lanes client-side by validity. A query
    deliberately returned in reverse-chronological order (id DESC over the
    three Prime-Minister terms) shows id 3 first; switching Order to 'by start'
    floats the earliest term (id 1) to the top."""
    _goto_temporal(page, temporal_studio_url)
    page.locator('.cv-temporal__src[data-source="query"]').click()
    page.locator('.cv-temporal__op[data-timeop="full"]').click()
    page.locator("#request").fill("SELECT * FROM cs4_holds ORDER BY id DESC")
    page.locator("#temporal-mapping").select_option("provsql.time_validity_view")
    lane0 = page.locator(".cv-temporal__lane").first.locator(".cv-temporal__lanelbl")
    expect(lane0).to_contain_text("3", timeout=8000)   # query order: id 3 (2022) first
    page.locator("#temporal-sort").select_option("start")
    expect(lane0).to_contain_text("1")                 # by start: id 1 (2010) first


def test_reload_restores_query_source(
    page: Page, temporal_studio_url: str
) -> None:
    """A plain page reload restores the Temporal UI (source, query, time op)
    rather than silently falling back to the Relation display."""
    _goto_temporal(page, temporal_studio_url)
    page.locator('.cv-temporal__src[data-source="query"]').click()
    page.locator('.cv-temporal__op[data-timeop="full"]').click()
    page.locator("#request").fill("SELECT * FROM sensor")
    page.locator("#temporal-mapping").select_option("public.sensor_validity")
    expect(page.locator(".cv-temporal__lane")).to_have_count(3, timeout=8000)

    page.reload(wait_until="networkidle")
    # Still in Query source, query preserved, timeline redrawn -- not reverted.
    expect(page.locator('.cv-temporal__src[data-source="query"]')).to_have_class(
        re.compile(r"\bis-active\b"), timeout=8000)
    expect(page.locator("#request")).to_have_value("SELECT * FROM sensor")
    expect(page.locator(".cv-temporal__lane")).to_have_count(3, timeout=8000)


def test_failed_query_clears_timeline(
    page: Page, temporal_studio_url: str
) -> None:
    """A query that errors must erase the timeline, not leave the previous
    result on screen behind the error banner."""
    _goto_temporal(page, temporal_studio_url)
    page.locator('.cv-temporal__src[data-source="query"]').click()
    page.locator('.cv-temporal__op[data-timeop="full"]').click()
    page.locator("#request").fill("SELECT * FROM sensor")
    page.locator("#temporal-mapping").select_option("public.sensor_validity")
    expect(page.locator(".cv-temporal__lane")).to_have_count(3, timeout=8000)

    # A failing query (unknown relation) -> error banner, and no stale lanes.
    page.locator("#request").fill("SELECT * FROM no_such_table_xyz")
    page.locator("#request").dispatch_event("change")
    expect(page.locator(".cv-temporal__status.is-error")).to_be_visible(timeout=8000)
    expect(page.locator(".cv-temporal__lane")).to_have_count(0)


def test_empty_union_renders_never_marker(
    page: Page, temporal_studio_url: str
) -> None:
    """The row with an empty validity union renders the '∅ never' marker
    rather than a bar."""
    _goto_temporal(page, temporal_studio_url)
    page.locator('.cv-temporal__src[data-source="query"]').click()
    page.locator('.cv-temporal__op[data-timeop="full"]').click()
    page.locator("#request").fill("SELECT * FROM emptyval")
    page.locator("#temporal-mapping").select_option("public.emptyval_validity")
    expect(page.locator(".cv-temporal__never")).to_be_visible(timeout=8000)
