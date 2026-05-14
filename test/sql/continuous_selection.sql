\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Determinism for every Monte Carlo call below.
SET provsql.monte_carlo_seed = 42;

CREATE TABLE sensors(id text, reading provsql.random_variable);
INSERT INTO sensors VALUES
  ('s1', provsql.normal(2.5, 0.5)),
  ('s2', provsql.uniform(1, 3));
SELECT add_provenance('sensors');

-- ====================================================================
-- A: provenance-tracked table with WHERE on a random_variable column.
-- The TODO's anchor: WHERE reading > 2 lifts into provenance,
-- expect P(s1)=0.84..., P(s2)=0.5 (within 0.02 of truth at n=100k).
-- The standard idiom is: capture into a table, remove_provenance,
-- then SELECT to see clean output.  Without this, the planner hook
-- adds a provsql column to the outer SELECT and the UUIDs in the
-- output are non-deterministic.
-- ====================================================================
CREATE TABLE result_a AS
  SELECT id,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - CASE id WHEN 's1' THEN 0.8413 WHEN 's2' THEN 0.5 END) < 0.02
         AS within_tolerance
    FROM sensors WHERE reading > 2;
SELECT remove_provenance('result_a');
SELECT * FROM result_a ORDER BY id;
DROP TABLE result_a;

-- ====================================================================
-- B: mixed WHERE -- non-RV conjunct stays in WHERE and filters rows;
-- RV conjunct lifts into provenance.  Only s1 survives the id filter.
-- ====================================================================
CREATE TABLE result_b AS
  SELECT id,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - 0.8413) < 0.02 AS within_tolerance
    FROM sensors WHERE id = 's1' AND reading > 2;
SELECT remove_provenance('result_b');
SELECT * FROM result_b ORDER BY id;
DROP TABLE result_b;

-- ====================================================================
-- C: NOT(reading > 2) -- De Morgan flips the comparator at the gate
-- level.  P(N(2.5,0.5) <= 2) = 1 - 0.8413 = 0.1587.
-- ====================================================================
CREATE TABLE result_c AS
  SELECT id,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - CASE id WHEN 's1' THEN 0.1587 WHEN 's2' THEN 0.5 END) < 0.02
         AS within_tolerance
    FROM sensors WHERE NOT (reading > 2);
SELECT remove_provenance('result_c');
SELECT * FROM result_c ORDER BY id;
DROP TABLE result_c;

-- ====================================================================
-- D: OR over two RV comparisons.  The De Morgan walker emits
-- provenance_plus when not negated; this is a Boolean OR (with the
-- usual circuit-level inclusion-exclusion handled by downstream
-- evaluators).
-- For s1 ~ N(2.5, 0.5):
--   P(reading > 3) = 0.1587, P(reading < 1) ~= 0.0013, OR ~ 0.16.
-- For s2 ~ U(1, 3):
--   P(reading > 3) = 0,      P(reading < 1) = 0,       OR  = 0.
-- The events are disjoint here, so monte-carlo on the gate_plus is
-- exact for s2 and within MC noise for s1.
-- ====================================================================
CREATE TABLE result_d AS
  SELECT id,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - CASE id WHEN 's1' THEN 0.1600 WHEN 's2' THEN 0.0 END) < 0.03
         AS within_tolerance
    FROM sensors WHERE reading > 3 OR reading < 1;
SELECT remove_provenance('result_d');
SELECT * FROM result_d ORDER BY id;
DROP TABLE result_d;

-- ====================================================================
-- E: WHERE on RV arithmetic -- reading + 1 > 3 is reading > 2 in
-- disguise.  The lifted gate_cmp's left child is a gate_arith.
-- ====================================================================
CREATE TABLE result_e AS
  SELECT id,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - CASE id WHEN 's1' THEN 0.8413 WHEN 's2' THEN 0.5 END) < 0.02
         AS within_tolerance
    FROM sensors WHERE reading + 1 > 3;
SELECT remove_provenance('result_e');
SELECT * FROM result_e ORDER BY id;
DROP TABLE result_e;

DROP TABLE sensors;

