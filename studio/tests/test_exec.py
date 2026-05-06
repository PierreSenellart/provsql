"""Tests for POST /api/exec: multi-statement splitting, where-mode wrapping,
status / error handling. The wrapping is exercised against the personnel
table set up by conftest."""
from __future__ import annotations

import pytest


def post_exec(client, sql, mode="where"):
    resp = client.post("/api/exec", json={"sql": sql, "mode": mode})
    assert resp.status_code == 200, resp.data
    return resp.get_json()


# ──────── single-statement happy paths ────────


def test_select_returns_rows(client):
    payload = post_exec(client, "SELECT name FROM personnel WHERE name = 'John'", mode="where")
    final = payload["blocks"][-1]
    assert final["kind"] == "rows"
    # Where-mode wraps the SELECT, so __prov / __wprov / provsql columns
    # appear in addition to the user's name column.
    names = {c["name"] for c in final["columns"]}
    assert "name" in names
    assert "__prov" in names and "__wprov" in names
    assert payload["wrapped"] is True
    assert any(row[0] == "John" for row in final["rows"])


def test_circuit_mode_does_not_wrap(client):
    payload = post_exec(client, "SELECT name FROM personnel WHERE name = 'John'", mode="circuit")
    final = payload["blocks"][-1]
    assert final["kind"] == "rows"
    assert payload["wrapped"] is False
    names = {c["name"] for c in final["columns"]}
    assert "__prov" not in names
    assert "__wprov" not in names


def test_create_returns_status(client):
    # CREATE TEMP TABLE is session-scoped; using a unique name avoids cross-test interference.
    payload = post_exec(
        client,
        "CREATE TEMPORARY TABLE t_status_demo (x int)",
        mode="where",
    )
    final = payload["blocks"][-1]
    assert final["kind"] == "status"
    assert "CREATE TABLE" in final["message"]


def test_syntax_error_returns_error_block(client):
    payload = post_exec(client, "SELEKT 1", mode="where")
    final = payload["blocks"][-1]
    assert final["kind"] == "error"
    assert final.get("sqlstate") == "42601"  # syntax_error
    assert "syntax" in final["message"].lower()


# ──────── multi-statement behaviour ────────


def test_multi_statement_shows_only_last(client):
    # Earlier successful statements (CREATE) are silently discarded; only
    # the final SELECT is rendered.
    sql = (
        "CREATE TEMPORARY TABLE t_multi_demo (x int);"
        " INSERT INTO t_multi_demo VALUES (1),(2),(3);"
        " SELECT x FROM t_multi_demo ORDER BY x"
    )
    payload = post_exec(client, sql, mode="circuit")
    assert len(payload["blocks"]) == 1
    final = payload["blocks"][0]
    assert final["kind"] == "rows"
    assert [r[0] for r in final["rows"]] == [1, 2, 3]


def test_syntax_error_in_non_final_statement_halts_batch(client):
    sql = (
        "CREATE TEMPORARY TABLE t_halt_demo (x int);"
        " SELEKT 1;"
        " SELECT 2"
    )
    payload = post_exec(client, sql, mode="circuit")
    # The error in the middle statement halts the batch and is the only block returned.
    assert len(payload["blocks"]) == 1
    assert payload["blocks"][0]["kind"] == "error"
    assert "syntax" in payload["blocks"][0]["message"].lower()


def test_dml_returning_last_runs_unwrapped(client):
    # A DML last statement must NOT be wrapped: wrapping turns it into a SELECT,
    # which would lose the side effects. The where-mode regex only matches WITH/SELECT.
    sql = (
        "CREATE TEMPORARY TABLE t_dml_demo (x int);"
        " INSERT INTO t_dml_demo VALUES (10),(20)"
    )
    payload = post_exec(client, sql, mode="where")
    final = payload["blocks"][-1]
    assert final["kind"] == "status"
    assert payload["wrapped"] is False


