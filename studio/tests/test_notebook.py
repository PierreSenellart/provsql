"""Tests for the notebook-kernel API: /api/nb/session lifecycle and
/api/nb/exec cell execution on a pinned, stateful connection.

The defining property under test is Jupyter-like session state: a temp
table created in one cell is visible in the next, an error cell rolls
back without killing the kernel, and a restart (delete + create) wipes
the state. See doc/TODO/studio-notebook-mode.md §2.
"""
from __future__ import annotations


def _new_session(client):
    resp = client.post("/api/nb/session")
    assert resp.status_code == 200, resp.data
    payload = resp.get_json()
    assert payload["session_id"]
    assert payload["pid"] > 0
    return payload["session_id"]


def _run_cell(client, session_id, sql, **kw):
    resp = client.post("/api/nb/exec",
                       json={"session_id": session_id, "sql": sql, **kw})
    assert resp.status_code == 200, resp.data
    return resp.get_json()


# ──────── session lifecycle ────────


def test_session_create_status_delete(client):
    sid = _new_session(client)
    st = client.get(f"/api/nb/session/{sid}/status").get_json()
    assert st["alive"] is True
    assert st["pid"] > 0
    assert st["idle_seconds"] >= 0

    assert client.delete(f"/api/nb/session/{sid}").status_code == 204
    assert client.get(f"/api/nb/session/{sid}/status").status_code == 404
    # Idempotence: deleting again is a clean 404, not a 500.
    assert client.delete(f"/api/nb/session/{sid}").status_code == 404


def test_close_alias_for_beacon(client):
    """navigator.sendBeacon can only POST; /close mirrors DELETE."""
    sid = _new_session(client)
    assert client.post(f"/api/nb/session/{sid}/close").status_code == 204
    assert client.get(f"/api/nb/session/{sid}/status").status_code == 404


def test_exec_on_unknown_session_flags_kernel_dead(client):
    resp = client.post("/api/nb/exec",
                       json={"session_id": "nope", "sql": "SELECT 1"})
    assert resp.status_code == 404
    assert resp.get_json()["kernel_dead"] is True


def test_kernel_cap(client, app):
    app.config["MAX_KERNELS"] = 2
    sids = [_new_session(client) for _ in range(2)]
    resp = client.post("/api/nb/session")
    assert resp.status_code == 429
    for sid in sids:
        client.delete(f"/api/nb/session/{sid}")
    # Room again after closing one.
    sid = _new_session(client)
    client.delete(f"/api/nb/session/{sid}")


def test_idle_timeout_reaps_kernel(client, app):
    sid = _new_session(client)
    # Backdate last_used past the timeout, then trigger the lazy GC
    # via any kernel-touching request.
    app.config["KERNEL_IDLE_TIMEOUT"] = 0.0
    resp = client.post("/api/nb/exec",
                       json={"session_id": sid, "sql": "SELECT 1"})
    assert resp.status_code == 404
    assert resp.get_json()["kernel_dead"] is True


# ──────── stateful cell execution ────────


def test_temp_table_persists_across_cells(client):
    """The whole point of the kernel: session state survives cells."""
    sid = _new_session(client)
    out = _run_cell(client, sid, "CREATE TEMP TABLE nb_state(x int)")
    assert out["blocks"][-1]["kind"] == "status"
    assert out["kernel_dead"] is False

    _run_cell(client, sid, "INSERT INTO nb_state VALUES (1), (2)")
    out = _run_cell(client, sid, "SELECT count(*) AS n FROM nb_state")
    final = out["blocks"][-1]
    assert final["kind"] == "rows", final
    assert final["rows"][0][0] == 2
    client.delete(f"/api/nb/session/{sid}")


def test_plain_set_persists_across_cells(client):
    """A user-level SET (not SET LOCAL) sticks for the kernel's life."""
    sid = _new_session(client)
    _run_cell(client, sid, "SET application_name = 'nb-kernel-test'")
    out = _run_cell(client, sid, "SHOW application_name")
    assert out["blocks"][-1]["rows"][0][0] == "nb-kernel-test"
    client.delete(f"/api/nb/session/{sid}")


def test_error_cell_rolls_back_but_kernel_survives(client):
    sid = _new_session(client)
    _run_cell(client, sid, "CREATE TEMP TABLE nb_rb(x int)")
    # The INSERT in this failing cell must roll back with the cell.
    out = _run_cell(client, sid,
                    "INSERT INTO nb_rb VALUES (1);\nSELECT 1/0")
    assert out["blocks"][-1]["kind"] == "error"
    assert out["kernel_dead"] is False

    out = _run_cell(client, sid, "SELECT count(*) AS n FROM nb_rb")
    assert out["blocks"][-1]["rows"][0][0] == 0
    client.delete(f"/api/nb/session/{sid}")


def test_restart_clears_state(client):
    """Restart = delete + create: temp tables are gone on the new kernel."""
    sid = _new_session(client)
    _run_cell(client, sid, "CREATE TEMP TABLE nb_restart(x int)")
    client.delete(f"/api/nb/session/{sid}")

    sid2 = _new_session(client)
    out = _run_cell(client, sid2, "SELECT * FROM nb_restart")
    final = out["blocks"][-1]
    assert final["kind"] == "error"
    assert final.get("sqlstate") == "42P01"  # undefined_table
    client.delete(f"/api/nb/session/{sid2}")


