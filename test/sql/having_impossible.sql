\set ECHO none
\pset format unaligned

-- Groups whose HAVING predicate holds in no world are dropped by the rewriter,
-- before any gate is created for them: PostgreSQL computes, as ordinary
-- aggregates, the range the aggregate takes over the selections of the
-- group's rows, and keeps the group only if the predicate can hold for some
-- value in it.  A row whose provenance is zero is the same as an absent row;
-- this only gets rid of some of them early.  The check is a sufficient one:
-- whatever it keeps is still evaluated exactly.
--
--   g=1: 2, 4      g=2: -3, 5      g=3: 7      g=4: NULL    (each row p = 0.5)

CREATE TABLE hi(g int, x int, s text);
INSERT INTO hi VALUES (1,2,'b'), (1,4,'d'), (2,-3,'a'), (2,5,'e'), (3,7,'g'), (4,NULL,NULL);
SELECT add_provenance('hi');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM hi; END $$;

CREATE FUNCTION hi_run(pred text) RETURNS TABLE(g int, p numeric) LANGUAGE plpgsql AS $$
BEGIN
  EXECUTE format(
    'CREATE TEMP TABLE hi_tmp AS
       SELECT g, round(provsql.probability_evaluate(provsql.provenance())::numeric, 4) AS p
       FROM hi GROUP BY g HAVING %s', pred);
  SET LOCAL client_min_messages = warning;
  PERFORM provsql.remove_provenance('hi_tmp');
  RETURN QUERY SELECT t.g, t.p FROM hi_tmp t;
  DROP TABLE hi_tmp;
END $$;

-- ---------------------------------------------------------------------------
-- 1. Upper and lower ends of each aggregate
-- ---------------------------------------------------------------------------

-- count: at most the number of rows.  >= 2 keeps g=1, 2 (0.25 each).
SELECT 'count(*) >= 2' AS q, * FROM hi_run('count(*) >= 2') ORDER BY g;
-- > 2 holds nowhere.
SELECT 'count(*) > 2' AS q, * FROM hi_run('count(*) > 2') ORDER BY g;
-- count(x) ignores the NULL of g=4.
SELECT 'count(x) >= 1' AS q, * FROM hi_run('count(x) >= 1') ORDER BY g;
-- = 0 is never dropped on these grounds (the lower end 0 is always possible
-- for count(x), and count(*) = 0 evaluates to zero, not dropped).
SELECT 'count(x) = 0' AS q, * FROM hi_run('count(x) = 0') ORDER BY g;

-- sum: at most the sum of the positive values (6, 5, 7), at least that of the
-- negative ones (0, -3, 0).
SELECT 'sum(x) >= 6' AS q, * FROM hi_run('sum(x) >= 6') ORDER BY g;
SELECT 'sum(x) > 6' AS q, * FROM hi_run('sum(x) > 6') ORDER BY g;
SELECT 'sum(x) < 0' AS q, * FROM hi_run('sum(x) < 0') ORDER BY g;
SELECT 'sum(x) <= -4' AS q, * FROM hi_run('sum(x) <= -4') ORDER BY g;
-- A constant inside the range is kept even when unreachable: sum = 5 over
-- {2, 4} is evaluated, to zero; over {-3, 5} it holds when only 5 is present.
SELECT 'sum(x) = 5' AS q, * FROM hi_run('sum(x) = 5') ORDER BY g;
SELECT 'sum(x) = 100' AS q, * FROM hi_run('sum(x) = 100') ORDER BY g;
-- All values non-positive: the upper end is at most 0.
SELECT 'sum(-x) > 0' AS q, * FROM hi_run('sum(-abs(x)) > 0') ORDER BY g;

-- max and min: both range over [min(x), max(x)].
SELECT 'max(x) >= 5' AS q, * FROM hi_run('max(x) >= 5') ORDER BY g;
SELECT 'max(x) < 0' AS q, * FROM hi_run('max(x) < 0') ORDER BY g;
SELECT 'min(x) > 4' AS q, * FROM hi_run('min(x) > 4') ORDER BY g;
SELECT 'min(x) <= -3' AS q, * FROM hi_run('min(x) <= -3') ORDER BY g;
SELECT 'avg(x) > 4' AS q, * FROM hi_run('avg(x) > 4') ORDER BY g;
SELECT 'avg(x) = 100' AS q, * FROM hi_run('avg(x) = 100') ORDER BY g;
-- Text: collation-aware comparison of the ends.  (The evaluators do not
-- compare a max with a text constant, so only the groups kept are listed.)
CREATE TABLE hi_txt AS SELECT g FROM hi GROUP BY g HAVING max(s) >= 'e';
SELECT remove_provenance('hi_txt');
SELECT 'max(s) >= e' AS q, g FROM hi_txt ORDER BY g;

-- The aggregate on the right, arithmetic folded into the threshold, FILTER.
SELECT '6 <= sum(x)' AS q, * FROM hi_run('6 <= sum(x)') ORDER BY g;
SELECT 'sum(x) + 1 > 7' AS q, * FROM hi_run('sum(x) + 1 > 7') ORDER BY g;
SELECT 'sum(x) f >= 5' AS q, * FROM hi_run('sum(x) FILTER (WHERE x > 3) >= 5') ORDER BY g;
-- Against a grouping key.
SELECT 'sum(x) >= 3 * g' AS q, * FROM hi_run('sum(x) >= 3 * g') ORDER BY g;

-- ---------------------------------------------------------------------------
-- 2. Boolean structure
-- ---------------------------------------------------------------------------

-- AND: one impossible conjunct suffices.
SELECT 'AND' AS q, * FROM hi_run('count(*) >= 1 AND sum(x) > 6') ORDER BY g;
-- OR: dropped only if every disjunct is impossible (g=1: neither; g=4: both).
SELECT 'OR' AS q, * FROM hi_run('sum(x) > 6 OR max(x) < 0') ORDER BY g;
-- OR with a part nothing is known about (<>): nothing dropped, zero evaluated.
SELECT 'OR unknown' AS q, * FROM hi_run('sum(x) > 100 OR count(*) <> 1') ORDER BY g;
-- NOT is pushed to the atoms: NOT (sum <= 6) is sum > 6.
SELECT 'NOT' AS q, * FROM hi_run('NOT (sum(x) <= 6)') ORDER BY g;
-- A regular atom decides by itself.
SELECT 'regular AND' AS q, * FROM hi_run('count(*) >= 1 AND g > 2') ORDER BY g;
SELECT 'regular OR' AS q, * FROM hi_run('sum(x) > 100 OR g = 3') ORDER BY g;
-- Aggregates on both sides: nothing known.
SELECT 'agg vs agg' AS q, * FROM hi_run('sum(x) > 100 * count(*)') ORDER BY g;

-- ---------------------------------------------------------------------------
-- 3. Scalar aggregation, and agreement with the evaluator
-- ---------------------------------------------------------------------------

CREATE TABLE hi_s1 AS SELECT round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hi HAVING count(*) > 6;
SELECT remove_provenance('hi_s1'); SELECT 'scalar impossible' AS q, count(*) AS n FROM hi_s1;
CREATE TABLE hi_s2 AS SELECT round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hi HAVING count(*) >= 6;
SELECT remove_provenance('hi_s2'); SELECT 'scalar possible' AS q, p FROM hi_s2;

-- The same predicates as a selection on the aggregate column of a subquery
-- reach the evaluator, which finds them zero: a zero row and an absent row
-- are the same.
CREATE TABLE hi_e AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p,
    sr_formula(provenance()) = '𝟘' AS is_zero
  FROM (SELECT g, sum(x) AS v FROM hi GROUP BY g) t WHERE v > 6;
SELECT remove_provenance('hi_e');
SELECT 'evaluator, sum > 6' AS q, g, p, is_zero FROM hi_e ORDER BY g;

DROP FUNCTION hi_run(text);
DROP TABLE hi, hi_s1, hi_s2, hi_e, hi_txt;
