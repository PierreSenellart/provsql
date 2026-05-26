"""Security: provsql.tool_search_path is superuser-only (PGC_SUSET).

The GUC controls which directories the postgres OS user searches for
external-tool binaries, so it must not be settable by a non-superuser
(otherwise any role could redirect ProvSQL at an attacker-controlled
binary). Studio therefore has to degrade gracefully when it cannot set
it: the per-query set_config is swallowed so the user's query still
runs, and the Config-panel field is presented as admin-managed.

These tests need a superuser test connection (to create a throwaway
non-superuser role and to SET ROLE to it); they skip otherwise.
"""
from __future__ import annotations

import secrets

import pytest

from provsql_studio import db


def _is_superuser(pool) -> bool:
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT current_setting('is_superuser')::bool")
        return bool(cur.fetchone()[0])


def test_conn_reports_settable_for_superuser(client, app):
    pool = app.extensions["provsql_pool"]
    if not _is_superuser(pool):
        pytest.skip("test connection is not a superuser")
    info = client.get("/api/conn").get_json()
    assert info.get("tool_search_path_settable") is True


def test_apply_tool_search_path_applies_for_superuser(app):
    pool = app.extensions["provsql_pool"]
    if not _is_superuser(pool):
        pytest.skip("test connection is not a superuser")
    with pool.connection() as conn:
        with conn.cursor() as cur:
            assert db.apply_tool_search_path(cur, "/opt/d4") is True
            cur.execute("SHOW provsql.tool_search_path")
            assert cur.fetchone()[0] == "/opt/d4"
        conn.rollback()


def test_apply_tool_search_path_swallowed_for_non_superuser(app):
    # Simulate a non-superuser session via SET ROLE, then confirm the
    # helper degrades gracefully: it returns False, does not raise, and
    # leaves the transaction usable so the user's actual query still runs.
    pool = app.extensions["provsql_pool"]
    if not _is_superuser(pool):
        pytest.skip("need a superuser to create the throwaway role")
    role = f"studio_nosuper_{secrets.token_hex(4)}"
    with pool.connection() as conn:
        with conn.cursor() as cur:
            cur.execute(f'CREATE ROLE "{role}" NOSUPERUSER')
        conn.commit()
        try:
            with conn.cursor() as cur:
                cur.execute(f'SET ROLE "{role}"')
                applied = db.apply_tool_search_path(cur, "/tmp/evil")
                assert applied is False
                # The swallowed permission error left the transaction
                # usable (the savepoint was rolled back cleanly).
                cur.execute("SELECT 1")
                assert cur.fetchone()[0] == 1
            # SET ROLE was issued inside this transaction, so the rollback
            # restores the superuser identity for the cleanup below.
            conn.rollback()
        finally:
            with conn.cursor() as cur:
                cur.execute("RESET ROLE")
                cur.execute(f'DROP ROLE IF EXISTS "{role}"')
            conn.commit()
