\set ECHO none
\pset format unaligned

-- ==========================================================================
-- Comparison readout over a recursive random_variable CTE (PG15+ only, like
-- the other WITH RECURSIVE tests -- see test/schedule.15).
--
-- The recursion is UNION ALL: RVs cannot be set-UNIONed (their btree
-- comparator raises -- a distribution has no ordering), so a UNION ALL bag
-- recursion is the only shape.  It carries no token provenance, so the planner
-- hook leaves the whole WITH intact for native execution while the outer
-- expected(cost <= c) comparison is lifted into its event token.
--
-- Diamond: e0 1->2 T0 ~ N(10,2) SHARED, e1 2->4 T1 ~ N(5,1), e2 2->4
-- T2 ~ N(6,1.5); cost = min(T0+T1, T0+T2).  The shared T0 must stay coupled
-- through the recursion and the min: correlated Pr(cost <= 15) ~ 0.558 and
-- E[cost] ~ 14.67 (NOT the independent-leaf 14.10).  MC-backed (seed pinned).
-- ==========================================================================

SET provsql.active = on;
SET provsql.monte_carlo_seed = 42;
SET provsql.rv_mc_samples = 200000;
SET search_path TO public, provsql;

CREATE TABLE edge_rv(frm int, tto int, t random_variable);
INSERT INTO edge_rv VALUES
  (1, 2, normal(10, 2)),
  (2, 4, normal(5, 1)),
  (2, 4, normal(6, 1.5));

WITH RECURSIVE pc(node, cost, hops, vis) AS (
    SELECT 1, 0::random_variable, 0, ARRAY[1]
  UNION ALL
    SELECT e.tto, pc.cost + e.t, pc.hops + 1, pc.vis || e.tto
    FROM pc JOIN edge_rv e ON e.frm = pc.node
    WHERE pc.hops < 5 AND NOT (e.tto = ANY(pc.vis))
),
m AS (SELECT min(cost) AS cost FROM pc WHERE node = 4)
SELECT abs(expected(cost <= 15) - 0.558) < 0.02 AS recursive_ontime_ok,
       abs(expected(cost)      - 14.67) < 0.1   AS recursive_mean_shared_ok
  FROM m;

DROP TABLE edge_rv;

SELECT 'ok'::text AS continuous_recursive_cte_done;
