\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- ----------------------------------------------------------------------
-- Scalar (no GROUP BY) aggregation: the empty input is a real possible
-- world (one row, count 0), so a true-on-empty count HAVING predicate
-- (= 0, < k, <= k) must INCLUDE it -- unlike a grouped aggregate, where
-- the empty group is no row.  The agg gate carries a scalar flag
-- (info2 high bit, set by provenance_aggregate) that the probability cmp
-- evaluators read to add probZero = prod(1 - p_i) when 0 satisfies the
-- predicate.  Regression for the empty-group under-count.
--
-- t has four independent tuples at p = 0.5, so over the whole table
-- count ~ Binomial(4, 0.5):  P(count=0)=1/16, P(count=1)=4/16, ...
-- ----------------------------------------------------------------------

DROP TABLE IF EXISTS seh_t CASCADE;
CREATE TABLE seh_t(x int);
INSERT INTO seh_t VALUES (10),(20),(30),(40);
SELECT add_provenance('seh_t');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM seh_t; END $$;

-- True-on-empty predicates now count the empty world:
--   count(*) = 0   -> P(count=0)            = 0.0625
--   count(*) < 2   -> P(count<=1)           = 0.0625 + 0.25 = 0.3125
--   count(*) <= 1  -> same                  = 0.3125
-- False-on-empty predicates are unchanged (the empty world fails anyway):
--   count(*) >= 1  -> P(count>=1) = 1-1/16  = 0.9375
--   count(*) = 2   -> P(count=2)            = 0.3750
-- Tautologies (true in every world, empty included) are gate_one = 1 for a
-- scalar aggregate -- NOT "group is non-empty" (0.9375), which is the grouped
-- reading.  count(*) >= 0 and count(*) > -1 are resolved by the always-true
-- rewriter, which is scalar-aware.  count(*) <> 0 excludes the empty world
-- (0 = 0 fails <>0), so it stays 0.9375.
DO $$
DECLARE r record; p numeric;
BEGIN
  FOR r IN SELECT * FROM (VALUES
      ('count(*) = 0',  0.0625),
      ('count(*) < 2',  0.3125),
      ('count(*) <= 1', 0.3125),
      ('count(*) >= 1', 0.9375),
      ('count(*) = 2',  0.3750),
      ('count(*) >= 0', 1.0000),
      ('count(*) > -1', 1.0000),
      ('count(*) <> 0', 0.9375)) v(pred, truth)
  LOOP
    EXECUTE format(
      'CREATE TEMP TABLE seh_r AS SELECT probability_evaluate(provenance()) AS pp '
      'FROM seh_t HAVING %s', r.pred);
    PERFORM remove_provenance('seh_r');
    SELECT round(pp::numeric, 4) INTO p FROM seh_r;
    RAISE NOTICE 'scalar HAVING %  p=%  (truth %)', rpad(r.pred, 14), p, r.truth;
    DROP TABLE seh_r;
  END LOOP;
END $$;

-- Monte Carlo and the cmp pre-pass must agree (both include the empty world).
-- All routes agree (each independently counts the empty world):
--   cmp-on  : the closed-form cmp pre-pass (CountCmpEvaluator)
--   cmp-off : the generic enumeration / Boolean expansion (count_enum)
--   mc      : Monte Carlo samples the all-absent world directly
SET provsql.monte_carlo_seed = 1234;
CREATE TEMP TABLE seh_tk AS SELECT provenance() AS pv FROM seh_t HAVING count(*) = 0;
SELECT remove_provenance('seh_tk');
SELECT 'count=0 cmp-on' AS shape,
       round(probability_evaluate((SELECT pv FROM seh_tk))::numeric, 4) AS p;
SET provsql.cmp_probability_evaluation = off;
SELECT 'count=0 cmp-off' AS shape,
       round(probability_evaluate((SELECT pv FROM seh_tk))::numeric, 4) AS p;
SET provsql.cmp_probability_evaluation = on;
SELECT 'count=0 mc (1dp)' AS shape,
       round(probability_evaluate((SELECT pv FROM seh_tk), 'monte-carlo', '200000')::numeric, 1) AS p;
DROP TABLE seh_tk;

-- Grouped aggregation is UNCHANGED: the empty group is no row, so a
-- true-on-empty predicate selects nothing from the empty world.
--   g=1 (two tuples): count<2 <=> exactly one present = 0.5
--   g=2,g=3 (one tuple each): count<2 <=> present = 0.5
DROP TABLE IF EXISTS seh_g CASCADE;
CREATE TABLE seh_g(g int, x int);
INSERT INTO seh_g VALUES (1,10),(1,20),(2,30),(3,40);
SELECT add_provenance('seh_g');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM seh_g; END $$;

CREATE TEMP TABLE seh_gr AS
  SELECT g, probability_evaluate(provenance()) AS pp
  FROM seh_g GROUP BY g HAVING count(*) < 2;
SELECT remove_provenance('seh_gr');
SELECT 'grouped count<2' AS shape, g, round(pp::numeric, 4) AS p
FROM seh_gr ORDER BY g;
DROP TABLE seh_gr;

