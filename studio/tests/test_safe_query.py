"""Tests for Studio's safe-query (provsql.boolean_provenance) integration.

Covers the four points Studio added on top of the C-side rewriter:

  * The compiled-semiring registry (``db._COMPILED_SEMIRINGS``) mirrors
    the C++ per-semiring ``compatibleWithBooleanRewrite()`` predicate.
  * POST /api/exec with ``prov_scheme = "boolean"`` sets the GUC such that
    the rewriter actually fires (the per-row provenance UUID points at a
    ``gate_assumed_boolean``).
  * POST /api/exec with ``prov_scheme = "semiring"`` leaves the GUC off
    (no ``gate_assumed_boolean`` produced).
  * GET /api/circuit/<uuid> elides ``gate_assumed_boolean`` wrappers and
    stamps the immediate child with ``boolean_assumed = true`` so the
    front-end can render the dashed-ring + "B" badge marker.
"""
from __future__ import annotations

import psycopg


def _setup_two_tracked_tables(client):
    """Drop+create two trivial provenance-tracked tables sharing a key.
    Returns the cleanup SQL to fire in a teardown finally block."""
    setup = (
        "DROP TABLE IF EXISTS sq_t_a, sq_t_b;"
        " CREATE TABLE sq_t_a (x int);"
        " CREATE TABLE sq_t_b (x int);"
        " INSERT INTO sq_t_a VALUES (1), (2);"
        " INSERT INTO sq_t_b VALUES (1), (2);"
        " SELECT add_provenance('sq_t_a');"
        " SELECT add_provenance('sq_t_b');"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    return "DROP TABLE IF EXISTS sq_t_a, sq_t_b;"


def _provenance_uuid(payload):
    """Pull the first row's provsql UUID out of an /api/exec response."""
    final = payload["blocks"][-1]
    assert final["kind"] == "rows", final
    col_names = [c["name"] for c in final["columns"]]
    # The wrapped + bare provenance() column appears under either name
    # depending on how the user wrote the SELECT ; accept both.
    for c in ("provenance", "provsql", "__prov"):
        if c in col_names:
            return final["rows"][0][col_names.index(c)]
    raise AssertionError(f"no provenance column in {col_names}")


def _gate_type(test_dsn, uuid):
    """Query provsql.get_gate_type to verify the C-side gate kind of a UUID."""
    with psycopg.connect(
        f"{test_dsn} options='-c search_path=provsql_test,provsql,public'"
    ) as conn, conn.cursor() as cur:
        cur.execute("SELECT provsql.get_gate_type(%s::uuid)::text", (uuid,))
        return cur.fetchone()[0]


# ──────── Python registry vs. C++ predicate ────────


def test_compiled_registry_has_boolean_rewrite_flag():
    """Every entry in db._COMPILED_SEMIRINGS must declare
    `boolean_rewrite_compatible`; the truthy set must match the C++
    semiring::*::compatibleWithBooleanRewrite() = true semirings.
    Drift detector : if the C++ predicate is flipped, this assertion
    fails and the maintainer must update both this expectation and
    the registry flag."""
    from provsql_studio.db import _COMPILED_SEMIRINGS

    missing = [
        name for name, spec in _COMPILED_SEMIRINGS.items()
        if "boolean_rewrite_compatible" not in spec
    ]
    assert not missing, f"registry entries missing the flag: {missing}"
    bad_type = [
        name for name, spec in _COMPILED_SEMIRINGS.items()
        if not isinstance(spec["boolean_rewrite_compatible"], bool)
    ]
    assert not bad_type, f"non-bool flag: {bad_type}"
    compat = {
        name for name, spec in _COMPILED_SEMIRINGS.items()
        if spec["boolean_rewrite_compatible"]
    }
    # Mirrors src/semiring/*.h compatibleWithBooleanRewrite() = true.
    # `interval-union` is a registry family entry covering sr_temporal /
    # sr_interval_num / sr_interval_int, all of which delegate to
    # semiring::IntervalUnion ; all three return true.
    assert compat == {"boolexpr", "boolean", "formula", "interval-union"}, compat


# ──────── prov_scheme → planner-hook GUC ────────


def test_prov_scheme_boolean_triggers_safe_query_rewrite(client, test_dsn):
    """A hierarchical CQ run with prov_scheme=boolean must produce a
    per-row provenance whose root is gate_assumed_boolean (the marker
    the rewriter wraps every safe-query output in).  Same query under
    prov_scheme=semiring produces a plain plus/times gate."""
    cleanup = _setup_two_tracked_tables(client)
    try:
        sql = ("SELECT a.x, provenance() FROM sq_t_a a, sq_t_b b "
               "WHERE a.x = b.x GROUP BY a.x")

        resp_b = client.post("/api/exec", json={
            "sql": sql,
            "mode": "circuit",
            "prov_scheme": "boolean",
        })
        assert resp_b.status_code == 200, resp_b.data
        uuid_b = _provenance_uuid(resp_b.get_json())
        assert _gate_type(test_dsn, uuid_b) == "assumed_boolean"

        resp_s = client.post("/api/exec", json={
            "sql": sql,
            "mode": "circuit",
            "prov_scheme": "semiring",
        })
        assert resp_s.status_code == 200, resp_s.data
        uuid_s = _provenance_uuid(resp_s.get_json())
        # Without the rewrite the root is the regular per-row root the
        # default pipeline produces (a plus over GROUP BY witnesses,
        # times over the join, ...) ; the only invariant we pin is
        # that it must NOT be gate_assumed_boolean.
        assert _gate_type(test_dsn, uuid_s) != "assumed_boolean"
    finally:
        client.post("/api/exec", json={"sql": cleanup, "mode": "circuit"})


def test_prov_scheme_where_keeps_safe_query_off(client, test_dsn):
    """Selecting the Where flavour must not enable boolean_provenance
    (mutually exclusive at the C level)."""
    cleanup = _setup_two_tracked_tables(client)
    try:
        sql = ("SELECT a.x, provenance() FROM sq_t_a a, sq_t_b b "
               "WHERE a.x = b.x GROUP BY a.x")
        resp = client.post("/api/exec", json={
            "sql": sql,
            "mode": "circuit",
            "prov_scheme": "where",
        })
        assert resp.status_code == 200, resp.data
        uuid_w = _provenance_uuid(resp.get_json())
        assert _gate_type(test_dsn, uuid_w) != "assumed_boolean"
    finally:
        client.post("/api/exec", json={"sql": cleanup, "mode": "circuit"})


# ──────── /api/circuit/<uuid> elides assumed_boolean wrappers ────────


def test_circuit_elides_assumed_boolean_wrapper(client, test_dsn):
    """The circuit endpoint must drop gate_assumed_boolean wrappers
    from the scene and stamp every wrapper's immediate child with
    `boolean_assumed = True` so the front-end can render the dashed
    ring + B badge.  Scene root must be the (non-wrapper) descendant ;
    no node in the returned nodes list may carry type 'assumed_boolean'.
    """
    cleanup = _setup_two_tracked_tables(client)
    try:
        sql = ("SELECT a.x, provenance() FROM sq_t_a a, sq_t_b b "
               "WHERE a.x = b.x GROUP BY a.x")
        resp = client.post("/api/exec", json={
            "sql": sql,
            "mode": "circuit",
            "prov_scheme": "boolean",
        })
        assert resp.status_code == 200, resp.data
        wrapper_uuid = _provenance_uuid(resp.get_json())
        assert _gate_type(test_dsn, wrapper_uuid) == "assumed_boolean"

        scene = client.get(f"/api/circuit/{wrapper_uuid}").get_json()
        # Wrapper UUID must NOT appear as a node ; the new root is the
        # wrapper's single child, which carries the marker.
        ids = {n["id"]: n for n in scene["nodes"]}
        assert wrapper_uuid not in ids
        assert scene["root"] != wrapper_uuid
        assert scene["root"] in ids
        types = {n["type"] for n in scene["nodes"]}
        assert "assumed_boolean" not in types
        # At least one node must be marked ; the scene root is the
        # most reliable: it is the elided wrapper's direct child.
        assert ids[scene["root"]]["boolean_assumed"] is True
    finally:
        client.post("/api/exec", json={"sql": cleanup, "mode": "circuit"})


def test_boolean_mode_is_session_sticky_on_circuit_fetch(client, test_dsn):
    """Picking the Boolean flavour in /api/exec must propagate to
    subsequent /api/circuit fetches : the load-time
    foldBooleanIdentities pass is what makes the wrapper appear on
    circuits NOT produced by the safe-query rewriter, and without
    server-side stickiness it would only fire on the original exec
    transaction.

    Build a non-hierarchical circuit hand-shaped as times(plus(u,v),
    plus(u,v)) ; the safe-query rewriter does not handle this shape
    so no wrapper is persisted to the mmap.  Run /api/exec under
    Boolean mode (purely to set SESSION_MODES) ; then /api/circuit
    must return a scene marked boolean_assumed because the
    load-time fold fires.  Switching back to Semiring must drop the
    marker on the next fetch.
    """
    cleanup = _setup_two_tracked_tables(client)
    try:
        with psycopg.connect(
            f"{test_dsn} options='-c search_path=provsql_test,provsql,public'"
        ) as conn, conn.cursor() as cur:
            cur.execute(
                "SELECT provsql.provenance_plus(ARRAY["
                "(SELECT provsql FROM sq_t_a WHERE x=1),"
                "(SELECT provsql FROM sq_t_b WHERE x=1)])")
            sub = cur.fetchone()[0]
            cur.execute(
                "SELECT public.uuid_generate_v5("
                "provsql.uuid_ns_provsql(), "
                "concat('test-shared-sub-', %s::text))", (sub,))
            root = cur.fetchone()[0]
            cur.execute(
                "SELECT provsql.create_gate(%s, 'times', ARRAY[%s, %s])",
                (root, sub, sub))
            conn.commit()

        # Flip to Boolean mode via a no-op exec; this is what
        # populates app.config["SESSION_MODES"] on the server.
        resp = client.post("/api/exec", json={
            "sql": "SELECT 1", "mode": "circuit", "prov_scheme": "boolean",
        })
        assert resp.status_code == 200, resp.data

        # /api/circuit fetch : must see the assumed_boolean wrap on
        # the descendant of the elided wrapper (depth 0 -> 1).
        scene = client.get(f"/api/circuit/{root}?depth=3").get_json()
        assert any(n.get("boolean_assumed") for n in scene["nodes"]), scene

        # Now switch back to Semiring : SESSION_MODES drops the entry,
        # foldBooleanIdentities does not fire on the next fetch, and
        # the scene comes back without the marker.
        client.post("/api/exec", json={
            "sql": "SELECT 1", "mode": "circuit", "prov_scheme": "semiring",
        })
        scene2 = client.get(f"/api/circuit/{root}?depth=3").get_json()
        assert not any(n.get("boolean_assumed") for n in scene2["nodes"]), scene2
    finally:
        client.post("/api/exec", json={"sql": cleanup, "mode": "circuit"})


def test_circuit_no_marker_when_no_wrapper(client, test_dsn):
    """Sanity inverse : a circuit fetched on a UUID produced WITHOUT
    the safe-query rewriter must not have any node with
    boolean_assumed = True (the marker is exclusively produced by
    server-side wrapper elision)."""
    cleanup = _setup_two_tracked_tables(client)
    try:
        sql = ("SELECT a.x, provenance() FROM sq_t_a a, sq_t_b b "
               "WHERE a.x = b.x GROUP BY a.x")
        resp = client.post("/api/exec", json={
            "sql": sql,
            "mode": "circuit",
            "prov_scheme": "semiring",
        })
        assert resp.status_code == 200, resp.data
        uuid_s = _provenance_uuid(resp.get_json())
        scene = client.get(f"/api/circuit/{uuid_s}").get_json()
        marked = [n for n in scene["nodes"] if n.get("boolean_assumed")]
        assert not marked, marked
    finally:
        client.post("/api/exec", json={"sql": cleanup, "mode": "circuit"})
