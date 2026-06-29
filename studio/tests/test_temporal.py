"""Tests for Temporal mode: the relation/mapping pickers, the source x timeop
matrix over a table / a view / a query, the error guards, and validity parsing
(empty union, unbounded, single instant, multi-component multirange).

All run against the `temporal_*` fixtures (see conftest's TEMPORAL_SETUP)."""
from __future__ import annotations

import pytest

from provsql_studio.db import _parse_multirange


def _temporal(client, **payload):
    return client.post("/api/temporal", json=payload)


def _years(intervals):
    """[(lower_year, upper_year)] for an interval list, None for unbounded ends."""
    out = []
    for iv in intervals:
        lo = iv["lower"][:4] if iv["lower"] else None
        hi = iv["upper"][:4] if iv["upper"] else None
        out.append((lo, hi))
    return out


def _first_span(row):
    return _years(row["valid_time"])[0]


def _by_lower(spans):
    return sorted(spans, key=lambda t: (t[0] or ""))


# ── Pickers ────────────────────────────────────────────────────────────────

def test_relation_picker_lists_relations_not_mappings(temporal_client):
    rels = temporal_client.get("/api/temporal_relations").get_json()
    qnames = {r["qname"] for r in rels}
    # Real relations and the join view are offered...
    assert {"cs4_holds", "cs4_person", "cs4_person_position",
            "sensor", "degen", "emptyval", "multi"} <= qnames
    # ...but the provenance-mapping views are not (they belong in the mapping
    # picker, not as relations to inspect).
    assert not any(q.endswith("_validity") for q in qnames)
    assert "time_validity_view" not in qnames


def test_mapping_picker_lists_validity_views_canonical_first(temporal_client):
    maps = temporal_client.get("/api/temporal_mappings").get_json()
    qnames = [m["qname"] for m in maps]
    assert qnames[0] == "provsql.time_validity_view"  # pinned first
    for v in ("public.sensor_validity", "public.degen_validity",
              "public.emptyval_validity", "public.multi_validity"):
        assert v in qnames


# ── Relation source x timeop ────────────────────────────────────────────────

def test_relation_full_table(temporal_client):
    d = _temporal(temporal_client, source="relation", relation="cs4_holds",
                  timeop="full").get_json()
    assert d["timeop"] == "full" and d["source"] == "relation"
    assert len(d["result"]) == 3
    spans = _by_lower(_first_span(r) for r in d["result"])
    assert spans == [("2010", "2016"), ("2016", "2022"), ("2022", None)]


def test_relation_asof_instant(temporal_client):
    d = _temporal(temporal_client, source="relation", relation="cs4_holds",
                  timeop="asof", at_time="2018-06-15").get_json()
    # Only the 2016-2022 term is valid at that instant.
    assert len(d["result"]) == 1
    assert _first_span(d["result"][0]) == ("2016", "2022")


def test_relation_during_overlap_not_clipped(temporal_client):
    # During is an overlap filter, mirroring timeslice: the two terms that meet
    # [2014, 2017) are returned with their FULL validity (2010-2016, 2016-2022),
    # NOT clipped to the window. (The Studio timeline frames the window
    # client-side; the data is unclipped.)
    d = _temporal(temporal_client, source="relation", relation="cs4_holds",
                  timeop="during", from_time="2014-01-01", to_time="2017-01-01").get_json()
    spans = _by_lower(_first_span(r) for r in d["result"])
    assert spans == [("2010", "2016"), ("2016", "2022")]


def test_view_join_intersects_validity(temporal_client):
    # The join view's provenance couples each person's lifespan with their term;
    # sr_temporal yields the term windows, and all three PMs appear.
    d = _temporal(temporal_client, source="relation", relation="cs4_person_position",
                  timeop="full").get_json()
    assert len(d["result"]) == 3
    assert {r["name"] for r in d["result"]} == {"Alice Blanc", "Bernard Chai", "Carla Diop"}
    spans = _by_lower(_first_span(r) for r in d["result"])
    assert spans == [("2010", "2016"), ("2016", "2022"), ("2022", None)]


# ── Query source ────────────────────────────────────────────────────────────

def test_query_source_full(temporal_client):
    d = _temporal(temporal_client, source="query", query="SELECT * FROM sensor",
                  mapping="public.sensor_validity", timeop="full").get_json()
    assert len(d["result"]) == 3
    assert {r["reading"] for r in d["result"]} == {"A", "B", "C"}


