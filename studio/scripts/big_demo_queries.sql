-- Canned queries for ProvSQL Studio rendering / circuit stress tests.
-- Loader: studio/scripts/load_demo_big.sql.
--
-- Each block below targets one stress axis. Copy-paste a single block into
-- the studio querybox; the loader does NOT auto-execute these.

------------------------------------------------------------------------
-- 1. RESULT-TABLE STRESS: the full table, ~50k rows.
--    Today: the front-end renders every row. Browser may stall.
--    With the cap: shows the first N rows + a truncation footer.
------------------------------------------------------------------------
SELECT * FROM bench_events;

------------------------------------------------------------------------
-- 2. RESULT-TABLE STRESS (filtered): ~10k rows after a filter.
--    Same axis, smaller blast radius.
------------------------------------------------------------------------
SELECT * FROM bench_events WHERE severity >= 3;

------------------------------------------------------------------------
-- 3. CIRCUIT-CAP TRIPPER (high fan-in): each agg_token aggregates over
--    ~10000 input gates -> BFS at depth 1 sees thousands of children
--    and trips the 200-node cap immediately.
--    Click any agg_token cell in circuit mode to surface the 413 path.
------------------------------------------------------------------------
SELECT severity, COUNT(*), SUM(cost), AVG(cost)
FROM bench_events
GROUP BY severity;

------------------------------------------------------------------------
-- 4. JOIN CIRCUIT (moderate): the per-row provenance is times(eq(...),
--    input). Click any provsql cell to inspect the structure under the
--    cap.
------------------------------------------------------------------------
SELECT e.id, e.region, d.display, e.severity, e.cost
FROM bench_events e JOIN bench_dim d ON e.region = d.region
WHERE e.severity = 4;

------------------------------------------------------------------------
-- 5. SHARED-SUBGRAPH CIRCUIT: UNION of two filters that overlap on
--    bench_events. Same input gates appear under two parents -> tests
--    that circuit_subgraph emits one row per (parent, child) edge and
--    that the front-end dedups by node id.
------------------------------------------------------------------------
SELECT id, region, severity FROM bench_events WHERE severity = 4
UNION
SELECT id, region, severity FROM bench_events WHERE region = 'EU' AND severity >= 3;

------------------------------------------------------------------------
-- 6. PROBABILITY EVALUATION at scale. After running, click the provsql
--    cell on any row, switch the eval strip to:
--      semiring = probability, method = monte-carlo, args = 1000
--    and observe the runtime + Hoeffding bound on a non-trivial circuit.
------------------------------------------------------------------------
SELECT region, severity, COUNT(*) AS n
FROM bench_events
WHERE severity >= 3
GROUP BY region, severity;

------------------------------------------------------------------------
-- 7. MAPPING-BASED EVAL: counting / formula semirings against the
--    bench_severity_mapping (value INTEGER) shipped by the loader.
--    Click an agg_token, pick semiring = counting and the
--    bench_severity_mapping mapping in the dropdown.
------------------------------------------------------------------------
SELECT region, COUNT(DISTINCT severity) AS distinct_severities
FROM bench_events
WHERE severity >= 2
GROUP BY region;

------------------------------------------------------------------------
-- 8. WHERE-MODE SIDEBAR STRESS: a narrow filter on a tracked relation.
--    The relations sidebar still tries to render all 50000 rows of
--    bench_events. With the per-relation cap: shows first M, with a
--    "showing M of 50000" affordance.
------------------------------------------------------------------------
SELECT id, region, severity FROM bench_events WHERE id = 1234;
