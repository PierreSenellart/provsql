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


def test_circuit_too_large_returns_actionable_413(test_dsn, tmp_path, monkeypatch):
    """When the rendered subgraph exceeds max_circuit_nodes, the route must
    answer 413 with the structured payload the front-end uses to surface
    a "Render at depth N-1" button: node_count, cap, depth, hint. The
    'Paris' DISTINCT circuit has 4 nodes (3 input gates + 1 plus root);
    capping at 2 forces the path."""
    from provsql_studio.app import create_app
    monkeypatch.setenv("PROVSQL_STUDIO_CONFIG_DIR", str(tmp_path / "studio_cfg"))
    app = create_app(
        dsn=f"{test_dsn} options='-c search_path=provsql_test,provsql,public'",
        max_circuit_nodes=2,
    )
    app.config.update(TESTING=True)
    client = app.test_client()
    root = _circuit_root_for_distinct_city(test_dsn, "Paris")
    resp = client.get(f"/api/circuit/{root}")
    assert resp.status_code == 413
    body = resp.get_json()
    assert body["error"] == "circuit too large"
    assert body["cap"] == 2
    assert body["node_count"] > 2
    # depth lets the front-end know the rendering depth used by the
    # server (it picks up MAX_CIRCUIT_DEPTH when the request omits it).
    assert isinstance(body["depth"], int) and body["depth"] >= 1
    # depth_1_size lets the front-end decide whether the "Render at
    # depth 1" retry is meaningful: only when this fits under the cap.
    # The Paris circuit at depth=1 is the plus root + 3 input children
    # = 4 nodes, which is > cap=2 here, so the front-end will (correctly)
    # suppress the retry button for this case.
    assert body["depth_1_size"] == 4


# ──────── frontier + expand ────────