def test_copy_from_stdin_works_in_cell(client):
    """The dump-style COPY path goes through exec_batch_on unchanged."""
    sid = _new_session(client)
    out = _run_cell(client, sid, (
        "CREATE TEMP TABLE nb_copy(id int, name text);\n"
        "COPY nb_copy (id, name) FROM stdin;\n"
        "1\tAlice\n"
        "2\tBob\n"
        "\\.\n"
        "SELECT count(*) AS n FROM nb_copy"
    ))
    assert out["blocks"][-1]["rows"][0][0] == 2
    client.delete(f"/api/nb/session/{sid}")


def test_wedged_connection_kills_kernel_cleanly(client):
    """COPY TO STDOUT wedges the connection mid-protocol: the cell must
    come back as a clean error with kernel_dead=True (no 500), and the
    session must be gone."""
    sid = _new_session(client)
    out = _run_cell(client, sid, "COPY personnel TO STDOUT")
    assert out["blocks"][-1]["kind"] == "error"
    assert out["kernel_dead"] is True
    assert client.get(f"/api/nb/session/{sid}/status").status_code == 404


def test_where_scheme_wraps_final_select(client):
    """Per-cell provenance scheme: where adds __prov / __wprov."""
    sid = _new_session(client)
    out = _run_cell(client, sid,
                    "SELECT name FROM personnel WHERE name = 'John'",
                    prov_scheme="where")
    final = out["blocks"][-1]
    names = {c["name"] for c in final["columns"]}
    assert "__prov" in names and "__wprov" in names
    assert out["wrapped"] is True

    out = _run_cell(client, sid,
                    "SELECT name FROM personnel WHERE name = 'John'")
    names = {c["name"] for c in out["blocks"][-1]["columns"]}
    assert "__prov" not in names
    client.delete(f"/api/nb/session/{sid}")


def test_empty_cell_is_a_noop(client):
    sid = _new_session(client)
    out = _run_cell(client, sid, "   \n")
    assert out["blocks"] == []
    assert out["kernel_dead"] is False
    client.delete(f"/api/nb/session/{sid}")


def test_connection_switch_drops_kernels(client, app):
    """Swapping the database invalidates every kernel (they are pinned
    to the old DSN)."""
    sid = _new_session(client)
    current_db = client.get("/api/conn").get_json()["database"]
    resp = client.post("/api/conn", json={"database": current_db})
    assert resp.status_code == 200, resp.data
    assert client.get(f"/api/nb/session/{sid}/status").status_code == 404


# ──────── database creation (notebook binding "create" action) ────────


def test_create_database_endpoint(client, test_dsn):
    """POST /api/databases creates the database with provsql installed
    (the binding banner's create action); duplicates 409, bad names 400."""
    import psycopg
    name = "provsql_nb_scratch_test"
    admin = "dbname=postgres"
    try:
        resp = client.post("/api/databases", json={"name": name})
        assert resp.status_code == 200, resp.data
        payload = resp.get_json()
        assert payload["ok"] is True and payload["database"] == name
        # provsql was installed in the new database (warning is None).
        assert payload["warning"] is None, payload["warning"]
        params = psycopg.conninfo.conninfo_to_dict(test_dsn)
        params["dbname"] = name
        with psycopg.connect(psycopg.conninfo.make_conninfo(**params)) as conn:
            row = conn.execute(
                "SELECT 1 FROM pg_extension WHERE extname='provsql'").fetchone()
            assert row is not None

        assert client.post("/api/databases",
                           json={"name": name}).status_code == 409
        assert client.post("/api/databases",
                           json={"name": "no;injection"}).status_code == 400
        assert client.post("/api/databases",
                           json={"name": ""}).status_code == 400
    finally:
        with psycopg.connect(admin, autocommit=True) as conn:
            conn.execute(f'DROP DATABASE IF EXISTS "{name}"')


# ──────── bundled example notebooks ────────


def test_examples_list_and_fetch(client):
    """The generated tutorial / case-study notebooks are listed with
    their titles and database bindings, and fetchable by name."""
    resp = client.get("/api/nb/examples")
    assert resp.status_code == 200
    names = [e["name"] for e in resp.get_json()]
    examples = {e["name"]: e for e in resp.get_json()}
    assert "tutorial" in examples and "cs1" in examples
    # The tutorial leads the list; case studies follow.
    assert names[0] == "tutorial" and names[1:] == sorted(names[1:])
    assert examples["tutorial"]["database"] == "tutorial"
    assert "Daphine" in examples["tutorial"]["title"]

    resp = client.get("/api/nb/examples/tutorial")
    assert resp.status_code == 200
    doc = resp.get_json()
    assert doc["nbformat"] == 4
    assert doc["metadata"]["provsql"]["database"] == "tutorial"
    kinds = {c["cell_type"] for c in doc["cells"]}
    assert kinds == {"markdown", "code"}
    # The setup splice carried the COPY data blocks along.
    assert any("FROM stdin;" in "".join(c["source"])
               for c in doc["cells"] if c["cell_type"] == "code")

    assert client.get("/api/nb/examples/nope").status_code == 404
    assert client.get("/api/nb/examples/..%2Fetc").status_code in (400, 404)


