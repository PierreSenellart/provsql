"""Tests for the Config-panel endpoints (/api/config GET/POST).

These cover the panel-managed GUCs (`provsql.active`, `provsql.verbose_level`)
that are stored in app state and applied as SET LOCAL on every batch. The
toggle-managed GUCs (where_provenance, update_provenance) live elsewhere and
are deliberately rejected by this endpoint to avoid silently overriding the
per-query toggles.
"""
from __future__ import annotations


def test_config_get_shows_defaults_and_no_overrides(client):
    cfg = client.get("/api/config").get_json()
    assert cfg["overrides"] == {}
    eff = cfg["effective"]
    # Server-side defaults match the C registration in src/provsql.c.
    assert eff["provsql.active"] == "on"
    assert eff["provsql.verbose_level"] == "0"


def test_config_post_active_off_persists_across_requests(client):
    resp = client.post("/api/config", json={"key": "provsql.active", "value": "off"})
    assert resp.status_code == 200, resp.data
    assert resp.get_json()["value"] == "off"
    # The override is now visible on the next GET, and the effective value
    # reflects it because show_panel_gucs applies it via SET LOCAL.
    cfg = client.get("/api/config").get_json()
    assert cfg["overrides"]["provsql.active"] == "off"
    assert cfg["effective"]["provsql.active"] == "off"


def test_config_post_active_actually_disables_rewriter(client):
    # Sanity-check that the override propagates into exec_batch's SET LOCAL,
    # not just into the panel's own GET. A SHOW provsql.active inside the
    # query batch must observe the panel-set value.
    client.post("/api/config", json={"key": "provsql.active", "value": "off"})
    resp = client.post("/api/exec",
                       json={"sql": "SHOW provsql.active", "mode": "circuit"})
    assert resp.status_code == 200
    final = resp.get_json()["blocks"][-1]
    assert final["kind"] == "rows"
    assert final["rows"][0][0] == "off"


def test_config_verbose_level_canonicalises_int(client):
    resp = client.post("/api/config",
                       json={"key": "provsql.verbose_level", "value": " 42 "})
    assert resp.status_code == 200
    assert resp.get_json()["value"] == "42"
    # Effective value must be the same after a fresh GET.
    cfg = client.get("/api/config").get_json()
    assert cfg["effective"]["provsql.verbose_level"] == "42"


def test_config_post_rejects_out_of_range_verbose(client):
    resp = client.post("/api/config",
                       json={"key": "provsql.verbose_level", "value": "200"})
    assert resp.status_code == 400


def test_config_post_rejects_non_panel_guc(client):
    # `provsql.where_provenance` is whitelisted overall but is owned by the
    # per-query toggle; the panel must not accept it.
    resp = client.post("/api/config",
                       json={"key": "provsql.where_provenance", "value": "on"})
    assert resp.status_code == 400


def test_config_post_rejects_unknown_guc(client):
    resp = client.post("/api/config",
                       json={"key": "provsql.no_such_setting", "value": "on"})
    assert resp.status_code == 400


def test_panel_verbose_only_applies_inside_exec_batch(client, app):
    """`provsql.verbose_level` is pinned to 0 at the pool's connection-
    configure step so Studio's housekeeping queries stay quiet, and the
    user's panel value is layered on top via SET LOCAL inside exec_batch
    only. The override must therefore NOT leak to the connection-default
    scope: a SHOW outside exec_batch should still see 0."""
    # Set a high verbose level via the panel.
    resp = client.post("/api/config",
                       json={"key": "provsql.verbose_level", "value": "55"})
    assert resp.status_code == 200

    # Inside an /api/exec batch, the override is visible.
    r = client.post("/api/exec",
                    json={"sql": "SHOW provsql.verbose_level", "mode": "circuit"})
    assert r.get_json()["blocks"][-1]["rows"][0][0] == "55"

    # Outside exec_batch (a route that opens its own short transaction),
    # the configure-callback default applies. Use the pool directly to
    # mirror what list_relations / conn_info / list_databases do.
    pool = app.extensions["provsql_pool"]
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute("SHOW provsql.verbose_level")
        assert cur.fetchone()[0] == "0"


