"""Tests for the circuit + leaf endpoints in Stage 3.

Coverage:
  * GET  /api/circuit/<uuid>           – known query → expected node/edge shape
  * GET  /api/circuit/<uuid>?depth=1   – frontier flagged when children unfetched
  * POST /api/circuit/<uuid>/expand    – frontier expand returns the next layer
  * GET  /api/leaf/<uuid>              – input gate maps back to its personnel row
  * agg_token UUID accepted by /api/circuit/

These exercise the full pipeline (psycopg → circuit_subgraph → graphviz dot → JSON),
so they require the test database to have provsql installed and personnel set up
(both handled by conftest).
"""
from __future__ import annotations

import psycopg
import pytest


def _personnel_uuid(test_dsn: str, name: str) -> str:
    """Read the provsql UUID for a personnel row by name."""
    with psycopg.connect(
        f"{test_dsn} options='-c search_path=provsql_test,provsql,public'"
    ) as conn, conn.cursor() as cur:
        cur.execute("SELECT provsql::text FROM personnel WHERE name = %s", (name,))
        row = cur.fetchone()
    assert row, f"personnel row {name!r} missing"
    return row[0]


def _circuit_root_for_distinct_city(test_dsn: str, city: str) -> str:
    """Run SELECT DISTINCT city ... and return the provenance UUID for one row.

    This is the canonical Stage 3 demo case: DISTINCT collapses several
    personnel rows into a single + gate whose children are the per-row inputs.
    """
    with psycopg.connect(
        f"{test_dsn} options='-c search_path=provsql_test,provsql,public'"
    ) as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.provenance()::text "
            "FROM (SELECT DISTINCT city FROM personnel) t WHERE city = %s",
            (city,),
        )
        row = cur.fetchone()
    assert row, f"distinct city {city!r} returned no row"
    return row[0]


# ──────── /api/circuit/<uuid> ────────


def test_circuit_distinct_city_paris_has_plus_root(client, test_dsn):
    """SELECT DISTINCT city → 'Paris' has 3 personnel rows, so the root is a
    + gate with 3 input children."""
    root = _circuit_root_for_distinct_city(test_dsn, "Paris")
    resp = client.get(f"/api/circuit/{root}")
    assert resp.status_code == 200
    data = resp.get_json()
    assert data["root"] == root
    nodes_by_id = {n["id"]: n for n in data["nodes"]}
    assert root in nodes_by_id
    assert nodes_by_id[root]["type"] == "plus"

    edges_from_root = [e for e in data["edges"] if e["from"] == root]
    assert len(edges_from_root) == 3, edges_from_root
    children = [nodes_by_id[e["to"]] for e in edges_from_root]
    assert all(c["type"] == "input" for c in children)


def test_circuit_returns_xy_for_every_node(client, test_dsn):
    """Layout must populate x/y on every node (graphviz post-processing)."""
    root = _circuit_root_for_distinct_city(test_dsn, "Berlin")
    resp = client.get(f"/api/circuit/{root}")
    assert resp.status_code == 200
    data = resp.get_json()
    assert data["nodes"], "expected at least one node"
    for n in data["nodes"]:
        assert isinstance(n["x"], (int, float))
        assert isinstance(n["y"], (int, float))


def test_circuit_invalid_uuid_returns_400(client):
    resp = client.get("/api/circuit/not-a-uuid")
    assert resp.status_code == 400


# ──────── frontier + expand ────────


def test_circuit_depth_1_marks_frontier(client, test_dsn):
    """At depth=1, a + gate's input children have unexplored data — they
    should be marked as frontier when they themselves have children. For the
    DISTINCT-city case the inputs are leaf inputs (no children) so they are
    NOT frontier; the test instead checks that the depth-1 envelope is enforced
    (no nodes past depth 1)."""
    root = _circuit_root_for_distinct_city(test_dsn, "Paris")
    resp = client.get(f"/api/circuit/{root}?depth=1")
    assert resp.status_code == 200
    data = resp.get_json()
    assert data["depth"] == 1
    assert max(n["depth"] for n in data["nodes"]) <= 1


