"""End-to-end test for the 'karp-luby' probability method through /api/evaluate.

Builds a DNF-shaped circuit -- (x1 AND x2) OR (x3 AND x4) over tuple-independent
leaves -- evaluates it with the Karp-Luby FPRAS, and checks both that a float
comes back in a plausible band and that the extension's machine-readable
approximation-guarantee NOTICE (relative (eps, delta) over the clause count) is
surfaced in the response, which is what the Studio eval strip renders.
"""
from __future__ import annotations

import psycopg


def _dnf_root(test_dsn: str) -> str:
    """Build (x1 AND x2) OR (x3 AND x4) with 0.5 leaves; return the root token.

    Disjoint clauses, so the exact probability is 1-(1-0.25)(1-0.25) = 0.4375.
    """
    dsn = f"{test_dsn} options='-c search_path=public,provsql'"
    with psycopg.connect(dsn, autocommit=True) as conn, conn.cursor() as cur:
        cur.execute("SET provsql.provenance = 'semiring'")
        cur.execute("DROP TABLE IF EXISTS kl_dnf CASCADE")
        cur.execute("CREATE TABLE kl_dnf(id int)")
        cur.execute("INSERT INTO kl_dnf SELECT generate_series(1, 4)")
        cur.execute("SELECT add_provenance('kl_dnf')")
        cur.execute("DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM kl_dnf; END $$")
        cur.execute(
            "SELECT provenance_plus(ARRAY["
            "  provenance_times((SELECT provsql FROM kl_dnf WHERE id=1),"
            "                   (SELECT provsql FROM kl_dnf WHERE id=2)),"
            "  provenance_times((SELECT provsql FROM kl_dnf WHERE id=3),"
            "                   (SELECT provsql FROM kl_dnf WHERE id=4))])::text")
        row = cur.fetchone()
    assert row, "DNF witness produced no token"
    return row[0]


def test_evaluate_karp_luby_returns_float_with_guarantee(client, test_dsn):
    root = _dnf_root(test_dsn)
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "probability",
        "method": "karp-luby",
        "arguments": "eps=0.1,delta=0.05",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "float"
    # Exact 0.4375; a relative eps=0.1 estimate stays in a comfortable band.
    assert 0.30 < float(data["result"]) < 0.55, data
    # The extension emits the (eps, delta) guarantee as a NOTICE at the
    # verbose_level Studio floors evaluation at; it must reach the response so
    # the eval strip can render the bound.
    notices = " ".join(data.get("notices") or [])
    assert "approximation-guarantee:" in notices, notices
    assert "kind=relative" in notices, notices
    assert "clauses=2" in notices, notices
