"""Tests for POST /api/install-provsql and the missing-extension detection
that backs the Notebook-mode "ProvSQL is not installed" banner.

A notebook may be opened against an arbitrary database that does not yet
have the extension; /api/conn reports `extension_version = None` there, and
the install endpoint runs `CREATE EXTENSION IF NOT EXISTS provsql CASCADE`
so the user can make the database ready in one click.
"""
from __future__ import annotations

import os
import secrets

import psycopg
import pytest

from provsql_studio.app import create_app


@pytest.fixture()
def no_provsql_dsn():
    """A fresh database with NO extension installed, so the missing-provsql
    path is exercised. Mirrors the `cs8_dsn` lifecycle but deliberately
    skips the `CREATE EXTENSION`. Function-scoped: the install test mutates
    it, so each test gets a pristine database."""
    override = os.environ.get("PROVSQL_STUDIO_NO_PROVSQL_DSN")
    if override:
        yield override
        return

    suffix = secrets.token_hex(4)
    db_name = f"provsql_studio_bare_{suffix}"
    admin_dsn = "dbname=postgres"
    with psycopg.connect(admin_dsn, autocommit=True) as admin:
        admin.execute(f'CREATE DATABASE "{db_name}"')
    try:
        yield f"dbname={db_name}"
    finally:
        with psycopg.connect(admin_dsn, autocommit=True) as admin:
            admin.execute(
                "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                "WHERE datname = %s AND pid <> pg_backend_pid()",
                (db_name,),
            )
            admin.execute(f'DROP DATABASE IF EXISTS "{db_name}"')


@pytest.fixture()
def no_provsql_client(no_provsql_dsn, tmp_path, monkeypatch):
    """A Studio client bound to the bare (provsql-less) database. Same
    config-dir redirect and pool teardown as the `app` fixture."""
    monkeypatch.setenv("PROVSQL_STUDIO_CONFIG_DIR", str(tmp_path / "studio_cfg"))
    app = create_app(dsn=f"{no_provsql_dsn} options='-c search_path=public,provsql'")
    app.config.update(TESTING=True)
    try:
        yield app.test_client()
    finally:
        pool = app.extensions.get("provsql_pool")
        if pool is not None:
            try:
                pool.close()
            except Exception:
                pass


def test_conn_reports_provsql_missing(no_provsql_client):
    """On a database without the extension, /api/conn reports a null
    version : this is exactly the signal the Notebook banner keys on."""
    info = no_provsql_client.get("/api/conn").get_json()
    assert info["extension_version"] is None


def test_install_provsql_full_cycle(no_provsql_client):
    """Detect missing, install, then the version surfaces; a second install
    is an idempotent no-op returning the same version."""
    assert no_provsql_client.get("/api/conn").get_json()["extension_version"] is None

    resp = no_provsql_client.post("/api/install-provsql")
    assert resp.status_code == 200, resp.data
    payload = resp.get_json()
    assert payload["ok"] is True
    version = payload["version"]
    assert isinstance(version, str) and version

    # /api/conn now agrees the extension is present at that version.
    assert no_provsql_client.get("/api/conn").get_json()["extension_version"] == version

    # Idempotent: installing again is a no-op, same version, still 200.
    again = no_provsql_client.post("/api/install-provsql")
    assert again.status_code == 200
    assert again.get_json()["version"] == version


def test_install_provsql_idempotent_when_present(client):
    """On the standard (provsql-installed) test database, the endpoint is a
    harmless no-op that returns the already-installed version."""
    installed = client.get("/api/conn").get_json()["extension_version"]
    assert installed  # the test DB has provsql

    resp = client.post("/api/install-provsql")
    assert resp.status_code == 200, resp.data
    payload = resp.get_json()
    assert payload["ok"] is True
    assert payload["version"] == installed
