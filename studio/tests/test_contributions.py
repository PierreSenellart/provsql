"""Tests for the Contributions-mode endpoint:

  * POST /api/contributions – per-input Shapley / Banzhaf contributions
    toward one pinned result token, backed by provsql.shapley_all_vars.

Uses the same personnel fixture as test_evaluate.py. The target circuit is
``SELECT DISTINCT city FROM personnel WHERE city = 'Paris'`` whose single
result row is a ⊕ over the three Paris input gates (Dave, Magdalen, Nancy),
so the all-variables enumeration returns exactly those three inputs.
"""
from __future__ import annotations

import pytest


@pytest.fixture()
def names_mapping(client):
    """A (value, provenance) mapping from personnel.name, so contributions
    resolve to person names. Qualified `personnel_names`; via the test
    search_path it is also reachable unqualified."""
    setup = (
        "DROP TABLE IF EXISTS personnel_names;"
        " CREATE TABLE personnel_names AS"
        "   SELECT name AS value, provsql AS provenance FROM personnel;"
        " SELECT remove_provenance('personnel_names');"
        " CREATE INDEX ON personnel_names(provenance);"
        # Give every input gate a probability so expected Shapley / Banzhaf
        # are well-defined and deterministic.
        " DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM personnel; END $$;"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "contributions"})
    assert resp.status_code == 200, resp.data
    yield "provsql_test.personnel_names"
    # Restore personnel to the implicit default probability (1.0) so this
    # fixture does not leak 0.5 into the session-shared DB and break tests
    # that assume untouched probabilities (e.g. test_circuit's leaf-resolve).
    client.post("/api/exec", json={
        "sql": "DROP TABLE personnel_names;"
               " DO $$ BEGIN PERFORM set_prob(provsql, 1.0) FROM personnel; END $$;",
        "mode": "contributions",
    })


def _paris_token(client) -> str:
    """Run the Paris DISTINCT query and return its result-row provsql UUID."""
    resp = client.post("/api/exec", json={
        "sql": "SELECT DISTINCT city FROM personnel WHERE city = 'Paris'",
        "mode": "contributions",
    })
    assert resp.status_code == 200, resp.data
    final = resp.get_json()["blocks"][-1]
    assert final["kind"] == "rows", final
    cols = [c["name"] for c in final["columns"]]
    return final["rows"][0][cols.index("provsql")]


def test_contributions_shapley_with_mapping(client, names_mapping):
    token = _paris_token(client)
    resp = client.post("/api/contributions", json={
        "token": token,
        "measure": "shapley",
        "mapping": names_mapping,
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "contributions"
    assert data["measure"] == "shapley"
    result = data["result"]
    # Three Paris inputs, each labelled by name via the mapping.
    assert len(result) == 3
    labels = {r["label"] for r in result}
    assert labels == {"Dave", "Magdalen", "Nancy"}
    for r in result:
        assert r["value"] is not None and float(r["value"]) > 0
    # Returned in descending contribution order.
    vals = [float(r["value"]) for r in result]
    assert vals == sorted(vals, reverse=True)


def test_contributions_banzhaf_measure(client, names_mapping):
    token = _paris_token(client)
    resp = client.post("/api/contributions", json={
        "token": token,
        "measure": "banzhaf",
        "mapping": names_mapping,
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["measure"] == "banzhaf"
    assert len(data["result"]) == 3


def test_contributions_without_mapping_has_null_labels(client, names_mapping):
    # names_mapping is still used only to seed probabilities here; we pass
    # no mapping in the request, so labels come back as None for the
    # front-end to resolve lazily via /api/leaf.
    token = _paris_token(client)
    resp = client.post("/api/contributions", json={"token": token})
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["measure"] == "shapley"  # default
    assert data["mapping"] is None
    assert len(data["result"]) == 3
    assert all(r["label"] is None for r in data["result"])
    # Variables are valid UUID strings.
    for r in data["result"]:
        assert isinstance(r["variable"], str) and len(r["variable"]) == 36


def test_contributions_bad_measure_is_400(client, names_mapping):
    token = _paris_token(client)
    resp = client.post("/api/contributions",
                       json={"token": token, "measure": "nonsense"})
    assert resp.status_code == 400, resp.data
    assert "measure" in resp.get_json()["error"]


def test_contributions_bad_token_is_400(client):
    resp = client.post("/api/contributions", json={"token": "not-a-uuid"})
    assert resp.status_code == 400
    assert "token" in resp.get_json()["error"]


def test_leaf_row_is_in_table_column_order(client):
    """`/api/leaf` (which Contributions mode and the circuit inspector use to
    label inputs) must return the source row in table-column order, not the
    scrambled key order PostgreSQL's jsonb storage imposes on
    `resolve_input`'s `to_jsonb(t)`."""
    # A single-row select makes the row's provsql token an input gate.
    resp = client.post("/api/exec", json={
        "sql": "SELECT * FROM personnel WHERE name = 'Dave'",
        "mode": "contributions",
    })
    final = resp.get_json()["blocks"][-1]
    cols = [c["name"] for c in final["columns"]]
    token = final["rows"][0][cols.index("provsql")]

    resp = client.get(f"/api/leaf/{token}")
    assert resp.status_code == 200, resp.data
    row = resp.get_json()["matches"][0]["row"]
    # personnel is defined as (id, name, position, city, classification).
    assert list(row.keys()) == ["id", "name", "position", "city", "classification"]


def test_contributions_unknown_mapping_is_400(client, names_mapping):
    token = _paris_token(client)
    resp = client.post("/api/contributions", json={
        "token": token,
        "mapping": "provsql_test.does_not_exist",
    })
    assert resp.status_code == 400, resp.data
    assert "mapping" in resp.get_json()["error"]
