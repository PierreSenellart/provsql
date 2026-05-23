"""Smoke tests for the knowledge-compilation demo helpers.

Focuses on the parts that are robust to missing external tools:
* /api/kc/tools always returns a structured payload.
* probability_benchmark drops rows whose tool dependencies are absent.

The benchmark unit test monkey-patches ``query_tool_availability`` so
the path under test is the per-row filter, not the actual availability
of d4 / panini / weightmc / … on the CI host.
"""
from __future__ import annotations

import pytest

from provsql_studio import kc


def test_tools_endpoint_shape(client):
    """``/api/kc/tools`` returns the three-section payload the Studio
    front-end and ``probability_benchmark`` both consume."""
    resp = client.get("/api/kc/tools")
    assert resp.status_code == 200
    data = resp.get_json()
    assert set(data.keys()) >= {"tools", "options", "methods"}

    # Every dropdown option from the eval strip must be a key in the
    # respective availability map (so the front-end can decide
    # visibility without falling back on "missing key = available").
    expected_compilers = {
        "d4", "d4v2", "c2d", "minic2d", "dsharp",
        "panini-obdd", "panini-obdd-and", "panini-decdnnf",
        "panini-r2d2", "panini-ccdd",
        "tree-decomposition", "interpret-as-dd", "default",
    }
    assert set(data["options"]["compilation"]) == expected_compilers

    expected_wmc = {"ganak", "sharpsat-td", "dpmc", "weightmc;0.8;0.2"}
    assert set(data["options"]["wmc"]) == expected_wmc

    # In-process methods (no external dependency) must always be
    # reported as available, regardless of which tools are installed.
    for inproc in ("independent", "possible-worlds",
                   "tree-decomposition", "monte-carlo"):
        key = f"{inproc}|"
        assert data["methods"][key] is True


def test_tools_endpoint_invalid_compiler_unaffected(client):
    """Sanity-check: the tools_status payload's dpmc availability
    requires BOTH htb and dmc; either-missing implies False."""
    data = client.get("/api/kc/tools").get_json()
    tools = data["tools"]
    dpmc_avail = data["options"]["wmc"]["dpmc"]
    assert dpmc_avail == (tools.get("htb", False) and tools.get("dmc", False))


def test_benchmark_filters_missing_tools(app, monkeypatch):
    """probability_benchmark drops rows whose tool dependency is
    flagged missing; in-process methods always survive."""
    real = kc.query_tool_availability

    def fake(pool):
        avail = real(pool)
        for tool in ("panini", "htb", "dmc", "weightmc"):
            if tool in avail:
                avail[tool] = False
        return avail

    monkeypatch.setattr(kc, "query_tool_availability", fake)

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

    # Missing-tool rows are gone.
    for absent in (
        ("compilation", "panini-obdd"),
        ("compilation", "panini-obdd-and"),
        ("compilation", "panini-decdnnf"),
        ("compilation", "panini-r2d2"),
        ("compilation", "panini-ccdd"),
        ("wmc", "dpmc"),
        ("wmc", "weightmc;0.8;0.2"),
    ):
        assert absent not in method_args, f"row for missing tool slipped through: {absent}"


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