def test_circuit_self_join_has_frontier_at_depth_1(client, test_dsn):
    """A self-join produces a × gate over two + gates. At depth=1 the +
    gates appear as leaves (frontier=True since they DO have children)."""
    with psycopg.connect(
        f"{test_dsn} options='-c search_path=provsql_test,provsql,public'",
        autocommit=True,
    ) as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.provenance()::text FROM ("
            " SELECT DISTINCT P1.city FROM personnel P1 JOIN personnel P2 "
            " ON P1.city = P2.city WHERE P1.id < P2.id"
            ") t WHERE city = 'Paris'"
        )
        root = cur.fetchone()[0]

    resp = client.get(f"/api/circuit/{root}?depth=1")
    assert resp.status_code == 200
    data = resp.get_json()
    # At least one node at depth 1 is a frontier (has children we didn't fetch).
    frontier_nodes = [n for n in data["nodes"] if n["frontier"]]
    assert frontier_nodes, "expected at least one frontier at depth=1"


def test_circuit_expand_returns_next_layer(client, test_dsn):
    """Expanding a frontier returns a fresh subgraph rooted there."""
    root = _circuit_root_for_distinct_city(test_dsn, "Paris")
    resp = client.get(f"/api/circuit/{root}?depth=1")
    data = resp.get_json()
    # Pick any non-root node and expand it. For the DISTINCT case all children
    # are leaves so we expand one anyway and expect a 1-node response.
    target = next(n for n in data["nodes"] if n["id"] != root)
    expand = client.post(
        f"/api/circuit/{root}/expand",
        json={"frontier_node_uuid": target["id"], "additional_depth": 2},
    )
    assert expand.status_code == 200
    sub = expand.get_json()
    assert sub["root"] == target["id"]
    assert any(n["id"] == target["id"] for n in sub["nodes"])


# ──────── /api/leaf/<uuid> ────────


def test_leaf_resolves_personnel_row(client, test_dsn):
    """A direct provsql UUID from the personnel table maps back to that row."""
    uuid = _personnel_uuid(test_dsn, "Magdalen")
    resp = client.get(f"/api/leaf/{uuid}")
    assert resp.status_code == 200
    matches = resp.get_json()["matches"]
    assert len(matches) == 1
    assert matches[0]["relation"].endswith("personnel")
    assert matches[0]["row"]["name"] == "Magdalen"


def test_leaf_unknown_uuid_returns_404(client):
    # Random UUID that doesn't correspond to any input gate.
    resp = client.get("/api/leaf/00000000-0000-0000-0000-000000000123")
    assert resp.status_code == 404


# ──────── agg_token acceptance ────────


def test_circuit_accepts_agg_token_underlying_uuid(client, test_dsn):
    """The /api/circuit endpoint accepts UUIDs sourced from agg_token columns.

    A `GROUP BY` over a provenance-tagged relation produces `count(*)` as an
    `agg_token`. The text cast strips the aggregate value and returns the
    circuit root UUID — which the front-end uses for /api/circuit calls."""
    with psycopg.connect(
        f"{test_dsn} options='-c search_path=provsql_test,provsql,public'",
        autocommit=True,
    ) as conn, conn.cursor() as cur:
        # Materialize via CTAS to coerce the rewriter to actually emit
        # agg_token (a bare GROUP BY SELECT may keep it as bigint when the
        # planner sees no need for aggregate provenance).
        cur.execute(
            "CREATE TEMP TABLE _agg_demo AS "
            "SELECT city, count(*) AS c FROM personnel GROUP BY city"
        )
        cur.execute("SELECT pg_typeof(c)::text FROM _agg_demo LIMIT 1")
        assert cur.fetchone()[0] == "agg_token"
        # c::uuid (implicit cast) extracts the circuit root from the
        # agg_token; c::text by contrast returns the aggregate value only.
        cur.execute("SELECT (c::uuid)::text FROM _agg_demo WHERE city = 'Paris'")
        agg_uuid = cur.fetchone()[0]

    resp = client.get(f"/api/circuit/{agg_uuid}")
    assert resp.status_code == 200
    data = resp.get_json()
    assert data["root"] == agg_uuid
    assert data["nodes"], "expected a non-empty subgraph for the agg circuit"
    # The root of an aggregation circuit is an `agg` gate.
    nodes_by_id = {n["id"]: n for n in data["nodes"]}
    assert nodes_by_id[agg_uuid]["type"] == "agg"
