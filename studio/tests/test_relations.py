"""Tests for GET /api/relations: discovery + content + live refresh after add_provenance."""
from __future__ import annotations


def test_schema_lists_personnel_with_columns(client):
    # /api/schema must include personnel (a SELECT-able table on the test
    # search_path) with its full column list, including the rewriter's
    # provsql tag, and exclude catalog / provsql-internal schemas.
    resp = client.get("/api/schema")
    assert resp.status_code == 200
    rows = resp.get_json()
    assert isinstance(rows, list) and rows, rows
    by_qname = {f"{r['schema']}.{r['table']}": r for r in rows}
    assert "provsql_test.personnel" in by_qname
    rel = by_qname["provsql_test.personnel"]
    assert rel["kind"] == "table"
    col_names = [c["name"] for c in rel["columns"]]
    assert col_names == ["id", "name", "position", "city", "classification", "provsql"]
    # The catalog and provsql schemas must be filtered out.
    schemas = {r["schema"] for r in rows}
    assert "pg_catalog" not in schemas
    assert "information_schema" not in schemas
    assert "provsql" not in schemas


def test_schema_marks_provenance_tracked_relations(client):
    """The schema endpoint flags relations carrying a `provsql uuid`
    column so the front-end can render the PROV pill. personnel is
    provenance-tracked by the conftest setup; pg_catalog tables are
    excluded outright, so we add a vanilla untracked table inline to
    check the negative case too."""
    setup = (
        "DROP TABLE IF EXISTS rel_test_plain;"
        " CREATE TABLE rel_test_plain (a int, b text)"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    try:
        rows = client.get("/api/schema").get_json()
        by_qname = {f"{r['schema']}.{r['table']}": r for r in rows}
        assert by_qname["provsql_test.personnel"]["has_provenance"] is True
        assert by_qname["provsql_test.rel_test_plain"]["has_provenance"] is False
    finally:
        client.post(
            "/api/exec",
            json={"sql": "DROP TABLE rel_test_plain", "mode": "circuit"},
        )


def test_schema_marks_views_propagating_provenance(client):
    """A view built on top of a provenance-tracked relation never carries
    a literal `provsql` column in pg_attribute, but ProvSQL's planner
    hook injects one into the rewritten output. The schema endpoint must
    still flag such views (CS2's `f` and `f_replicated` are the canonical
    case)."""
    setup = (
        "DROP VIEW IF EXISTS rel_test_view;"
        " DROP VIEW IF EXISTS rel_test_view_plain;"
        " DROP TABLE IF EXISTS rel_test_plain;"
        " CREATE TABLE rel_test_plain (a int, b text);"
        " CREATE VIEW rel_test_view        AS SELECT * FROM personnel;"
        " CREATE VIEW rel_test_view_plain  AS SELECT * FROM rel_test_plain;"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    try:
        rows = client.get("/api/schema").get_json()
        by_qname = {f"{r['schema']}.{r['table']}": r for r in rows}
        # View on a provenance-tracked table propagates provenance.
        assert by_qname["provsql_test.rel_test_view"]["has_provenance"] is True
        # View on a plain table does not.
        assert by_qname["provsql_test.rel_test_view_plain"]["has_provenance"] is False
    finally:
        client.post("/api/exec", json={
            "sql": (
                "DROP VIEW IF EXISTS rel_test_view;"
                " DROP VIEW IF EXISTS rel_test_view_plain;"
                " DROP TABLE IF EXISTS rel_test_plain;"
            ),
            "mode": "circuit",
        })


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