def test_with_select_is_wrappable(client):
    sql = (
        "WITH paris AS (SELECT name FROM personnel WHERE city = 'Paris') "
        "SELECT name FROM paris ORDER BY name"
    )
    payload = post_exec(client, sql, mode="where")
    assert payload["wrapped"] is True
    final = payload["blocks"][-1]
    assert final["kind"] == "rows"
    names = {c["name"] for c in final["columns"]}
    assert "__prov" in names and "__wprov" in names
    assert sorted(r[0] for r in final["rows"]) == ["Dave", "Magdalen", "Nancy"]


def test_dollar_quoted_body_does_not_confuse_splitter(client):
    # A dollar-quoted function body contains semicolons that would break a
    # naive split. sqlparse handles this correctly.
    sql = (
        "DO $$ BEGIN PERFORM 1; PERFORM 2; END $$;"
        " SELECT 42 AS n"
    )
    payload = post_exec(client, sql, mode="circuit")
    assert len(payload["blocks"]) == 1
    final = payload["blocks"][0]
    assert final["kind"] == "rows"
    assert final["rows"][0][0] == 42


# ──────── where-mode wrap fallback for non-tracked relations ────────


def test_update_provenance_toggle_propagates_to_guc(client):
    # In circuit mode the front-end is free to flip update_provenance on or
    # off via the body of /api/exec. The wire must reach SET LOCAL: SHOW the
    # GUC inside the same batch and assert the value.
    payload = post_exec(
        client,
        "SHOW provsql.update_provenance",
        mode="circuit",
    )
    final = payload["blocks"][-1]
    # Default off: nothing in the payload toggles it on, so SHOW returns 'off'.
    assert final["kind"] == "rows"
    assert final["rows"][0][0] == "off"

    resp = client.post(
        "/api/exec",
        json={
            "sql": "SHOW provsql.update_provenance",
            "mode": "circuit",
            "update_provenance": True,
        },
    )
    assert resp.status_code == 200
    final = resp.get_json()["blocks"][-1]
    assert final["kind"] == "rows"
    assert final["rows"][0][0] == "on"


# ──────── statement timeout ────────


def test_cancel_endpoint_aborts_in_flight_query(app, client):
    # Bump the timeout high enough that pg_sleep can't naturally hit it,
    # then run the slow query on a background thread while the main
    # thread fires POST /api/cancel/<id>. The cancel must reach the
    # backend (via pg_cancel_backend on a fresh connection) and abort
    # the in-flight query as a 57014, not let it run to completion.
    import threading
    import time
    import uuid as uuid_mod

    app.config["STATEMENT_TIMEOUT"] = "30s"
    request_id = str(uuid_mod.uuid4())
    result_holder: dict = {}

    def run_slow():
        c = app.test_client()
        resp = c.post(
            "/api/exec",
            json={
                "sql": "SELECT pg_sleep(5)",
                "mode": "circuit",
                "request_id": request_id,
            },
        )
        result_holder["status"] = resp.status_code
        result_holder["payload"] = resp.get_json()

    th = threading.Thread(target=run_slow, daemon=True)
    th.start()

    # Wait until the registry has the pid (i.e. exec_batch has started
    # and registered via on_pid). Bounded loop so a regression that
    # never registers fails fast rather than hanging the suite.
    inflight = app.extensions["provsql_inflight"]
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        with inflight["lock"]:
            if request_id in inflight["by_id"]:
                break
        time.sleep(0.02)
    else:
        th.join(timeout=10)
        raise AssertionError(
            "exec_batch never registered a pid for the request id"
        )

    cancel_resp = client.post(f"/api/cancel/{request_id}")
    assert cancel_resp.status_code == 200, cancel_resp.data
    assert cancel_resp.get_json()["ok"] is True

    th.join(timeout=10)
    assert not th.is_alive(), "slow query did not return after cancel"
    assert result_holder["status"] == 200
    final = result_holder["payload"]["blocks"][-1]
    assert final["kind"] == "error"
    assert final["sqlstate"] == "57014"


