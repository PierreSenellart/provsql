"""Tests for GET /api/relations: discovery + content + live refresh after add_provenance."""
from __future__ import annotations


def test_personnel_listed(client):
    resp = client.get("/api/relations")
    assert resp.status_code == 200
    relations = resp.get_json()
    by_name = {r["regclass"]: r for r in relations}
    assert "personnel" in by_name
    rel = by_name["personnel"]

    col_names = [c["name"] for c in rel["columns"]]
    # SELECT * column order matches the table's pg_attribute order with
    # the rewriter-injected provsql trailing.
    assert col_names == ["id", "name", "position", "city", "classification", "provsql"]
    assert len(rel["rows"]) == 7
    assert any(r["values"][1] == "John" for r in rel["rows"])


def test_each_row_has_provenance_uuid(client):
    relations = client.get("/api/relations").get_json()
    rel = next(r for r in relations if r["regclass"] == "personnel")
    for row in rel["rows"]:
        # uuid is a 36-char hyphenated string.
        assert isinstance(row["uuid"], str) and len(row["uuid"]) == 36
        # The provsql cell value matches the row's uuid identifier.
        assert row["values"][rel["prov_col"]] == row["uuid"]


def test_add_provenance_picks_up_new_relation(client, app):
    # Run add_provenance via /api/exec, then re-query relations and check
    # the new table shows up. Use a unique name so we don't clash with other tests.
    table = "rel_test_widget"

    # Drop any prior leftover and create the new table + provenance, all in one batch.
    setup_sql = (
        f"DROP TABLE IF EXISTS {table};"
        f" CREATE TABLE {table} (id INT, label TEXT);"
        f" INSERT INTO {table} VALUES (1, 'a'), (2, 'b');"
        f" SELECT add_provenance('{table}'::regclass)"
    )
    resp = client.post("/api/exec", json={"sql": setup_sql, "mode": "circuit"})
    assert resp.status_code == 200, resp.data

    try:
        relations = client.get("/api/relations").get_json()
        names = {r["regclass"] for r in relations}
        assert table in names
        rel = next(r for r in relations if r["regclass"] == table)
        col_names = [c["name"] for c in rel["columns"]]
        assert col_names == ["id", "label", "provsql"]
        assert len(rel["rows"]) == 2
    finally:
        client.post(
            "/api/exec",
            json={"sql": f"DROP TABLE {table}", "mode": "circuit"},
        )
