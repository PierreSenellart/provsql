"""Tests for the connection-related endpoints introduced after Stage 3:
GET /api/conn, GET /api/databases, and POST /api/conn (database switch).

The switch test requires a second database the test user can connect to;
'postgres' (the maintenance DB) is always present and writable enough for a
read-only switch, so we use it as the destination.
"""
from __future__ import annotations

import psycopg


def test_conn_reports_user_and_test_database(client, test_dsn):
    info = client.get("/api/conn").get_json()
    # current_user matches whoever ran the test (psycopg picks up PG* env vars).
    assert info["user"]
    # Database matches the per-session test database (or whatever DSN override).
    expected = _dbname_of(test_dsn)
    assert info["database"] == expected
    # Host is None for Unix-socket connections; otherwise a string.
    assert info["host"] is None or isinstance(info["host"], str)


def test_databases_lists_postgres_and_test_db(client, test_dsn):
    dbs = client.get("/api/databases").get_json()
    assert isinstance(dbs, list)
    assert all(isinstance(s, str) for s in dbs)
    assert "postgres" in dbs
    assert _dbname_of(test_dsn) in dbs
    # Template DBs must not appear.
    assert "template0" not in dbs
    assert "template1" not in dbs


def test_conn_switch_to_postgres_round_trip(client, test_dsn):
    # Initial connection points at the test database.
    initial = client.get("/api/conn").get_json()["database"]
    assert initial == _dbname_of(test_dsn)

    # Switch to 'postgres'.
    resp = client.post("/api/conn", json={"database": "postgres"})
    assert resp.status_code == 200, resp.data
    after = resp.get_json()
    assert after["database"] == "postgres"

    # Subsequent reads use the new pool.
    assert client.get("/api/conn").get_json()["database"] == "postgres"
    # 'postgres' has no provenance-tagged relations.
    assert client.get("/api/relations").get_json() == []

    # Switch back so any later test using `client` sees the test DB.
    resp = client.post("/api/conn", json={"database": initial})
    assert resp.status_code == 200
    assert client.get("/api/conn").get_json()["database"] == initial


def test_conn_switch_rejects_missing_database_arg(client):
    resp = client.post("/api/conn", json={})
    assert resp.status_code == 400


def test_conn_switch_rejects_inaccessible_database(client):
    # A database name almost certainly absent from the cluster — the server
    # must refuse without tearing the existing pool down.
    resp = client.post("/api/conn", json={"database": "no_such_db_xyzzy_12345"})
    assert resp.status_code == 403


# ──────── helpers ────────


def _dbname_of(dsn: str) -> str:
    """Extract the dbname from a libpq-style DSN string."""
    return psycopg.conninfo.conninfo_to_dict(dsn)["dbname"]