def test_cancel_endpoint_404s_when_no_query_in_flight(client):
    resp = client.post("/api/cancel/00000000-0000-0000-0000-000000000000")
    assert resp.status_code == 404
    assert resp.get_json()["ok"] is False


def test_statement_timeout_returns_clear_error_and_drops_pg_notices(app, client):
    # Force a tight per-statement timeout, then run a query guaranteed to
    # exceed it. The error must be the Studio-styled timeout message (not
    # the bare PG "canceling statement" text), carry sqlstate 57014, and
    # the response must contain no notices: any PG NOTICE buffered before
    # the cancel could have been truncated mid-message and showing it
    # alongside the timeout would mislead.
    app.config["STATEMENT_TIMEOUT"] = "100ms"
    payload = post_exec(client, "SELECT pg_sleep(2)", mode="circuit")
    final = payload["blocks"][-1]
    assert final["kind"] == "error"
    assert final["sqlstate"] == "57014"
    assert "statement timeout" in final["message"].lower()
    assert "100ms" in final["message"]
    assert payload["notices"] == []


def test_where_mode_falls_back_when_no_provenance_relation(client):
    # SELECT against a table that has no provsql column. The wrap would
    # otherwise raise "provenance() called on a table without provenance";
    # /api/exec must roll the savepoint back, retry unwrapped, and surface
    # an info notice instead of an error block.
    sql = (
        "CREATE TEMPORARY TABLE t_untagged (id int, label text);"
        " INSERT INTO t_untagged VALUES (1, 'x'), (2, 'y');"
        " SELECT * FROM t_untagged ORDER BY id"
    )
    payload = post_exec(client, sql, mode="where")
    assert payload["wrapped"] is False
    # Wrap-fallback rides in `notices` with severity INFO (uniform path
    # with server-side NOTICE / WARNING messages).
    info_notices = [n for n in payload["notices"] if n["severity"] == "INFO"]
    assert info_notices, payload["notices"]
    assert "not provenance-tracked" in info_notices[0]["message"].lower()
    final = payload["blocks"][-1]
    assert final["kind"] == "rows"
    assert [r[0] for r in final["rows"]] == [1, 2]
    # No __prov / __wprov columns since the wrap was dropped.
    names = {c["name"] for c in final["columns"]}
    assert "__prov" not in names
    assert "__wprov" not in names


# ──────── auto-prepare guard (regression for psycopg3 prepare_threshold) ────────


def test_where_provenance_toggle_survives_auto_prepare(client):
    """psycopg3 auto-prepares a query after `prepare_threshold` (default 5)
    executions, freezing whatever planner-hook decisions the FIRST plan
    made — including the gates produced for `provsql.where_provenance`.
    The pool's `_configure` disables auto-prepare so SET LOCAL toggles
    keep reaching the planner. This test runs the same SELECT eleven
    times (well past the default threshold), alternating wp on/off, and
    verifies the resulting provsql UUID swings between two values.

    Without the prepare_threshold=None override, every iteration past
    the 5th would return the wp=on UUID regardless of the toggle."""
    sql = "SELECT name FROM personnel WHERE name = 'John'"

    def root_uuid(wp):
        resp = client.post("/api/exec", json={
            "sql": sql,
            "mode": "circuit",
            "where_provenance": wp,
        })
        assert resp.status_code == 200, resp.data
        final = resp.get_json()["blocks"][-1]
        cols = [c["name"] for c in final["columns"]]
        return final["rows"][0][cols.index("provsql")]

    on_uuid  = root_uuid(True)
    off_uuid = root_uuid(False)
    # Sanity: the rewriter MUST emit different gates for the two wp
    # settings (where_provenance=on wraps the input gate in project/eq
    # subgraphs, changing the root UUID).
    assert on_uuid != off_uuid

    # Now hammer the query through the prepare threshold and back.
    for _ in range(8):
        assert root_uuid(True)  == on_uuid
        assert root_uuid(False) == off_uuid
