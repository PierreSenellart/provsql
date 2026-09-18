#!/usr/bin/env python3
"""Cost of provenance tracking: plain SQL against ProvSQL, query by query.

For each query of queries.py:
  plain   provsql.active = off, best of 3;
  cold    provsql.active = on, input tokens fresh (no gate of the query exists);
  warm    same query again (every gate already exists), best of 2;
  profile a second cold run with track_functions = all, giving the time spent
          in each ProvSQL SQL function (self time), and the EXPLAIN node times.
Times come from EXPLAIN (ANALYZE): no row is sent to the client, the target
list is computed.  Planning time includes the ProvSQL rewriting.
"""
import json, sys, time, argparse
import psycopg2
from queries import QUERIES

TRACKED = [("r", "r0"), ("s", "s0"), ("d", "d0"), ("edge", "edge0")]

def connect(db):
    c = psycopg2.connect(dbname=db)
    c.autocommit = True
    cur = c.cursor()
    cur.execute("SET search_path TO public, provsql")
    cur.execute("SET client_min_messages = warning")
    cur.execute("SET max_parallel_workers_per_gather = 0")
    cur.execute("SET jit = off")
    cur.execute("SET work_mem = '256MB'")
    return c, cur

def refresh(cur):
    """New copies of the tracked tables: every input token is fresh."""
    for t, src in TRACKED:
        cur.execute(f"DROP TABLE IF EXISTS {t} CASCADE")
        cur.execute(f"CREATE TABLE {t} AS SELECT * FROM {src}")
        cur.execute(f"SELECT add_provenance('{t}')")
        cur.execute(f"VACUUM FULL {t}")
    cur.execute("CREATE INDEX ON r(a)"); cur.execute("CREATE INDEX ON s(a)")
    cur.execute("CREATE INDEX ON d(a)"); cur.execute("CREATE INDEX ON edge(src)")
    for t, _ in TRACKED:
        cur.execute(f"ANALYZE {t}")

def explain(cur, sql, timing):
    cur.execute("EXPLAIN (ANALYZE, FORMAT JSON, TIMING %s) %s"
                % ("ON" if timing else "OFF", sql))
    j = cur.fetchone()[0]
    if isinstance(j, str):
        j = json.loads(j)
    j = j[0]
    return {"plan_ms": j["Planning Time"], "exec_ms": j["Execution Time"],
            "rows": j["Plan"]["Actual Rows"], "plan": j["Plan"]}

def funcstats(cur):
    cur.execute("SELECT pg_stat_force_next_flush()")
    cur.execute("SELECT 1")
    time.sleep(0.2)
    cur.execute("SELECT pg_stat_clear_snapshot()")
    cur.execute("""SELECT funcname, calls, total_time, self_time
                   FROM pg_stat_user_functions WHERE schemaname = 'provsql'""")
    return {f: (c, t, s) for f, c, t, s in cur.fetchall()}

def nb_gates(cur):
    cur.execute("SELECT get_nb_gates()")
    return cur.fetchone()[0]

def strip(plan):
    """Keep per-node exclusive time out of an EXPLAIN plan tree."""
    out = []
    def walk(n, depth):
        kids = n.get("Plans", [])
        tot = n.get("Actual Total Time", 0) * n.get("Actual Loops", 1)
        ex = tot - sum(k.get("Actual Total Time", 0) * k.get("Actual Loops", 1) for k in kids)
        out.append({"depth": depth, "node": n["Node Type"],
                    "rel": n.get("Relation Name") or n.get("Alias") or "",
                    "rows": n.get("Actual Rows"), "excl_ms": round(ex, 2)})
        for k in kids:
            walk(k, depth + 1)
    walk(plan, 0)
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="provsql_bench")
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--out", default="results.json")
    ap.add_argument("--set", nargs="*", default=[], help="GUC=value, applied to the session")
    a = ap.parse_args()
    conn, cur = connect(a.db)
    for kv in a.set:
        k, v = kv.split("=", 1)
        cur.execute(f"SET {k} = %s", (v,))
    results = []
    for name, feature, sql in QUERIES:
        if a.only and name not in a.only:
            continue
        res = {"name": name, "feature": feature, "sql": sql}
        try:
            refresh(cur)
            cur.execute("SET track_functions = 'none'")
            cur.execute("SET provsql.active = off")
            plain = [explain(cur, sql, False) for _ in range(3)]
            res["plain_exec_ms"] = min(p["exec_ms"] for p in plain)
            res["plain_plan_ms"] = min(p["plan_ms"] for p in plain)
            res["plain_rows"] = plain[0]["rows"]
            cur.execute("SET provsql.active = on")
            g0 = nb_gates(cur)
            cold = explain(cur, sql, False)
            res["gates_created"] = nb_gates(cur) - g0
            res["cold_exec_ms"], res["cold_plan_ms"] = cold["exec_ms"], cold["plan_ms"]
            res["prov_rows"] = cold["rows"]
            warm = [explain(cur, sql, False) for _ in range(2)]
            res["warm_exec_ms"] = min(w["exec_ms"] for w in warm)
            res["warm_plan_ms"] = min(w["plan_ms"] for w in warm)
            # profile run
            refresh(cur)
            cur.execute("SET track_functions = 'all'")
            f0 = funcstats(cur)
            prof = explain(cur, sql, True)
            f1 = funcstats(cur)
            res["profile_exec_ms"], res["profile_plan_ms"] = prof["exec_ms"], prof["plan_ms"]
            fs = {}
            for f, (c, t, s) in f1.items():
                c0, t0, s0 = f0.get(f, (0, 0, 0))
                if c - c0:
                    fs[f] = {"calls": c - c0, "total_ms": round(t - t0, 2),
                             "self_ms": round(s - s0, 2)}
            res["functions"] = fs
            res["nodes"] = strip(prof["plan"])
        except Exception as e:
            res["error"] = str(e).strip().split("\n")[0]
            conn.rollback() if not conn.autocommit else None
        results.append(res)
        if "error" in res:
            print(f"{name:24s} ERROR {res['error']}", flush=True)
        else:
            print(f"{name:24s} plain {res['plain_exec_ms']:9.1f}  cold {res['cold_exec_ms']:9.1f}"
                  f"  warm {res['warm_exec_ms']:9.1f}  plan {res['cold_plan_ms']:8.1f}"
                  f"  gates {res['gates_created']:8d}  rows {res['prov_rows']}", flush=True)
        json.dump(results, open(a.out, "w"), indent=1)

if __name__ == "__main__":
    main()
