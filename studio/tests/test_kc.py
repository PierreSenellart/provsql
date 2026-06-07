"""Smoke tests for the knowledge-compilation demo helpers.

Focuses on the parts that are robust to missing external tools:
* /api/kc/tools always returns the live external-tool catalog from
  provsql.tools.
* probability_benchmark lists every available tool and drops the rest.

The benchmark unit test monkey-patches ``query_catalog`` so the path
under test is the per-row availability filter, not the actual presence
of d4 / panini / weightmc / … on the CI host.
"""
from __future__ import annotations

import psycopg
import pytest

from provsql_studio import kc


def test_tools_endpoint_shape(client):
    """``/api/kc/tools`` returns the live external-tool catalog (from
    provsql.tools) that the Studio front-end and ``probability_benchmark``
    both consume."""
    resp = client.get("/api/kc/tools")
    assert resp.status_code == 200
    data = resp.get_json()
    assert set(data.keys()) >= {"compile", "wmc", "inprocess_compilers"}

    # Each catalog entry carries a name, an availability flag, and a
    # preference (the order the registry would select in).
    for entry in data["compile"] + data["wmc"]:
        assert set(entry) >= {"name", "available", "preference"}
        assert isinstance(entry["available"], bool)

    # The compilers / counters ProvSQL ships with appear in the catalog
    # (an admin-registered tool would appear too, with no Studio change).
    compile_names = {e["name"] for e in data["compile"]}
    assert {"d4", "d4v2", "c2d", "minic2d", "dsharp",
            "panini-obdd", "panini-obdd-and", "panini-decdnnf"} <= compile_names
    wmc_names = {e["name"] for e in data["wmc"]}
    assert {"ganak", "sharpsat-td", "dpmc", "weightmc"} <= wmc_names

    # In-process meta-routes (no external tool, always available) are
    # listed separately so the front-end appends them to the compiler picker.
    # 'inversion-free' is in-process too (offered only for a certified root).
    assert set(data["inprocess_compilers"]) == {
        "tree-decomposition", "interpret-as-dd", "inversion-free", "default"}


def test_tools_endpoint_dpmc_single_entry(client):
    """The two-binary dpmc pipeline (htb | dmc) appears as one wmc entry
    with a boolean availability flag; the C view ANDs its dependencies."""
    data = client.get("/api/kc/tools").get_json()
    dpmc = [e for e in data["wmc"] if e["name"] == "dpmc"]
    assert len(dpmc) == 1
    assert isinstance(dpmc[0]["available"], bool)


def test_benchmark_filters_missing_tools(app, monkeypatch):
    """probability_benchmark lists every available tool and drops the
    unavailable ones; in-process methods always survive."""
    real = kc.query_catalog

    def fake(pool):
        cat = real(pool)
        # Mark the Panini variants, dpmc and weightmc unavailable.
        for entry in cat["compile"] + cat["wmc"]:
            if entry["name"] in ("panini-obdd", "panini-obdd-and",
                                  "panini-decdnnf", "dpmc", "weightmc"):
                entry["available"] = False
        return cat

    monkeypatch.setattr(kc, "query_catalog", fake)

    pool = app.extensions["provsql_pool"]

    # Create a tracked row whose provenance gate we can hand to the
    # benchmark.  We reuse the personnel table that the setup.sql
    # fixture already populated.
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.provenance() FROM personnel WHERE id = 1"
        )
        token = str(cur.fetchone()[0])

    out = kc.probability_benchmark(pool, token, samples=100,
                                   statement_timeout="10s")
    method_args = {(r["method"], r["args"]) for r in out["rows"]}

    # In-process methods always present.
    assert ("independent", None) in method_args
    assert ("possible-worlds", None) in method_args
    assert ("tree-decomposition", None) in method_args
    assert ("monte-carlo", "100") in method_args

    # Unavailable-tool rows are gone.
    for absent in (
        ("compilation", "panini-obdd"),
        ("compilation", "panini-obdd-and"),
        ("compilation", "panini-decdnnf"),
        ("wmc", "dpmc"),
        ("wmc", "weightmc"),
    ):
        assert absent not in method_args, f"row for missing tool slipped through: {absent}"

    # d-DNNF-producing methods carry a size (nodes / edges); methods that
    # build no d-DNNF leave both null.
    by_key = {(r["method"], r["args"]): r for r in out["rows"]}
    td = by_key[("tree-decomposition", None)]
    assert td["error"] is None
    assert td["nodes"] is not None and td["edges"] is not None
    ind = by_key[("independent", None)]
    assert ind["nodes"] is None and ind["edges"] is None