def test_circuit_depth_1_marks_frontier(client, test_dsn):
    """At depth=1, a + gate's input children have unexplored data; they
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
    """A direct provsql UUID from the personnel table maps back to that row.

    ProvSQL returns 1.0 as the default probability for any input gate
    that doesn't have an explicit set_prob entry, so /api/leaf surfaces
    `probability: 1.0` on personnel rows untouched by the conftest setup."""
    uuid = _personnel_uuid(test_dsn, "Magdalen")
    resp = client.get(f"/api/leaf/{uuid}")
    assert resp.status_code == 200
    body = resp.get_json()
    matches = body["matches"]
    assert len(matches) == 1
    assert matches[0]["relation"].endswith("personnel")
    assert matches[0]["row"]["name"] == "Magdalen"
    assert body["probability"] == 1.0


def test_leaf_includes_probability_when_set(client, test_dsn):
    """When `set_prob` has assigned a non-default probability to an
    input gate, /api/leaf surfaces it next to the resolved row so the
    inspector can show it without a second round-trip."""
    uuid = _personnel_uuid(test_dsn, "Magdalen")
    with psycopg.connect(
        f"{test_dsn} options='-c search_path=provsql_test,provsql,public'"
    ) as conn, conn.cursor() as cur:
        cur.execute("SELECT provsql.set_prob(%s::uuid, 0.42)", (uuid,))
    try:
        resp = client.get(f"/api/leaf/{uuid}")
        assert resp.status_code == 200
        body = resp.get_json()
        assert body["probability"] == 0.42
    finally:
        # ProvSQL rejects NULL on set_prob; reset to 1.0 (the implicit
        # default) so other tests see Magdalen as unset.
        with psycopg.connect(
            f"{test_dsn} options='-c search_path=provsql_test,provsql,public'"
        ) as conn, conn.cursor() as cur:
            cur.execute("SELECT provsql.set_prob(%s::uuid, 1.0)", (uuid,))


def test_leaf_unknown_uuid_returns_404(client):
    # Random UUID that doesn't correspond to any input gate.
    resp = client.get("/api/leaf/00000000-0000-0000-0000-000000000123")
    assert resp.status_code == 404


def test_circuit_tracked_input_keeps_iota_label(client, test_dsn):
    """An input gate whose UUID appears in some tracked relation must
    keep its ι glyph regardless of any pinned probability.  Regression
    test for the old `info1 == 0` discriminator: `add_provenance`
    doesn't write into `info1`, so a tracked-table gate with a
    user-pinned probability used to render as e.g. "42%" instead of ι.
    The bulk catalog scan in `_fetch_tracked_input_uuids` is the
    source of truth now; the test pins the contract."""
    uuid = _personnel_uuid(test_dsn, "John")
    # Pin a non-default probability so the bug condition (prob != 1.0)
    # would have fired the percentage-label branch.
    client.post("/api/set_prob", json={"uuid": uuid, "probability": 0.42})
    try:
        resp = client.get(f"/api/circuit/{uuid}")
        assert resp.status_code == 200, resp.data
        body = resp.get_json()
        node = next((n for n in body["nodes"] if n["id"] == uuid), None)
        assert node is not None, body
        assert node["type"] == "input"
        assert node["label"] == "ι", node
        assert node["tracked_input"] is True
    finally:
        # Reset to the implicit default so other tests on John see 1.0.
        client.post("/api/set_prob", json={"uuid": uuid, "probability": 1.0})


def test_circuit_anonymous_input_renders_probability(client, test_dsn):
    """An anonymous input (no source row in any tracked relation) must
    render its probability inline as a percentage, including the 1.0
    case so the simplifier-minted dec-in-N anchor of a categorical
    block doesn't fall through to a bare ι next to its dec-mul-N
    siblings."""
    import uuid as _uuid
    anon_uuid = str(_uuid.uuid4())
    with psycopg.connect(
        f"{test_dsn} options='-c search_path=provsql_test,provsql,public'"
    ) as conn, conn.cursor() as cur:
        cur.execute("SELECT provsql.create_gate(%s::uuid, 'input')", (anon_uuid,))
        cur.execute("SELECT provsql.set_prob(%s::uuid, 0.37)", (anon_uuid,))
    resp = client.get(f"/api/circuit/{anon_uuid}")
    assert resp.status_code == 200, resp.data
    body = resp.get_json()
    node = next((n for n in body["nodes"] if n["id"] == anon_uuid), None)
    assert node is not None, body
    assert node["type"] == "input"
    assert node["label"] == "37%", node
    assert node["tracked_input"] is False


def test_leaf_anonymous_input_surfaces_probability(client, test_dsn):
    """An input gate created via `create_gate(uuid, 'input') + set_prob`
    -- e.g. by the `provsql.mixture(p_value, x, y)` overload, or by hand
    for a manual Bernoulli -- has no source row in any tracked relation,
    but its probability is still meaningful and must be surfaced.  Verify
    that /api/leaf returns 200 with an empty `matches` list AND the
    probability field, so the front-end can render the prob alongside
    the "no source row" notice."""
    import uuid as _uuid
    anon_uuid = str(_uuid.uuid4())
    with psycopg.connect(
        f"{test_dsn} options='-c search_path=provsql_test,provsql,public'"
    ) as conn, conn.cursor() as cur:
        cur.execute("SELECT provsql.create_gate(%s::uuid, 'input')", (anon_uuid,))
        cur.execute("SELECT provsql.set_prob(%s::uuid, 0.37)", (anon_uuid,))
    resp = client.get(f"/api/leaf/{anon_uuid}")
    assert resp.status_code == 200, resp.data
    body = resp.get_json()
    assert body["matches"] == []
    assert abs(body["probability"] - 0.37) < 1e-12


# ──────── /api/set_prob ────────


def test_set_prob_writes_value(client, test_dsn):
    """POST /api/set_prob writes to provsql.set_prob; the value is then
    visible via GET /api/leaf as `probability` on the same UUID."""
    uuid = _personnel_uuid(test_dsn, "John")
    try:
        resp = client.post("/api/set_prob", json={"uuid": uuid, "probability": 0.7})
        assert resp.status_code == 200, resp.data
        assert resp.get_json()["ok"] is True
        leaf = client.get(f"/api/leaf/{uuid}").get_json()
        assert leaf["probability"] == 0.7
    finally:
        # Reset to the implicit default so other tests on John don't see 0.7.
        client.post("/api/set_prob", json={"uuid": uuid, "probability": 1.0})


def test_set_prob_rejects_out_of_range(client, test_dsn):
    uuid = _personnel_uuid(test_dsn, "John")
    resp = client.post("/api/set_prob", json={"uuid": uuid, "probability": 2.5})
    assert resp.status_code == 400
    assert "between 0 and 1" in resp.get_json()["error"]


def test_set_prob_rejects_invalid_uuid(client):
    resp = client.post("/api/set_prob", json={"uuid": "not-a-uuid", "probability": 0.5})
    assert resp.status_code == 400


def test_set_prob_rejects_non_numeric(client, test_dsn):
    uuid = _personnel_uuid(test_dsn, "John")
    resp = client.post("/api/set_prob", json={"uuid": uuid, "probability": "abc"})
    assert resp.status_code == 400


# ──────── agg_token acceptance ────────


def test_circuit_accepts_agg_token_underlying_uuid(client, test_dsn):
    """The /api/circuit endpoint accepts UUIDs sourced from agg_token columns.

    A `GROUP BY` over a provenance-tagged relation produces `count(*)` as an
    `agg_token`. The text cast strips the aggregate value and returns the
    circuit root UUID, which the front-end uses for /api/circuit calls."""
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


def test_simplified_circuit_drops_decidable_cmp(client, test_dsn):
    """When provsql.simplify_on_load is on (the panel default and the
    server default), /api/circuit renders the IN-MEMORY simplified DAG
    so RangeCheck-decidable rv comparisons appear as gate_one /
    gate_zero leaves instead of the persisted gate_cmp shape.

    Build a query whose lifted WHERE is `uniform(0, 1) > -10`, which
    is certainly TRUE (support [0, 1] is entirely above -10).
    RangeCheck rewrites the cmp to gate_one; the times gate that wraps
    it then has gate_one as its second child.  With simplify_on_load
    turned off via /api/config, the same fetch returns the raw cmp.
    """
    import psycopg
    # Capture a row provenance whose WHERE conjunct is the rv cmp.
    sql = ("CREATE TEMP TABLE _simp_demo AS "
           "SELECT provsql.provenance() AS p "
           "WHERE provsql.uniform(0.0::float8, 1.0::float8) "
           "      > (-10)::provsql.random_variable;")
    with psycopg.connect(
        f"{test_dsn} options='-c search_path=provsql_test,provsql,public'",
        autocommit=True,
    ) as conn, conn.cursor() as cur:
        cur.execute(sql)
        cur.execute("SELECT p::text FROM _simp_demo")
        row = cur.fetchone()
    assert row, "expected a row from the FROM-less RV cmp"
    root = row[0]

    # Default panel state: simplify_on_load is on -> cmp folded to one.
    resp = client.get(f"/api/circuit/{root}?depth=2")
    assert resp.status_code == 200, resp.data
    scene = resp.get_json()
    types_on = {n["type"] for n in scene["nodes"]}
    assert "one" in types_on, scene
    assert "cmp" not in types_on, scene

    # Flip the panel GUC off; the next fetch must see the raw cmp.
    r = client.post("/api/config",
                    json={"key": "provsql.simplify_on_load", "value": "off"})
    assert r.status_code == 200, r.data
    try:
        resp = client.get(f"/api/circuit/{root}?depth=2")
        assert resp.status_code == 200, resp.data
        scene = resp.get_json()
        types_off = {n["type"] for n in scene["nodes"]}
        assert "cmp" in types_off, scene
    finally:
        client.post("/api/config",
                    json={"key": "provsql.simplify_on_load", "value": "on"})


# ──────── categorical mixtures ────────


def _categorical_root_uuid(test_dsn: str, probs_sql: str, values_sql: str) -> str:
    """Build a `provsql.categorical(probs, values)` random variable and
    return the root UUID."""
    import psycopg
    with psycopg.connect(
        f"{test_dsn} options='-c search_path=provsql_test,provsql,public'",
        autocommit=True,
    ) as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.random_variable_uuid("
            "  provsql.categorical(%s::float8[], %s::float8[]))::text"
            % (probs_sql, values_sql)
        )
        row = cur.fetchone()
    assert row, "categorical() returned no row"
    return row[0]


def test_circuit_categorical_has_categorical_shape(client, test_dsn):
    """`provsql.categorical([0.3, 0.7], [0, 10])` builds a root
    `gate_mixture` whose wires are `[key, mul_1, mul_2]`: the first
    child is a `gate_input` key anchor, the rest are `gate_mulinput`
    outcome leaves.  Each mulinput carries its outcome value in the
    `extra` field and its probability via set_prob (rendered inline by
    the simplified subgraph path so the front-end shows it without a
    separate round-trip)."""
    root = _categorical_root_uuid(
        test_dsn,
        "ARRAY[0.3, 0.7]", "ARRAY[0, 10]",
    )
    resp = client.get(f"/api/circuit/{root}?depth=2")
    assert resp.status_code == 200, resp.data
    scene = resp.get_json()
    nodes_by_id = {n["id"]: n for n in scene["nodes"]}

    root_node = nodes_by_id[root]
    assert root_node["type"] == "mixture", root_node

    # Children of the root in child_pos order.
    children = sorted(
        (e for e in scene["edges"] if e["from"] == root),
        key=lambda e: e["child_pos"],
    )
    assert len(children) == 3, children  # key + 2 mulinputs

    types = [nodes_by_id[e["to"]]["type"] for e in children]
    assert types == ["input", "mulinput", "mulinput"], types

    # The mulinputs carry their outcome values in `extra`, parseable
    # as float8 -- 0.0 and 10.0 in some order.
    values = sorted(float(nodes_by_id[e["to"]]["extra"]) for e in children[1:])
    assert values == [0.0, 10.0], values


def test_circuit_dirac_mixture_collapses_when_simplified(client, test_dsn):
    """The hybrid simplifier rewrites a Dirac-only `mixture(p, value,
    value)` cascade into the same categorical block `categorical()`
    builds directly: one fresh gate_input key + one gate_mulinput per
    outcome with its value in `extra`.  Studio renders the simplified
    DAG by default (provsql.hybrid_evaluation is on), so a mixture
    built from `as_random` Diracs shows as a categorical block in the
    panel."""
    import psycopg
    with psycopg.connect(
        f"{test_dsn} options='-c search_path=provsql_test,provsql,public'",
        autocommit=True,
    ) as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.random_variable_uuid("
            "  provsql.mixture(0.3::float8,"
            "                  provsql.as_random(0),"
            "                  provsql.as_random(10)))::text"
        )
        row = cur.fetchone()
    assert row, "mixture(...) returned no row"
    root = row[0]

    resp = client.get(f"/api/circuit/{root}?depth=2")
    assert resp.status_code == 200, resp.data
    scene = resp.get_json()
    nodes_by_id = {n["id"]: n for n in scene["nodes"]}

    root_node = nodes_by_id[root]
    assert root_node["type"] == "mixture", root_node

    # The collapsed form has a key + N mulinputs (≥ 2 here), not the
    # classic 3-wire [p, x, y] shape.
    children = sorted(
        (e for e in scene["edges"] if e["from"] == root),
        key=lambda e: e["child_pos"],
    )
    assert len(children) == 3, children
    types = [nodes_by_id[e["to"]]["type"] for e in children]
    assert types == ["input", "mulinput", "mulinput"], types

    # The mulinputs carry the Dirac values in `extra`.
    values = sorted(float(nodes_by_id[e["to"]]["extra"]) for e in children[1:])
    assert values == [0.0, 10.0], values


def test_circuit_categorical_mulinput_label_shows_value(client, test_dsn):
    """A mulinput's in-circle label normally renders as the generic '⋮'
    glyph (repair_key's ordinal mulinputs).  For the categorical
    mixture form the mulinput carries its outcome value in `extra`,
    and `_gate_label` renders that value inline so the canvas shows
    the payload at a glance."""
    root = _categorical_root_uuid(
        test_dsn,
        "ARRAY[0.25, 0.75]", "ARRAY[7, 42]",
    )
    resp = client.get(f"/api/circuit/{root}?depth=2")
    assert resp.status_code == 200, resp.data
    scene = resp.get_json()

    mul_labels = sorted(
        n["label"] for n in scene["nodes"]
        if n["type"] == "mulinput"
    )
    assert mul_labels == ["42", "7"], mul_labels  # alphabetical


def test_circuit_persisted_view_keeps_classic_mixture_shape(client, test_dsn):
    """With provsql.hybrid_evaluation turned off via /api/config, the
    simplifier's Dirac collapse does not run, so a Dirac mixture
    appears in its classic 3-wire `[p, x, y]` form instead of the
    categorical block."""
    import psycopg
    with psycopg.connect(
        f"{test_dsn} options='-c search_path=provsql_test,provsql,public'",
        autocommit=True,
    ) as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.random_variable_uuid("
            "  provsql.mixture(0.3::float8,"
            "                  provsql.as_random(0),"
            "                  provsql.as_random(10)))::text"
        )
        row = cur.fetchone()
    assert row
    root = row[0]

    r = client.post("/api/config",
                    json={"key": "provsql.hybrid_evaluation", "value": "off"})
    assert r.status_code == 200, r.data
    try:
        resp = client.get(f"/api/circuit/{root}?depth=2")
        assert resp.status_code == 200, resp.data
        scene = resp.get_json()
        nodes_by_id = {n["id"]: n for n in scene["nodes"]}

        children = sorted(
            (e for e in scene["edges"] if e["from"] == root),
            key=lambda e: e["child_pos"],
        )
        assert len(children) == 3, children
        types = [nodes_by_id[e["to"]]["type"] for e in children]
        # Classic shape: Bernoulli input + two Dirac value leaves.
        assert types == ["input", "value", "value"], types
    finally:
        client.post("/api/config",
                    json={"key": "provsql.hybrid_evaluation", "value": "on"})