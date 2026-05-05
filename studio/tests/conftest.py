"""Test harness for ProvSQL Studio.

A session-scoped fixture spins up an isolated PostgreSQL database, installs
the provsql extension, and runs the upstream `setup` + `add_provenance` test
SQL files so each test starts with the same `personnel` table the regression
suite uses.

The DSN can be overridden with the `PROVSQL_STUDIO_TEST_DSN` env var (CI use)
; in that case the harness assumes the database is already prepared and only
verifies the personnel table exists.
"""
from __future__ import annotations

import os
import secrets
from pathlib import Path

import psycopg
import pytest

from provsql_studio.app import create_app


REPO_ROOT = Path(__file__).resolve().parents[2]
SETUP_SQL_FILES = [
    REPO_ROOT / "test" / "sql" / "setup.sql",
    REPO_ROOT / "test" / "sql" / "add_provenance.sql",
]


def _read_setup_sql(path: Path) -> str:
    """Strip pg_regress meta-commands (\\set, \\pset) so we can run the file
    directly via psycopg without psql. Both setup files start with a couple
    of `\\` lines for output formatting; the rest is plain SQL."""
    out_lines = []
    for line in path.read_text().splitlines():
        if line.startswith("\\"):
            continue
        out_lines.append(line)
    return "\n".join(out_lines)


@pytest.fixture(scope="session")
def test_dsn() -> str:
    """Either reuse PROVSQL_STUDIO_TEST_DSN or create a fresh one-off DB."""
    override = os.environ.get("PROVSQL_STUDIO_TEST_DSN")
    if override:
        # Caller is responsible for the schema. Verify minimally.
        with psycopg.connect(override) as conn, conn.cursor() as cur:
            cur.execute("SELECT 1 FROM pg_extension WHERE extname='provsql'")
            assert cur.fetchone(), (
                "PROVSQL_STUDIO_TEST_DSN points to a database without "
                "the provsql extension. Install it before running tests."
            )
        yield override
        return

    # Create a unique database keyed on a random suffix so parallel runs don't
    # collide. We connect to the maintenance database `postgres`.
    suffix = secrets.token_hex(4)
    db_name = f"provsql_studio_test_{suffix}"
    admin_dsn = "dbname=postgres"

    with psycopg.connect(admin_dsn, autocommit=True) as admin:
        admin.execute(f'CREATE DATABASE "{db_name}"')

    try:
        target_dsn = f"dbname={db_name}"
        # setup.sql installs the extension itself (CREATE EXTENSION CASCADE,
        # then a drop/recreate cycle). We just feed it the file contents.
        with psycopg.connect(target_dsn, autocommit=True) as conn:
            for sqlfile in SETUP_SQL_FILES:
                conn.execute(_read_setup_sql(sqlfile))
        yield target_dsn
    finally:
        with psycopg.connect(admin_dsn, autocommit=True) as admin:
            # Forcibly drop any leftover connections so DROP doesn't block.
            admin.execute(
                "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                "WHERE datname = %s AND pid <> pg_backend_pid()",
                (db_name,),
            )
            admin.execute(f'DROP DATABASE IF EXISTS "{db_name}"')


@pytest.fixture()
def app(test_dsn: str, tmp_path, monkeypatch):
    """Per-test Flask app bound to the test DSN, with the schema search_path
    pre-set so unqualified `personnel` references resolve.

    Also redirects Studio's on-disk config (used by /api/config persistence)
    into a per-test tmp dir so tests can't read or write the user's real
    ~/.config/provsql-studio/config.json. The env var must be in place
    before `create_app()` runs because the factory eagerly loads any
    persisted GUC overrides into RUNTIME_GUCS, so we set it here rather
    than in an autouse fixture (whose ordering relative to `app` is
    fragile)."""
    monkeypatch.setenv("PROVSQL_STUDIO_CONFIG_DIR", str(tmp_path / "studio_cfg"))
    app = create_app(dsn=f"{test_dsn} options='-c search_path=provsql_test,provsql,public'")
    app.config.update(TESTING=True)
    yield app


@pytest.fixture()
def client(app):
    return app.test_client()