def test_benchmark_includes_karp_luby_and_suppresses_guarantee_notices(app):
    """karp-luby is a benchmark row (sharing the sample count), and the
    per-method approximation-guarantee NOTICE the extension emits for the
    sampling methods is the eval-strip bound, not a benchmark row, so the
    benchmark must drop it from its notice list."""
    pool = app.extensions["provsql_pool"]
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT provsql.provenance() FROM personnel WHERE id = 1")
        token = str(cur.fetchone()[0])

    out = kc.probability_benchmark(pool, token, samples=100,
                                   statement_timeout="10s")
    method_args = {(r["method"], r["args"]) for r in out["rows"]}
    # karp-luby runs with the same sample budget as monte-carlo.
    assert ("karp-luby", "100") in method_args
    # monte-carlo / karp-luby emit the guarantee NOTICE at the verbose level
    # the benchmark floors; none of it must leak into the notice list.
    notices = " ".join(out.get("notices") or [])
    assert "approximation-guarantee" not in notices, notices


def test_benchmark_includes_inversion_free_only_when_certified(app):
    """The 'inversion-free' row is offered only when the root carries the
    inversion-free certificate; a plain (uncertified) token omits it."""
    pool = app.extensions["provsql_pool"]

    # (a) a bare personnel input is not certified -> no inversion-free row
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT provsql.provenance() FROM personnel WHERE id = 1")
        plain = str(cur.fetchone()[0])
    assert not kc._root_is_inversion_free(pool, plain)
    out = kc.probability_benchmark(pool, plain, samples=50, statement_timeout="10s")
    assert ("inversion-free", None) not in {(r["method"], r["args"]) for r in out["rows"]}

    # (b) a certified self-join-free hierarchical circuit -> row present + sound
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute("SET provsql.inversion_free = on")
        cur.execute("SET provsql.provenance = 'semiring'")
        cur.execute("DROP TABLE IF EXISTS bm_a, bm_b CASCADE")
        cur.execute("CREATE TABLE bm_a(x int)")
        cur.execute("CREATE TABLE bm_b(x int)")
        cur.execute("INSERT INTO bm_a VALUES (1),(2)")
        cur.execute("INSERT INTO bm_b VALUES (1),(2)")
        cur.execute("SELECT add_provenance('bm_a')")
        cur.execute("SELECT add_provenance('bm_b')")
        cur.execute("DO $$ BEGIN PERFORM set_prob(provsql,0.5) FROM bm_a; "
                    "PERFORM set_prob(provsql,0.5) FROM bm_b; END $$")
        # NB: no LIMIT -- a LIMIT clause makes the query a non-pure-UCQ and
        # the inversion-free analysis (correctly) declines it; fetchone() takes
        # the first of the per-group tokens instead.
        cur.execute("SELECT provsql.provenance() FROM bm_a a, bm_b b "
                    "WHERE a.x = b.x GROUP BY a.x")
        cert = str(cur.fetchone()[0])
    assert kc._root_is_inversion_free(pool, cert)
    out = kc.probability_benchmark(pool, cert, samples=50, statement_timeout="10s")
    by_key = {(r["method"], r["args"]): r for r in out["rows"]}
    assert ("inversion-free", None) in by_key
    row = by_key[("inversion-free", None)]
    assert row["error"] is None and row["probability"] is not None


def test_compile_to_ddnnf_inversion_free(app):
    """The 'inversion-free' d-D compiler builds the structured d-DNNF (DOT +
    scene + server-side compile time) for a certified circuit, and errors on an
    uncertified one."""
    pool = app.extensions["provsql_pool"]
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute("SET provsql.inversion_free = on")
        cur.execute("SET provsql.provenance = 'semiring'")
        cur.execute("DROP TABLE IF EXISTS cd_a, cd_b CASCADE")
        cur.execute("CREATE TABLE cd_a(x int)")
        cur.execute("CREATE TABLE cd_b(x int)")
        cur.execute("INSERT INTO cd_a VALUES (1),(2)")
        cur.execute("INSERT INTO cd_b VALUES (1),(2)")
        cur.execute("SELECT add_provenance('cd_a')")
        cur.execute("SELECT add_provenance('cd_b')")
        cur.execute("DO $$ BEGIN PERFORM set_prob(provsql,0.5) FROM cd_a; "
                    "PERFORM set_prob(provsql,0.5) FROM cd_b; END $$")
        cur.execute("SELECT provsql.provenance() FROM cd_a a, cd_b b "
                    "WHERE a.x = b.x GROUP BY a.x")
        cert = str(cur.fetchone()[0])

    out = kc.compile_to_ddnnf(pool, cert, "inversion-free", statement_timeout="10s")
    assert out["dot"].lstrip().startswith("digraph")
    assert out["scene"]["nodes"]
    assert isinstance(out["milliseconds"], float) and out["milliseconds"] >= 0

    # an uncertified token has no inversion-free certificate -> SQL errors
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT provsql.provenance() FROM personnel WHERE id = 1")
        plain = str(cur.fetchone()[0])
    with pytest.raises(psycopg.Error):
        kc.compile_to_ddnnf(pool, plain, "inversion-free", statement_timeout="10s")