def test_config_options_round_trip(client):
    """Studio-level options (max_circuit_depth, statement_timeout) live
    alongside GUC overrides under `options` in the GET payload."""
    cfg = client.get("/api/config").get_json()
    assert "options" in cfg
    assert cfg["options"]["max_circuit_depth"] == 8        # CLI default
    assert cfg["options"]["statement_timeout_seconds"] == 30

    resp = client.post("/api/config", json={"key": "max_circuit_depth", "value": 12})
    assert resp.status_code == 200
    assert resp.get_json()["value"] == 12

    resp = client.post("/api/config",
                       json={"key": "statement_timeout_seconds", "value": 5})
    assert resp.status_code == 200
    assert resp.get_json()["value"] == 5

    cfg = client.get("/api/config").get_json()
    assert cfg["options"]["max_circuit_depth"] == 12
    assert cfg["options"]["statement_timeout_seconds"] == 5


def test_config_options_validation(client):
    # Out-of-range max depth.
    r = client.post("/api/config", json={"key": "max_circuit_depth", "value": 999})
    assert r.status_code == 400
    # Non-integer timeout.
    r = client.post("/api/config",
                    json={"key": "statement_timeout_seconds", "value": "abc"})
    assert r.status_code == 400


def test_config_options_persist_across_app_restart(test_dsn, tmp_path, monkeypatch):
    """A Studio restart should pick up the persisted options."""
    monkeypatch.setenv("PROVSQL_STUDIO_CONFIG_DIR", str(tmp_path / "studio_cfg"))
    from provsql_studio.app import create_app
    dsn = f"{test_dsn} options='-c search_path=provsql_test,provsql,public'"

    app1 = create_app(dsn=dsn)
    c1 = app1.test_client()
    c1.post("/api/config", json={"key": "max_circuit_depth", "value": 6})
    c1.post("/api/config", json={"key": "statement_timeout_seconds", "value": 7})

    app2 = create_app(dsn=dsn)
    cfg = app2.test_client().get("/api/config").get_json()
    assert cfg["options"]["max_circuit_depth"] == 6
    assert cfg["options"]["statement_timeout_seconds"] == 7
    # The persisted values must drive the live app config too — that's
    # what the rest of the request pipeline reads.
    assert app2.config["MAX_CIRCUIT_DEPTH"] == 6
    assert app2.config["STATEMENT_TIMEOUT"] == "7s"


def test_config_overrides_persist_across_app_restart(client, test_dsn):
    """The user's panel choices must survive a Studio restart: the new
    process re-reads the override from disk and applies it on its first
    /api/exec batch."""
    # First app: write an override.
    resp = client.post("/api/config",
                       json={"key": "provsql.verbose_level", "value": "42"})
    assert resp.status_code == 200

    # Simulate a Studio restart by building a fresh app and client. The
    # PROVSQL_STUDIO_CONFIG_DIR env var pinned by conftest is preserved
    # across this re-create (same pytest test, same monkeypatched env).
    from provsql_studio.app import create_app
    fresh_app = create_app(
        dsn=f"{test_dsn} options='-c search_path=provsql_test,provsql,public'"
    )
    fresh_client = fresh_app.test_client()
    cfg = fresh_client.get("/api/config").get_json()
    assert cfg["overrides"].get("provsql.verbose_level") == "42"
    assert cfg["effective"]["provsql.verbose_level"] == "42"
    # Deeper check: the override actually fires inside an exec_batch.
    resp = fresh_client.post(
        "/api/exec",
        json={"sql": "SHOW provsql.verbose_level", "mode": "circuit"},
    )
    assert resp.status_code == 200
    final = resp.get_json()["blocks"][-1]
    assert final["rows"][0][0] == "42"