def test_query_source_asof(temporal_client):
    # At 14:04 only A [14:00,14:05) and B [14:03,14:12) are valid; C starts 14:10.
    d = _temporal(temporal_client, source="query", query="SELECT * FROM sensor",
                  mapping="public.sensor_validity", timeop="asof",
                  at_time="2024-03-15T14:04").get_json()
    assert {r["reading"] for r in d["result"]} == {"A", "B"}


def test_query_source_during(temporal_client):
    d = _temporal(temporal_client, source="query", query="SELECT * FROM sensor",
                  mapping="public.sensor_validity", timeop="during",
                  from_time="2024-03-15T14:06", to_time="2024-03-15T14:09").get_json()
    # [14:06,14:09) overlaps only B [14:03,14:12).
    assert {r["reading"] for r in d["result"]} == {"B"}


# ── Validity parsing edge cases (real psycopg Multirange path) ───────────────

def test_empty_union_and_bounded(temporal_client):
    d = _temporal(temporal_client, source="query", query="SELECT * FROM emptyval",
                  mapping="public.emptyval_validity", timeop="full").get_json()
    by = {r["label"]: r["valid_time"] for r in d["result"]}
    assert by["never"] == []                       # empty {} -> no intervals
    assert _years(by["normal"]) == [("2016", "2022")]


def test_unbounded_and_single_instant(temporal_client):
    d = _temporal(temporal_client, source="query", query="SELECT * FROM degen",
                  mapping="public.degen_validity", timeop="full").get_json()
    by = {r["label"]: r["valid_time"] for r in d["result"]}
    assert by["alltime"] == [{"lower": None, "upper": None}]   # (-inf, +inf)
    point = by["point"]
    assert len(point) == 1
    assert point[0]["lower"] is not None and point[0]["lower"] == point[0]["upper"]


def test_multicomponent_multirange_splits(temporal_client):
    d = _temporal(temporal_client, source="query", query="SELECT * FROM multi",
                  mapping="public.multi_validity", timeop="full").get_json()
    assert len(d["result"]) == 1
    assert _years(d["result"][0]["valid_time"]) == [("2000", "2001"), ("2010", "2011")]


def test_duplicate_column_names_kept_in_cells(temporal_client):
    # Two columns aliased to the same name collapse in the name-keyed row dict,
    # but `cells` keeps both values positionally so the lane label is correct.
    d = _temporal(temporal_client, source="query",
                  query="SELECT reading AS x, id::text AS x FROM sensor WHERE id = 1",
                  mapping="public.sensor_validity", timeop="full").get_json()
    assert d["columns"] == ["x", "x"]
    assert d["result"][0]["cells"] == ["A", "1"]


def test_query_without_provenance_gives_clear_error(temporal_client):
    # A query that reads no provenance-tracked relation has no provenance
    # column; the error must explain that, not just "temporal query failed".
    r = _temporal(temporal_client, source="query", query="SELECT 1 AS x",
                  mapping="public.sensor_validity", timeop="full")
    assert r.status_code == 400
    assert "no provenance column" in r.get_json()["error"]


def test_sql_error_surfaces_postgres_message(temporal_client):
    # A genuine SQL error surfaces PostgreSQL's own message to the user.
    r = _temporal(temporal_client, source="query",
                  query="SELECT * FROM no_such_table_xyz",
                  mapping="public.sensor_validity", timeop="full")
    assert r.status_code == 500
    assert "does not exist" in r.get_json()["error"]


def test_parse_multirange_none():
    assert _parse_multirange(None) == []


# ── Error guards ─────────────────────────────────────────────────────────────

@pytest.mark.parametrize("payload, msg", [
    ({"source": "bogus", "timeop": "full"}, "unknown temporal source"),
    ({"source": "relation", "timeop": "bogus", "relation": "cs4_holds"},
     "unknown temporal time operation"),
    ({"source": "relation", "timeop": "full", "relation": "no_such_rel"},
     "unknown temporal relation"),
    ({"source": "query", "timeop": "full"}, "query source requires"),
    ({"source": "query", "timeop": "full", "query": "SELECT 1",
      "mapping": "public.no_such_mapping"}, "unknown temporal mapping"),
    ({"source": "relation", "timeop": "asof", "relation": "cs4_holds"},
     "requires an instant"),
    ({"source": "relation", "timeop": "during", "relation": "cs4_holds",
      "from_time": "2014-01-01"}, "requires from_time and to_time"),
])
def test_error_guards(temporal_client, payload, msg):
    resp = _temporal(temporal_client, **payload)
    assert resp.status_code == 400, resp.data
    assert msg in resp.get_json()["error"]