def test_tool_available_sql_function(app):
    """The C-side provsql.tool_available agrees with the registered
    behaviour: a definitely-absent name returns false; a bare 'sh'
    (always present on any POSIX host) returns true."""
    pool = app.extensions["provsql_pool"]
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT provsql.tool_available('definitely-not-a-real-tool-xyz')")
        assert cur.fetchone()[0] is False
        cur.execute("SELECT provsql.tool_available('sh')")
        assert cur.fetchone()[0] is True


# ---------------------------------------------------------------------------
# HTTP-layer coverage for /api/kc/{cnf,ddnnf,td,benchmark}: the kc_* helpers
# themselves are exercised through other tests in this file; here we pin the
# Flask route's input validation, status codes, and happy-path payload shape.
# ---------------------------------------------------------------------------


@pytest.fixture()
def personnel_token(app):
    """Provenance UUID for the first row of the personnel test fixture."""
    pool = app.extensions["provsql_pool"]
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT provsql.provenance() FROM personnel WHERE id = 1")
        return str(cur.fetchone()[0])


def test_kc_cnf_rejects_invalid_uuid(client):
    resp = client.get("/api/kc/cnf?token=not-a-uuid")
    assert resp.status_code == 400
    assert "UUID" in resp.get_json()["error"]


def test_kc_cnf_happy_path(client, personnel_token):
    resp = client.get(f"/api/kc/cnf?token={personnel_token}")
    assert resp.status_code == 200
    data = resp.get_json()
    assert data["weighted"] is True
    # The panel CNF is self-documenting: "c input <var> <uuid> <prob>"
    # comment lines travel with the text so a copied DIMACS carries the
    # variable mapping (the panel parses them to annotate source tuples;
    # psql users get the same via provsql.tseytin_cnf_mapping).
    assert data["cnf"].lstrip().startswith(("c input", "p cnf"))
    assert "c input" in data["cnf"]
    resp = client.get(f"/api/kc/cnf?token={personnel_token}&weighted=false")
    assert resp.status_code == 200
    assert resp.get_json()["weighted"] is False


def test_kc_ddnnf_rejects_invalid_uuid(client):
    resp = client.get("/api/kc/ddnnf?token=not-a-uuid&compiler=tree-decomposition")
    assert resp.status_code == 400


def test_kc_ddnnf_rejects_unknown_compiler(client, personnel_token):
    resp = client.get(
        f"/api/kc/ddnnf?token={personnel_token}&compiler=not-a-real-compiler"
    )
    assert resp.status_code == 400
    data = resp.get_json()
    assert "unknown compiler" in data["error"]
    assert "hint" in data


def test_kc_ddnnf_returns_501_for_missing_compiler(client, personnel_token, monkeypatch):
    """A hand-crafted GET picking a compiler whose binary is absent gets a
    clean 501 (with the missing-tool list) instead of a generic 500."""
    from provsql_studio import kc
    monkeypatch.setattr(
        kc, "missing_tools_for_compiler",
        lambda pool, compiler: ("d4",),
    )
    resp = client.get(
        f"/api/kc/ddnnf?token={personnel_token}&compiler=d4"
    )
    assert resp.status_code == 501
    data = resp.get_json()
    assert "d4" in data["missing_tools"]


def test_kc_nnf_rejects_invalid_uuid(client):
    resp = client.get("/api/kc/nnf?token=not-a-uuid&compiler=tree-decomposition")
    assert resp.status_code == 400


def test_kc_nnf_rejects_unknown_compiler(client, personnel_token):
    resp = client.get(
        f"/api/kc/nnf?token={personnel_token}&compiler=not-a-real-compiler"
    )
    assert resp.status_code == 400
    assert "unknown compiler" in resp.get_json()["error"]


def test_kc_nnf_happy_path(client, personnel_token):
    # tree-decomposition needs no external tool, so this is deterministic.
    resp = client.get(
        f"/api/kc/nnf?token={personnel_token}&compiler=tree-decomposition"
    )
    assert resp.status_code == 200
    data = resp.get_json()
    assert data["compiler"] == "tree-decomposition"
    assert data["nnf"].startswith("nnf ")


def test_kc_nnf_returns_501_for_missing_compiler(client, personnel_token, monkeypatch):
    from provsql_studio import kc
    monkeypatch.setattr(
        kc, "missing_tools_for_compiler",
        lambda pool, compiler: ("d4",),
    )
    resp = client.get(f"/api/kc/nnf?token={personnel_token}&compiler=d4")
    assert resp.status_code == 501
    assert "d4" in resp.get_json()["missing_tools"]