-- ====================================================================
-- F: untracked table (no add_provenance) -- WHERE on the RV column
-- still produces a result with a provenance column, synthesised from
-- gate_one * gate_cmp = gate_cmp.
-- ====================================================================
CREATE TABLE untracked(id text, reading provsql.random_variable);
INSERT INTO untracked VALUES
  ('u1', provsql.normal(0, 1)),
  ('u2', provsql.normal(0, 1));

CREATE TABLE result_f AS
  SELECT id,
         abs(probability_evaluate(provenance(), 'monte-carlo', '10000')
             - 0.5) < 0.02 AS within_tolerance
    FROM untracked WHERE reading > 0;
SELECT remove_provenance('result_f');
SELECT * FROM result_f ORDER BY id;
DROP TABLE result_f;

DROP TABLE untracked;

-- ====================================================================
-- G: full-stack composition -- WHERE on RV + GROUP BY (duplicate
-- elimination) + HAVING on the aggregate count.  Verifies the
-- planner-hook RV rewrite slots cleanly between the existing
-- aggregation rewriting (replaces aggregates with
-- provenance_aggregate, builds an agg_token), the array_agg combiner
-- that GROUP BY uses to fold within-group provenances, and the
-- HAVING walker that emits a provenance_cmp on the agg result.  The
-- resulting provsql is a multi-layer circuit:
--   cmp(HAVING) -> agg(count over array_agg(times(row.provsql,
--                                                 cmp(val > 1))))
-- We verify this STRUCTURE rather than running probability_evaluate
-- because the RV-aware Monte Carlo sampler does not yet pre-resolve
-- HAVING gate_agg children (the legacy boolean MC path does this via
-- @c provsql_having in @c CircuitFromMMap.cpp; the RV path can grow
-- the equivalent later).  Probability over WHERE + GROUP BY without
-- HAVING is exercised by section H below.
-- ====================================================================
CREATE TABLE readings(category text, val provsql.random_variable);
INSERT INTO readings VALUES
  ('hot',  provsql.normal(2, 1)),
  ('hot',  provsql.normal(2, 1)),
  ('warm', provsql.uniform(0, 2)),
  ('warm', provsql.uniform(0, 2));
SELECT add_provenance('readings');

-- Capture the per-group provsql so we can probe its shape.
CREATE TABLE result_g AS
  SELECT category FROM readings WHERE val > 1
    GROUP BY category HAVING count(*) > 1;
-- The added provsql column is the HAVING cmp at root.  Walk down:
-- (a) root is gate_cmp;
-- (b) the cmp's left child is gate_agg (the HAVING aggregate);
-- (c) at the bottom of the agg's semimod children, the per-row token
--     is a gate_times with a gate_cmp(val > 1) factor folded in by
--     the WHERE rewrite (proves the RV cmp lifted into per-row
--     provenance, not stranded at group level).
SELECT
  (SELECT bool_and(get_gate_type(provsql) = 'cmp')
     FROM result_g)                                          AS root_is_cmp,
  (SELECT bool_and(get_gate_type((get_children(provsql))[1])
                   = 'agg')
     FROM result_g)                                          AS having_lhs_is_agg,
  -- Drill down: the agg's first child is a semimod whose k_gate
  -- (semimod_extract_M_and_K's k) is a gate_times wrapping the
  -- per-row provsql AND the gate_cmp built by the WHERE rewrite.
  (SELECT bool_and(get_gate_type(
      (get_children(
         (get_children((get_children(provsql))[1]))[1])
      )[1]) = 'times')
     FROM result_g)                                          AS per_row_token_is_times,
  -- One of the times's children is the gate_cmp from `val > 1`.
  (SELECT bool_and('cmp' = ANY(
      ARRAY(SELECT get_gate_type(c)
              FROM unnest(get_children(
                (get_children(
                   (get_children((get_children(provsql))[1]))[1]))[1])
              ) c)))
     FROM result_g)                                          AS row_times_has_cmp_factor;
SELECT remove_provenance('result_g');
SELECT category FROM result_g ORDER BY category;
DROP TABLE result_g;

-- ====================================================================
-- H: WHERE on RV + GROUP BY (no HAVING).  Probability check for the
-- full pipeline excluding HAVING, since HAVING + RV does not yet run
-- under MC (see section G).  P(group fires) = P(at least one row
-- survives WHERE):
--   hot:  1 - (1 - 0.8413)^2 ~= 0.9748
--   warm: 1 - (1 - 0.5)^2     = 0.75
-- ====================================================================
CREATE TABLE result_h AS
  SELECT category,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - CASE category
                 WHEN 'hot'  THEN 0.9748
                 WHEN 'warm' THEN 0.75
               END) < 0.03 AS within_tolerance
    FROM readings WHERE val > 1
    GROUP BY category;
