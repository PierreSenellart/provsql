\set ECHO none
\if `which d4 > /dev/null 2>&1 && echo true || echo false`
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- A 3-way times circuit (P = 0.5 * 0.6 * 0.7 = 0.21). Higher than the
-- 1-2-3 join so the Monte-Carlo estimate has tight relative variance.
CREATE TABLE probability_benchmark_q AS
SELECT provenance() AS prov
FROM personnel p1, personnel p2, personnel p3
WHERE p1.id=5 AND p2.id=6 AND p3.id=7;

SELECT remove_provenance('probability_benchmark_q');

-- Make the planner-hook stand back: probability_benchmark is a
-- @c RETURNS TABLE function with multiple OUT attributes and the
-- rewriter does not yet support that shape. Seed Monte Carlo so the
-- output stays deterministic.
SET provsql.active = off;
SET provsql.monte_carlo_seed = 42;
-- probability_benchmark always runs every method; restrict the
-- displayed subset to the ones that are guaranteed-available on the
-- d4-gated CI hosts (the rest are exercised in the Studio backend
-- tests where missing compilers surface as per-row errors).
SELECT method, args, ROUND(probability::numeric, 3) AS prob, error
FROM probability_benchmark_q t, LATERAL provsql.probability_benchmark(
       t.prov, 10000) pb
WHERE method IN ('independent', 'tree-decomposition', 'possible-worlds', 'monte-carlo')
   OR (method = 'compilation' AND args = 'd4')
ORDER BY method, args NULLS FIRST;
SET provsql.monte_carlo_seed = -1;
SET provsql.active = on;

DROP TABLE probability_benchmark_q;
\else
\echo 'SKIPPING: d4 not available'
\endif
