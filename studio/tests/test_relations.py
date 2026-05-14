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


def test_schema_surfaces_provsql_column_types(client):
    """The schema endpoint must report the `random_variable` and
    `agg_token` types for columns that carry them, so the front-end can
    render the per-column RV / AGG pills in the schema panel. The
    fixture's search_path puts `provsql` before `public`, so
    format_type returns the unqualified form; we accept the qualified
    form too so the test stays robust to search_path drift."""
    setup = (
        "DROP TABLE IF EXISTS rel_test_provtypes;"
        " CREATE TABLE rel_test_provtypes ("
        "   id INT,"
        "   reading random_variable,"
        "   running agg_token"
        " );"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    try:
        rows = client.get("/api/schema").get_json()
        by_qname = {f"{r['schema']}.{r['table']}": r for r in rows}
        rel = by_qname["provsql_test.rel_test_provtypes"]
        col_types = {c["name"]: c["type"] for c in rel["columns"]}
        assert col_types["id"] == "integer"
        assert col_types["reading"] in ("random_variable", "provsql.random_variable")
        assert col_types["running"] in ("agg_token", "provsql.agg_token")
    finally:
        client.post(
            "/api/exec",
            json={"sql": "DROP TABLE rel_test_provtypes", "mode": "circuit"},
        )


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


def test_relations_truncation_when_table_exceeds_cap(test_dsn, tmp_path, monkeypatch):
    """When a relation has more rows than `max_sidebar_rows`, /api/relations
    must return exactly `max_sidebar_rows` rows, set truncated=True, and
    surface max_rows so the front-end can render the "showing N of ~T" hint.

    Personnel has 7 rows; we cap at 3 to force truncation."""
    from provsql_studio.app import create_app
    monkeypatch.setenv("PROVSQL_STUDIO_CONFIG_DIR", str(tmp_path / "studio_cfg"))
    app = create_app(
        dsn=f"{test_dsn} options='-c search_path=provsql_test,provsql,public'",
        max_sidebar_rows=3,
    )
    app.config.update(TESTING=True)
    client = app.test_client()
    relations = client.get("/api/relations").get_json()
    rel = next(r for r in relations if r["regclass"] == "personnel")
    assert rel["truncated"] is True
    assert rel["max_rows"] == 3
    assert len(rel["rows"]) == 3


def test_relations_no_truncation_when_under_cap(client):
    """Personnel has 7 rows; with the default cap (100) the response must
    not be flagged as truncated."""
    relations = client.get("/api/relations").get_json()
    rel = next(r for r in relations if r["regclass"] == "personnel")
    assert rel["truncated"] is False
    assert rel["max_rows"] == 100
    assert len(rel["rows"]) == 7


def test_relations_excludes_provenance_mappings(client):
    """Provenance-mapping-shaped relations -- tables with both `value` and
    `provenance uuid` columns -- play a different role from source data
    (they label input gates) and must not appear in the where-mode
    sidebar even when they happen to also carry a provsql column. The
    latter is the CTAS case: building a mapping with `CREATE TABLE foo
    AS SELECT x AS value, provenance() AS provenance FROM tracked` lets
    the planner hook inject a provsql column, which the older
    _RELATIONS_QUERY would have surfaced."""
    setup = (
        "DROP TABLE IF EXISTS rel_test_mapping;"
        # Mapping shape (value, provenance uuid) sourced from personnel
        # so the planner rewrite materializes a provsql column too.
        " CREATE TABLE rel_test_mapping AS"
        "   SELECT name AS value, provenance() AS provenance FROM personnel;"
        # And mark it tracked, just to be unambiguous about the case
        # under test (mapping-shape AND provsql column present).
        " SELECT add_provenance('rel_test_mapping'::regclass)"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    try:
        relations = client.get("/api/relations").get_json()
        names = {r["regclass"] for r in relations}
        assert "rel_test_mapping" not in names, (
            "mapping-shape relation leaked into the where-mode sidebar"
        )
        # Personnel itself is still listed -- the filter shouldn't be
        # over-broad.
        assert "personnel" in names
    finally:
        client.post(
            "/api/exec",
            json={"sql": "DROP TABLE rel_test_mapping", "mode": "circuit"},
        )


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