-- IS NULL HAVING.  sum/avg/min/max/array_agg are NULL exactly when the
-- aggregate has no contributor, so IS NULL is the empty-group test and IS NOT
-- NULL its complement.  For a scalar aggregate the empty input is a real row:
--   agg IS NULL     -> 1 ⊖ ⊕(tokens) = probZero = 0.0625
--   agg IS NOT NULL -> δ(⊕(tokens))  = P(non-empty) = 0.9375
-- For a grouped aggregate IS NULL is gate_zero (a present group has a
-- contributor) and IS NOT NULL is the group's existence.  These are structural
-- (monus/delta), so route-independent.
DO $$
DECLARE r record; p numeric;
BEGIN
  FOR r IN SELECT * FROM (VALUES
      ('sum(x) IS NULL',        0.0625),
      ('max(x) IS NULL',        0.0625),
      ('avg(x) IS NULL',        0.0625),
      ('array_agg(x) IS NULL',  0.0625),
      ('max(x) IS NOT NULL',    0.9375),
      ('NOT (sum(x) IS NULL)',  0.9375)) v(pred, truth)
  LOOP
    EXECUTE format(
      'CREATE TEMP TABLE seh_n AS SELECT probability_evaluate(provenance()) AS pp '
      'FROM seh_t HAVING %s', r.pred);
    PERFORM remove_provenance('seh_n');
    SELECT round(pp::numeric, 4) INTO p FROM seh_n;
    RAISE NOTICE 'scalar HAVING %  p=%  (truth %)', rpad(r.pred, 20), p, r.truth;
    DROP TABLE seh_n;
  END LOOP;
END $$;

-- Grouped IS NOT NULL = group existence (g1 two tuples -> 0.75; g2,g3 -> 0.5).
CREATE TEMP TABLE seh_gn AS
  SELECT g, probability_evaluate(provenance()) AS pp
  FROM seh_g GROUP BY g HAVING max(x) IS NOT NULL;
SELECT remove_provenance('seh_gn');
SELECT 'grouped max IS NOT NULL' AS shape, g, round(pp::numeric, 4) AS p
FROM seh_gn ORDER BY g;
DROP TABLE seh_gn;

DROP TABLE seh_g CASCADE;

-- ----------------------------------------------------------------------
-- count(col) (as opposed to count(*)) with NULLs.  count(col) counts only
-- rows whose col IS NOT NULL, but -- unlike sum/min/max/avg -- its empty group
-- still has the real value 0, so a scalar true-on-empty count(col) predicate
-- (= 0, < k, <= k) must include the all-absent world AND every world in which
-- only NULL-valued rows are present (count(col) = 0 there too).  The agg gate
-- keeps the COUNT identity even though its per-row value is the 0/1 indicator
-- CASE WHEN col IS NOT NULL THEN 1 ELSE 0 END, so the evaluators route it to a
-- value-aware enumeration that keeps the scalar empty world.  Regression for
-- the under-count where a NULL-bearing count(col) was conflated with count(*).
--
-- seh_c: two non-NULL rows + two NULL rows, all p = 0.5.  count(x) ignores the
-- NULL rows entirely, so count(x) ~ Binomial(2, 0.5) over {10, 20}:
--   P(count(x)=0)=0.25, =1 -> 0.5, =2 -> 0.25.  The two NULL rows are free.
-- Contrast count(*)=0 = P(all four absent) = 0.0625: the gap is exactly the
-- NULL rows, which count(*) requires absent but count(x) does not.
DROP TABLE IF EXISTS seh_c CASCADE;
CREATE TABLE seh_c(x int);
INSERT INTO seh_c VALUES (10),(20),(NULL),(NULL);
SELECT add_provenance('seh_c');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM seh_c; END $$;

DO $$
DECLARE r record; p numeric;
BEGIN
  FOR r IN SELECT * FROM (VALUES
      ('count(x) = 0',  0.2500),   -- both non-NULL absent (NULL rows free)
      ('count(x) < 1',  0.2500),   -- same
      ('count(x) <= 1', 0.7500),   -- not both non-NULL present
      ('count(x) < 2',  0.7500),   -- same
      ('count(x) = 1',  0.5000),   -- exactly one non-NULL present
      ('count(x) >= 1', 0.7500),   -- at least one non-NULL (false-on-empty)
      ('count(x) <> 0', 0.7500),   -- excludes the count=0 worlds
      ('count(x) >= 0', 1.0000),   -- scalar tautology -> gate_one
      ('count(*) = 0',  0.0625)) v(pred, truth) -- contrast: NULL rows must be absent too
  LOOP
    EXECUTE format(
      'CREATE TEMP TABLE seh_cr AS SELECT probability_evaluate(provenance()) AS pp '
      'FROM seh_c HAVING %s', r.pred);
    PERFORM remove_provenance('seh_cr');
    SELECT round(pp::numeric, 4) INTO p FROM seh_cr;
    RAISE NOTICE 'scalar HAVING %  p=%  (truth %)', rpad(r.pred, 14), p, r.truth;
    DROP TABLE seh_cr;
  END LOOP;