def test_kc_td_returns_501_when_dot_missing(client, personnel_token, monkeypatch):
    """`/api/kc/td` needs `dot` to render the DOT to SVG; if absent, the
    route 501s ahead of the SQL call rather than letting the
    subprocess.CalledProcessError surface as a 500."""
    from provsql_studio import kc
    monkeypatch.setattr(
        kc, "missing_tools_for_names",
        lambda pool, names: tuple(names),
    )
    resp = client.get(f"/api/kc/td?token={personnel_token}")
    assert resp.status_code == 501
    assert "dot" in resp.get_json()["missing_tools"]


def test_kc_ddnnf_happy_path_tree_decomposition(client, personnel_token):
    """The tree-decomposition compiler is fully in-process (no external
    tool dependency), so the happy-path assertions are stable on any host."""
    resp = client.get(
        f"/api/kc/ddnnf?token={personnel_token}&compiler=tree-decomposition"
    )
    assert resp.status_code == 200
    data = resp.get_json()
    assert data["compiler"] == "tree-decomposition"
    assert "dot" in data and data["dot"].lstrip().startswith("digraph")
    assert "scene" in data
    assert {"nodes", "edges", "root"} <= set(data["scene"].keys())


def test_kc_td_rejects_invalid_uuid(client):
    resp = client.get("/api/kc/td?token=not-a-uuid")
    assert resp.status_code == 400


def test_kc_td_happy_path(client, personnel_token):
    resp = client.get(f"/api/kc/td?token={personnel_token}")
    assert resp.status_code == 200
    data = resp.get_json()
    assert "dot" in data
    assert "scene" in data
    assert "treewidth" in data["scene"]


def test_kc_benchmark_rejects_invalid_uuid(client):
    resp = client.get("/api/kc/benchmark?token=not-a-uuid")
    assert resp.status_code == 400


def test_kc_benchmark_rejects_non_integer_samples(client, personnel_token):
    resp = client.get(
        f"/api/kc/benchmark?token={personnel_token}&samples=abc"
    )
    assert resp.status_code == 400
    assert "integer" in resp.get_json()["error"]


@pytest.mark.parametrize("samples", ["0", "-1", "-100"])
def test_kc_benchmark_rejects_non_positive_samples(client, personnel_token, samples):
    resp = client.get(
        f"/api/kc/benchmark?token={personnel_token}&samples={samples}"
    )
    assert resp.status_code == 400
    assert "positive" in resp.get_json()["error"]


def _registry_or_skip(client):
    """GET the full registry, or skip when the extension predates it."""
    resp = client.get("/api/kc/registry")
    if resp.status_code == 501:
        pytest.skip("tool registry unavailable (extension < 1.8.0)")
    assert resp.status_code == 200
    return resp.get_json()


def test_registry_endpoint_shape(client):
    """``/api/kc/registry`` returns every tool with full columns plus
    whether the connected role may edit it (Tools panel data)."""
    data = _registry_or_skip(client)
    assert set(data) >= {"tools", "can_manage"}
    assert isinstance(data["can_manage"], bool)
    for t in data["tools"]:
        assert set(t) >= {"name", "kind", "operations", "endpoint",
                          "executable", "preference", "enabled", "available"}
        assert isinstance(t["operations"], list)
    assert "d4" in {t["name"] for t in data["tools"]}  # a seeded compiler


def test_registry_management_roundtrip(client):
    """register_tool / set_tool_preference / set_tool_enabled / unregister_tool
    are reachable through the Tools-panel routes and round-trip a kcmcp tool."""
    data = _registry_or_skip(client)
    if not data["can_manage"]:
        pytest.skip("connected role is not a superuser")
    name = "studio-test-kcmcp"
    r = client.post("/api/kc/registry/register", json={
        "name": name, "kind": "kcmcp", "operations": ["compile"],
        "input_formats": ["dimacs-cnf"], "output_format": "ddnnf-nnf",
        "parser": "nnf", "endpoint": "unix:/tmp/studio-test.sock",
    })
    assert r.status_code == 200, r.get_json()
    try:
        tools = {t["name"]: t for t in
                 client.get("/api/kc/registry").get_json()["tools"]}
        assert name in tools
        assert tools[name]["kind"] == "kcmcp"
        assert tools[name]["endpoint"] == "unix:/tmp/studio-test.sock"
        assert client.post("/api/kc/registry/preference",
                           json={"name": name, "preference": 7}).status_code == 200
        assert client.post("/api/kc/registry/enable",
                           json={"name": name, "enabled": False}).status_code == 200
        t = {x["name"]: x for x in
             client.get("/api/kc/registry").get_json()["tools"]}[name]
        assert t["preference"] == 7 and t["enabled"] is False
    finally:
        assert client.post("/api/kc/registry/unregister",
                           json={"name": name}).status_code == 200
    assert name not in {t["name"] for t in
                        client.get("/api/kc/registry").get_json()["tools"]}