SELECT remove_provenance('result_h');
SELECT * FROM result_h ORDER BY category;
DROP TABLE result_h;

DROP TABLE readings;

-- ====================================================================
-- I: mixed RV / non-RV inside a single Boolean expression is
-- explicitly rejected with a clear error -- the planner cannot lift
-- the RV part without lifting the deterministic part too, and the
-- latter would require synthesising a Boolean RV gate (CASE WHEN ...
-- THEN gate_one() ELSE gate_zero() END), which is deferred work.
-- This mirrors agg_token's check_expr_on_aggregate behaviour for
-- mixed WHERE clauses.
-- ====================================================================
CREATE TABLE mix(id text, reading provsql.random_variable);
INSERT INTO mix VALUES ('m1', provsql.normal(0, 1));
SELECT add_provenance('mix');
\set VERBOSITY terse
SELECT id FROM mix WHERE reading > 0 OR id = 'm1';
\set VERBOSITY default
DROP TABLE mix;

-- ====================================================================
-- J: WHERE on a random_variable with NO add_provenance()'d source
-- AND NO provenance() in the SELECT.  Before the has_provenance
-- walker recursed with itself (rather than the limited
-- provenance_function_walker), the rv-comparison OpExpr inside the
-- WHERE clause was invisible to the gate, so the planner hook was
-- skipped and the executor reached random_variable_cmp_placeholder.
-- The fix is the smallest case that exercises pure-RV provenance
-- synthesis: no tracked relation, no provenance() call, just a CTE
-- producing an RV and a WHERE comparing it to a constant.
-- ====================================================================
CREATE TABLE result_j AS
  WITH x(u) AS (SELECT uniform(0, 1))
  SELECT 1 AS one FROM x WHERE u > 0.5;
-- Two structural checks before removing the provenance column:
--  (a) row count: the rewriter must let the row through (WHERE rewritten
--      to TRUE, the lifted comparison goes to the provsql column).
--  (b) the root gate is the cmp itself, NOT a single-child times wrapping
--      it.  provenance_times(gate_one(), cmp) used to leave that useless
--      wrapper because the CASE dispatched on the unfiltered token count
--      and missed the [one, cmp] -> [cmp] collapse.
-- SET LOCAL provsql.active=off so the rewriter doesn't auto-add a fresh
-- provsql column to this very SELECT and wrap count(*) in agg_token.
BEGIN;
SET LOCAL provsql.active = off;
SELECT count(*) AS rows_returned,
       bool_and(get_gate_type(provsql) = 'cmp') AS root_is_cmp
  FROM result_j;
COMMIT;
SELECT remove_provenance('result_j');
DROP TABLE result_j;

-- ====================================================================
-- K: FROM-less SELECT with an rv_cmp in WHERE.  The planner hook used
-- to short-circuit on `q->commandType == CMD_SELECT && q->rtable`, so
-- a query with no FROM never even reached has_provenance and the
-- placeholder fired.  After widening the gate to drop the rtable
-- check, the rewriter lifts the comparison into the synthesised
-- provsql column the same way it does for FROM-bearing queries.
-- ====================================================================
CREATE TABLE result_k AS
  SELECT 1 AS one WHERE normal(0, 1) > 2;
BEGIN;
SET LOCAL provsql.active = off;
SELECT count(*) AS rows_returned,
       bool_and(get_gate_type(provsql) = 'cmp') AS root_is_cmp
  FROM result_k;
COMMIT;
SELECT remove_provenance('result_k');
DROP TABLE result_k;

RESET provsql.monte_carlo_seed;

SELECT 'ok'::text AS continuous_selection_done;
