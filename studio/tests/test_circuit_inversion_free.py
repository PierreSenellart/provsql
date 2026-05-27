"""End-to-end tests for the inversion-free rendering through /api/circuit.

A certified circuit is built in the test database, then fetched through the
real Flask circuit endpoint; the returned scene must elide the transparent
gate_annotation wrappers and expose the IF markers (certificate on the result
root, order key + rank on each certified leaf), including a node that carries
both the Boolean (B) and inversion-free (IF) markers.
"""
from __future__ import annotations

import psycopg


def _inversion_free_witness(test_dsn: str) -> str:
    """Build the non-read-once self-join witness S(x,y),A(x,y),S(x,z),B(x,z)
    under the inversion-free analysis and return its per-row provenance token.
    """
    dsn = f"{test_dsn} options='-c search_path=public,provsql'"
    with psycopg.connect(dsn, autocommit=True) as conn, conn.cursor() as cur:
        cur.execute("SET provsql.inversion_free = on")
        cur.execute("SET provsql.boolean_provenance = off")
        cur.execute("DROP TABLE IF EXISTS ifw_s, ifw_a, ifw_b CASCADE")
        cur.execute("CREATE TABLE ifw_s(x int, c2 int)")
        cur.execute("CREATE TABLE ifw_a(x int, c2 int)")
        cur.execute("CREATE TABLE ifw_b(x int, c2 int)")
        cur.execute("INSERT INTO ifw_s VALUES (1,10),(1,20),(1,30)")
        cur.execute("INSERT INTO ifw_a VALUES (1,10),(1,20)")
        cur.execute("INSERT INTO ifw_b VALUES (1,20),(1,30)")
        for t in ("ifw_s", "ifw_a", "ifw_b"):
            cur.execute("SELECT add_provenance(%s)", (t,))
        cur.execute("DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM ifw_s; "
                    "PERFORM set_prob(provsql, 0.5) FROM ifw_a; "
                    "PERFORM set_prob(provsql, 0.5) FROM ifw_b; END $$")
        cur.execute(
            "SELECT provenance()::text "
            "FROM ifw_s s1, ifw_a a, ifw_s s2, ifw_b b "
            "WHERE s1.x=a.x AND s1.c2=a.c2 AND s1.x=s2.x "
            "  AND s2.x=b.x AND s2.c2=b.c2 GROUP BY s1.x")
        row = cur.fetchone()
    assert row, "witness produced no row"
    return row[0]


def test_circuit_inversion_free_root_carries_certificate(client, test_dsn):
    root = _inversion_free_witness(test_dsn)
    data = client.get(f"/api/circuit/{root}").get_json()
    nodes = data["nodes"]
    # every transparent gate_annotation wrapper is elided from the scene
    assert all(n["type"] != "annotation" for n in nodes)
    # at least one node carries the certificate (the elided root annotation)
    certs = [n for n in nodes if n.get("if_cert")]
    assert certs, "expected a node carrying the inversion-free certificate"
    for n in certs:
        assert n["inversion_free"] is True
        c = n["if_cert"]
        assert c["natoms"] == 4 and c["nclasses"] == 3
        assert c["root_class"] == 0 and c["class_order"] == [0, 1, 2]


def test_circuit_inversion_free_leaves_carry_order_keys(client, test_dsn):
    root = _inversion_free_witness(test_dsn)
    data = client.get(f"/api/circuit/{root}").get_json()
    keyed = [n for n in data["nodes"] if n.get("if_key")]
    assert keyed, "expected certified leaves with order keys"
    # ranks are a contiguous 0..k-1 prefix (the scene-local Prop. 4.5 order)
    ranks = sorted(n["if_key"]["rank"] for n in keyed)
    assert ranks == list(range(len(ranks)))
    # the self-join witness has shared guard atoms (factor -1) and payloads
    factors = {n["if_key"]["factor"] for n in keyed}
    assert -1 in factors and any(f >= 0 for f in factors)

# NB: the renderer's ability to stack a B and an IF badge on one surviving node
# (when both wrapper kinds elide onto it) is covered by the pure-row unit test
# test_elide_markers_stacks_b_and_if_on_one_node in test_circuit_markers.py.  We
# do not build such a circuit live here: the only way to force one is
# assume_boolean() over a non-read-once token, which falsely asserts a safe
# (read-once) rewrite on a circuit that is not independent.