END $$;

-- All routes agree on the empty-world-including count(x)=0 (cmp-on closed form
-- bails on the non-unit 0/1 values and defers to the generic value-aware
-- enumeration; cmp-off takes it directly; MC samples the empty + NULL-only
-- worlds).
SET provsql.monte_carlo_seed = 4242;
CREATE TEMP TABLE seh_ck AS SELECT provenance() AS pv FROM seh_c HAVING count(x) = 0;
SELECT remove_provenance('seh_ck');
SELECT 'count(x)=0 cmp-on' AS shape,
       round(probability_evaluate((SELECT pv FROM seh_ck))::numeric, 4) AS p;
SET provsql.cmp_probability_evaluation = off;
SELECT 'count(x)=0 cmp-off' AS shape,
       round(probability_evaluate((SELECT pv FROM seh_ck))::numeric, 4) AS p;
SET provsql.cmp_probability_evaluation = on;
-- 0.25 sits exactly on a 1-decimal rounding boundary, so assert a tolerance
-- around the exact value instead (deterministic under the fixed MC seed).
SELECT 'count(x)=0 mc within 0.02' AS shape,
       abs(probability_evaluate((SELECT pv FROM seh_ck), 'monte-carlo', '200000') - 0.25) < 0.02 AS ok;
DROP TABLE seh_ck;

-- The agg_token moment surface treats count(x) like a SUM of 0/1 indicators
-- (empty group = 0): expected(count(x)) = sum of per-row P(present & non-NULL)
-- = 0.5 + 0.5 = 1.0; support = [0, #non-NULL rows] = [0, 2].
CREATE TEMP TABLE seh_cm AS SELECT count(x) AS cx FROM seh_c;
SELECT remove_provenance('seh_cm');
SELECT 'expected(count(x))' AS shape, round(expected(cx)::numeric, 4) AS e FROM seh_cm;
SELECT 'support(count(x))' AS shape, lo, hi FROM support((SELECT cx FROM seh_cm));
DROP TABLE seh_cm;

-- Grouped count(col): the empty group is no row, but a present group whose
-- counted column is all-NULL has count(x)=0 and must be kept (its existence is
-- the group's, not gate_zero).  seh_cg: group 1 = {10, NULL}, group 2 = {NULL}.
--   g1 count(x)=0 <=> 10 absent AND (NULL present, so the group exists) = 0.25
--   g2 count(x)=0 <=> its only (NULL) row present                       = 0.5
DROP TABLE IF EXISTS seh_cg CASCADE;
CREATE TABLE seh_cg(g int, x int);
INSERT INTO seh_cg VALUES (1,10),(1,NULL),(2,NULL);
SELECT add_provenance('seh_cg');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM seh_cg; END $$;
CREATE TEMP TABLE seh_cgr AS
  SELECT g, probability_evaluate(provenance()) AS pp
  FROM seh_cg GROUP BY g HAVING count(x) = 0;
SELECT remove_provenance('seh_cgr');
SELECT 'grouped count(x)=0' AS shape, g, round(pp::numeric, 4) AS p
FROM seh_cgr ORDER BY g;
DROP TABLE seh_cgr;
DROP TABLE seh_cg CASCADE;
DROP TABLE seh_c CASCADE;

-- Scalar-aggregation existence = gate_one: a scalar aggregate always returns one
-- row (count 0 / sum-min-max NULL over the empty input), so its provenance() is
-- certain (p=1), not P(non-empty).  And the agg_token moment surface excludes the
-- empty world for NULL-on-empty aggregates (min/max): expected(min) is conditional
-- on non-empty by default (min over the empty world is NULL, not +Infinity), so it
-- is finite.  3 rows v=10/20/30 present with p=0.5/0.4/0.3:
--   existence of count(*)  = 1.0
--   expected(min(v))       = E[min | non-empty] = 11.70 / 0.79 = 14.8101
DROP TABLE IF EXISTS seh_e CASCADE;
CREATE TABLE seh_e(v int);
INSERT INTO seh_e VALUES (10),(20),(30);
SELECT add_provenance('seh_e');
DO $$ BEGIN
  PERFORM set_prob(provenance(), 0.5) FROM seh_e WHERE v=10;
  PERFORM set_prob(provenance(), 0.4) FROM seh_e WHERE v=20;
  PERFORM set_prob(provenance(), 0.3) FROM seh_e WHERE v=30;
END $$;
CREATE TEMP TABLE seh_ex AS
  SELECT count(*) AS c, min(v) AS mn,
         round(probability_evaluate(provenance())::numeric, 4) AS existence
  FROM seh_e;
SELECT remove_provenance('seh_ex');
SELECT 'scalar count existence' AS shape, existence FROM seh_ex;
SELECT 'expected(min) conditional' AS shape, round(expected(mn)::numeric, 4) AS e
FROM seh_ex;
DROP TABLE seh_ex;
DROP TABLE seh_e CASCADE;

DROP TABLE seh_t CASCADE;
