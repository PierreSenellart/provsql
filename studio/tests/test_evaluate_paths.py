"""End-to-end test for the three-path tolerance surface through /api/evaluate.

The user grants a tolerance ('relative' / 'additive') and the extension's cost
chooser picks the mechanism, returning an EXACT value when one is cheap
("exact when cheaper") -- so on a small circuit a relative/additive request
resolves to an exact method and the response carries that method in
``resolved_method`` (which the Studio eval strip surfaces as "via <method>").
The 'exact' alias and the default method also report the route taken.
"""
from __future__ import annotations

import psycopg

# Exact methods the cost chooser may settle on for the tiny read-once DNF below
# (independent is cheapest, but the assertion stays robust to cost-tuning).
_EXACT_METHODS = {"independent", "possible-worlds", "sieve",
                  "tree-decomposition", "compilation", "inversion-free"}


def _dnf_root(test_dsn: str) -> str:
    """Build (x1 AND x2) OR (x3 AND x4) with 0.5 leaves; return the root token.

    Disjoint clauses, so the exact probability is 1-(1-0.25)(1-0.25) = 0.4375,
    and the circuit is read-once -- cheaply exact for any tolerance.
    """
    dsn = f"{test_dsn} options='-c search_path=public,provsql'"
    with psycopg.connect(dsn, autocommit=True) as conn, conn.cursor() as cur:
        cur.execute("SET provsql.boolean_provenance = off")
        cur.execute("DROP TABLE IF EXISTS pp_dnf CASCADE")
        cur.execute("CREATE TABLE pp_dnf(id int)")
        cur.execute("INSERT INTO pp_dnf SELECT generate_series(1, 4)")
        cur.execute("SELECT add_provenance('pp_dnf')")
        cur.execute("DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM pp_dnf; END $$")
        cur.execute(
            "SELECT provenance_plus(ARRAY["
            "  provenance_times((SELECT provsql FROM pp_dnf WHERE id=1),"
            "                   (SELECT provsql FROM pp_dnf WHERE id=2)),"
            "  provenance_times((SELECT provsql FROM pp_dnf WHERE id=3),"
            "                   (SELECT provsql FROM pp_dnf WHERE id=4))])::text")
        row = cur.fetchone()
    assert row, "DNF witness produced no token"
    return row[0]


def _evaluate(client, root, method):
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "probability",
        "method": method,
        "arguments": "epsilon=0.1,delta=0.05" if method in ("relative", "additive") else "",
    })
    assert resp.status_code == 200, resp.data
    return resp.get_json()


def test_relative_path_returns_exact_when_cheap(client, test_dsn):
    """A 'relative' request on a cheaply-exact circuit returns the EXACT value
    (no sampling) and reports the exact method actually used."""
    root = _dnf_root(test_dsn)
    data = _evaluate(client, root, "relative")
    assert data["kind"] == "float"
    # Exact-when-cheaper: the chooser used an exact method, so the value is
    # exactly 0.4375 (not an estimate).
    assert abs(float(data["result"]) - 0.4375) < 1e-9, data
    assert data.get("resolved_method") in _EXACT_METHODS, data


def test_additive_path_returns_exact_when_cheap(client, test_dsn):
    root = _dnf_root(test_dsn)
    data = _evaluate(client, root, "additive")
    assert data["kind"] == "float"
    assert abs(float(data["result"]) - 0.4375) < 1e-9, data
    assert data.get("resolved_method") in _EXACT_METHODS, data


def test_default_method_reports_resolved_method(client, test_dsn):
    """The default (empty) method runs the cost chooser; the response reports
    which exact method it settled on."""
    root = _dnf_root(test_dsn)
    data = _evaluate(client, root, "")
    assert abs(float(data["result"]) - 0.4375) < 1e-9, data
    assert data.get("resolved_method") in _EXACT_METHODS, data


def test_exact_alias_accepted(client, test_dsn):
    """'exact' is accepted as an alias for the default method."""
    root = _dnf_root(test_dsn)
    data = _evaluate(client, root, "exact")
    assert abs(float(data["result"]) - 0.4375) < 1e-9, data
    assert data.get("resolved_method") in _EXACT_METHODS, data