# ──────── empty-database action ────────


def test_empty_database_endpoint(test_dsn, tmp_path, monkeypatch):
    """POST /api/database/empty drops every user schema and recreates a
    blank public; the provsql extension survives. Run against a scratch
    database so the shared fixture is untouched."""
    import psycopg
    from provsql_studio.app import create_app

    name = "provsql_nb_empty_test"
    admin = "dbname=postgres"
    params = psycopg.conninfo.conninfo_to_dict(test_dsn)
    params["dbname"] = name
    scratch_dsn = psycopg.conninfo.make_conninfo(**params)
    with psycopg.connect(admin, autocommit=True) as conn:
        conn.execute(f'DROP DATABASE IF EXISTS "{name}"')
        conn.execute(f'CREATE DATABASE "{name}"')
    app = None
    try:
        with psycopg.connect(scratch_dsn, autocommit=True) as conn:
            conn.execute("CREATE EXTENSION provsql CASCADE")
            conn.execute("CREATE TABLE t(x int)")
            conn.execute("CREATE SCHEMA other")
            conn.execute("CREATE TABLE other.u(y int)")

        monkeypatch.setenv("PROVSQL_STUDIO_CONFIG_DIR", str(tmp_path / "cfg"))
        app = create_app(dsn=scratch_dsn)
        app.config.update(TESTING=True)
        client = app.test_client()

        resp = client.post("/api/database/empty")
        assert resp.status_code == 200, resp.data
        dropped = resp.get_json()["dropped_schemas"]
        assert "public" in dropped and "other" in dropped

        with psycopg.connect(scratch_dsn) as conn:
            assert conn.execute(
                "SELECT 1 FROM pg_extension WHERE extname='provsql'"
            ).fetchone()
            assert conn.execute(
                "SELECT to_regclass('public.t')").fetchone()[0] is None
            assert conn.execute(
                "SELECT count(*) FROM pg_namespace WHERE nspname='other'"
            ).fetchone()[0] == 0
            # public exists and is usable
            conn.execute("CREATE TABLE recheck(x int)")
    finally:
        if app is not None:
            app.extensions["provsql_kernels"]["close_all"]()
            app.extensions["provsql_pool"].close()
        with psycopg.connect(admin, autocommit=True) as conn:
            conn.execute(f'DROP DATABASE IF EXISTS "{name}"')


def test_tutorial_notebook_run_all_twice(test_dsn, tmp_path, monkeypatch):
    """The generated notebooks are idempotent: every SQL cell of the
    tutorial runs cleanly TWICE in a row on a scratch database (the
    setup guards, the NOTICE-only add_provenance / mapping re-runs, and
    the DROP-IF-EXISTS narrative blocks all compose)."""
    import json
    from pathlib import Path

    import psycopg
    from provsql_studio.app import create_app

    name = "provsql_nb_idem_tutorial"
    admin = "dbname=postgres"
    params = psycopg.conninfo.conninfo_to_dict(test_dsn)
    params["dbname"] = name
    scratch_dsn = psycopg.conninfo.make_conninfo(**params)
    with psycopg.connect(admin, autocommit=True) as conn:
        conn.execute(f'DROP DATABASE IF EXISTS "{name}"')
        conn.execute(f'CREATE DATABASE "{name}"')
    app = None
    try:
        with psycopg.connect(scratch_dsn, autocommit=True) as conn:
            conn.execute("CREATE EXTENSION provsql CASCADE")

        monkeypatch.setenv("PROVSQL_STUDIO_CONFIG_DIR", str(tmp_path / "cfg"))
        app = create_app(dsn=scratch_dsn)
        app.config.update(TESTING=True)
        client = app.test_client()

        doc = json.loads(
            (Path(app.root_path) / "notebooks" / "tutorial.ipynb").read_text())
        sqls = ["".join(c["source"]) for c in doc["cells"]
                if c["cell_type"] == "code"]
        assert len(sqls) > 20

        for round_no in (1, 2):
            sid = client.post("/api/nb/session").get_json()["session_id"]
            for i, sql_text in enumerate(sqls):
                resp = client.post("/api/nb/exec", json={
                    "session_id": sid, "sql": sql_text})
                payload = resp.get_json()
                errors = [b for b in (payload.get("blocks") or [])
                          if b.get("kind") == "error"]
                assert not errors, (
                    f"round {round_no}, cell {i}: "
                    f"{sql_text[:90]!r} -> {errors}")
                assert payload.get("kernel_dead") is False
            client.delete(f"/api/nb/session/{sid}")
    finally:
        if app is not None:
            app.extensions["provsql_kernels"]["close_all"]()
            app.extensions["provsql_pool"].close()
        with psycopg.connect(admin, autocommit=True) as conn:
            conn.execute(f'DROP DATABASE IF EXISTS "{name}"')
