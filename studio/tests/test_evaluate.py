"""Tests for the semiring-evaluation endpoints in circuit mode:

  * GET  /api/provenance_mappings  – discover (value, provenance uuid) tables.
  * POST /api/evaluate              – dispatch sr_* / probability_evaluate.

Both share a setup helper that builds a `personnel_names` mapping from the
personnel rows the conftest seeds, so the tests run against the same
fixture as test_relations.py and don't depend on cs5-style data.
"""
from __future__ import annotations

import pytest


@pytest.fixture()
def mapping(client):
    """Create a (value, provenance) mapping table from personnel.name and
    drop it after the test. The qualified name is `personnel_names`; via
    the test search_path it's also reachable unqualified."""
    setup = (
        "DROP TABLE IF EXISTS personnel_names;"
        " CREATE TABLE personnel_names AS"
        "   SELECT name AS value, provsql AS provenance FROM personnel;"
        " SELECT remove_provenance('personnel_names');"
        " CREATE INDEX ON personnel_names(provenance);"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    yield "personnel_names"
    client.post("/api/exec", json={"sql": "DROP TABLE personnel_names", "mode": "circuit"})


def _root_uuid(client, sql: str) -> str:
    """Run a circuit-mode query that produces a single provsql UUID and
    return it. The query must select from a provenance-tracked relation
    so the rewriter emits a provsql column."""
    resp = client.post("/api/exec", json={"sql": sql, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    payload = resp.get_json()
    final = payload["blocks"][-1]
    assert final["kind"] == "rows", payload
    cols = [c["name"] for c in final["columns"]]
    pi = cols.index("provsql")
    return final["rows"][0][pi]


# ──────── /api/provenance_mappings ────────


def test_mappings_lists_personnel_names(client, mapping):
    resp = client.get("/api/provenance_mappings")
    assert resp.status_code == 200
    rows = resp.get_json()
    qnames = {r["qname"] for r in rows}
    assert "provsql_test.personnel_names" in qnames
    entry = next(r for r in rows if r["qname"] == "provsql_test.personnel_names")
    # On the test search_path (provsql_test, provsql, public) the relation
    # is visible unqualified, so the bare name lands in display_name.
    assert entry["display_name"] == "personnel_names"


def test_mappings_skips_non_mapping_tables(client):
    """personnel itself has a provsql uuid column but no `value` column,
    so it must NOT appear in the mapping discovery result."""
    rows = client.get("/api/provenance_mappings").get_json()
    qnames = {r["qname"] for r in rows}
    assert "provsql_test.personnel" not in qnames


# ──────── /api/evaluate ────────


def test_evaluate_boolexpr_does_not_need_mapping(client):
    """boolexpr labels leaves with their own UUIDs, no mapping required."""
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={"token": root, "semiring": "boolexpr"})
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "text"
    # boolexpr returns the (single) input gate's UUID for a one-row select.
    assert isinstance(data["result"], str) and data["result"]


def test_evaluate_formula_with_mapping(client, mapping):
    """sr_formula labels each input gate with its mapped value."""
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "formula",
        "mapping": f"provsql_test.{mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "text"
    assert "John" in data["result"]


@pytest.fixture()
def counting_mapping(client):
    """Counting / boolean semirings need a typed mapping (the value
    column's type is consumed by the C semiring evaluator). We map every
    personnel row to 1 — sr_counting then sums to the row count, and
    sr_boolean treats a non-zero entry as TRUE."""
    setup = (
        "DROP TABLE IF EXISTS personnel_ones;"
        " CREATE TABLE personnel_ones AS"
        "   SELECT 1::int AS value, provsql AS provenance FROM personnel;"
        " SELECT remove_provenance('personnel_ones');"
        " CREATE INDEX ON personnel_ones(provenance);"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    yield "personnel_ones"
    client.post("/api/exec", json={"sql": "DROP TABLE personnel_ones", "mode": "circuit"})


def test_evaluate_counting_returns_int(client, counting_mapping):
    """sr_counting over the seven personnel rows collapsed into one
    DISTINCT result. The rewriter unions the seven input gates under a
    plus; with each leaf mapped to 1 the counting semiring sums to 7."""
    root = _root_uuid(client, "SELECT 1 AS k FROM personnel GROUP BY 1")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "counting",
        "mapping": f"provsql_test.{counting_mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "int"
    assert int(data["result"]) == 7


def test_evaluate_boolean_returns_bool(client, counting_mapping):
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "boolean",
        "mapping": f"provsql_test.{counting_mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "bool"
    assert data["result"] is True


def test_evaluate_missing_mapping_is_400(client):
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "formula",
    })
    assert resp.status_code == 400
    assert "mapping" in resp.get_json()["error"].lower()


def test_evaluate_unknown_semiring_is_400(client):
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "bogus",
    })
    assert resp.status_code == 400
    assert "unknown" in resp.get_json()["error"].lower()


def test_evaluate_unknown_probability_method_is_400(client):
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "probability",
        "method": "made-up-method",
    })
    assert resp.status_code == 400
    assert "method" in resp.get_json()["error"].lower()


def test_evaluate_invalid_uuid_is_400(client):
    resp = client.post("/api/evaluate", json={
        "token": "not-a-uuid",
        "semiring": "boolexpr",
    })
    assert resp.status_code == 400
