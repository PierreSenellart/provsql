\set ECHO none
\pset format unaligned

-- ----------------------------------------------------------------------
-- HAVING COUNT(DISTINCT y) probability.  Regression for the bug where a
-- DISTINCT aggregate in a HAVING clause was evaluated as a plain COUNT
-- over the raw tuples (the agg-distinct GROUP-BY rewrite only handled the
-- target list, never q->havingQual), so duplicate same-value tuples were
-- counted more than once in the provenance circuit.
--
-- Setup (all tuples independent, p = 0.5):
--   g=1 : y in {10 (twice), 20, 30}   g=2 : y in {40, 50}
-- Per-distinct-value EXISTS probabilities:
--   g=1: P(10)=1-0.5^2=0.75, P(20)=0.5, P(30)=0.5;  g=2: P(40)=P(50)=0.5.
-- COUNT(DISTINCT y) is the number of distinct values present, so its
-- distribution is the (independent) sum of those per-value indicators.
-- ----------------------------------------------------------------------

DROP TABLE IF EXISTS hcd_t CASCADE;
CREATE TABLE hcd_t(g int, y int);
INSERT INTO hcd_t VALUES (1,10),(1,10),(1,20),(1,30),(2,40),(2,50);
SELECT add_provenance('hcd_t');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM hcd_t; END $$;

-- COUNT(DISTINCT y) >= 2
-- g=1: P(>=2 of {0.75,0.5,0.5}) = 0.625 ; g=2: P(both of {0.5,0.5}) = 0.25
CREATE TEMP TABLE r AS SELECT g, probability_evaluate(provenance()) AS p
  FROM hcd_t GROUP BY g HAVING count(DISTINCT y) >= 2;
SELECT remove_provenance('r');
SELECT 'count(distinct) >= 2' AS shape, g, ROUND(p::numeric,4) AS p FROM r ORDER BY g;
DROP TABLE r;

-- COUNT(DISTINCT y) >= 3
-- g=1: P(all 3) = 0.75*0.5*0.5 = 0.1875 ; g=2: impossible (only 2 distinct)
CREATE TEMP TABLE r AS SELECT g, probability_evaluate(provenance()) AS p
  FROM hcd_t GROUP BY g HAVING count(DISTINCT y) >= 3;
SELECT remove_provenance('r');
SELECT 'count(distinct) >= 3' AS shape, g, ROUND(p::numeric,4) AS p FROM r ORDER BY g;
DROP TABLE r;

-- COUNT(DISTINCT y) = 2
-- g=1: P(exactly 2) = 0.4375 ; g=2: P(2) = 0.25
CREATE TEMP TABLE r AS SELECT g, probability_evaluate(provenance()) AS p
  FROM hcd_t GROUP BY g HAVING count(DISTINCT y) = 2;
SELECT remove_provenance('r');
SELECT 'count(distinct) = 2' AS shape, g, ROUND(p::numeric,4) AS p FROM r ORDER BY g;
DROP TABLE r;

-- COUNT(DISTINCT y) <= 1  (empty group excluded: exactly one distinct present)
-- g=1: P(exactly 1) = 0.3125 ; g=2: P(1) = 0.5
CREATE TEMP TABLE r AS SELECT g, probability_evaluate(provenance()) AS p
  FROM hcd_t GROUP BY g HAVING count(DISTINCT y) <= 1;
SELECT remove_provenance('r');
SELECT 'count(distinct) <= 1' AS shape, g, ROUND(p::numeric,4) AS p FROM r ORDER BY g;
DROP TABLE r;

-- DISTINCT aggregate in both SELECT list and HAVING: value and probability
-- must both be correct (the value path was always correct; this pins that
-- the HAVING fix does not disturb it).
CREATE TEMP TABLE r AS
  SELECT g, count(DISTINCT y) AS cd, probability_evaluate(provenance()) AS p
  FROM hcd_t GROUP BY g HAVING count(DISTINCT y) >= 2;
SELECT remove_provenance('r');
SELECT 'select+having' AS shape, g, cd, ROUND(p::numeric,4) AS p FROM r ORDER BY g;
DROP TABLE r;

-- The off-path (exact possible-worlds enumeration) must agree with the
-- default path: the fix is in the circuit the rewrite builds, not in the
-- closed-form cmp evaluator, so both see the corrected contributors.
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE off AS SELECT g, probability_evaluate(provenance()) AS p
  FROM hcd_t GROUP BY g HAVING count(DISTINCT y) >= 2;
SELECT remove_provenance('off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE oncf AS SELECT g, probability_evaluate(provenance()) AS p
  FROM hcd_t GROUP BY g HAVING count(DISTINCT y) >= 2;
SELECT remove_provenance('oncf');
SELECT 'off vs on' AS shape, o.g,
       ROUND(o.p::numeric,4) AS p_off, ROUND(n.p::numeric,4) AS p_on,
       ROUND(ABS(o.p-n.p)::numeric,6) AS diff
FROM off o JOIN oncf n USING (g) ORDER BY o.g;
DROP TABLE off, oncf;

DROP TABLE hcd_t CASCADE;

-- Regression: COUNT(DISTINCT) over an OUTER join.  The agg-distinct GROUP-BY
-- rewrite appends its deduped subqueries to the range table but indexed them by
-- from-list position -- wrong when the FROM is a JoinExpr (an outer join is one
-- from-list item spanning several rtable slots), so a base arm was referenced
-- instead ("rel N already exists" / "no relation entry for relid N").
CREATE TABLE hcd_r(g int);
CREATE TABLE hcd_q(g int, y int);
INSERT INTO hcd_r VALUES (1),(2),(3);
INSERT INTO hcd_q VALUES (1,10),(2,20),(2,20),(3,30),(3,31);  -- g=2 same value; g=3 distinct
SELECT add_provenance('hcd_r');
SELECT add_provenance('hcd_q');
DO $$ BEGIN
  PERFORM set_prob(provenance(), 1.0) FROM hcd_r;
  PERFORM set_prob(provenance(), 0.5) FROM hcd_q;
END $$;

-- count(DISTINCT y) per g over the LEFT JOIN: g=1 -> 1, g=2 -> 1 (same value), g=3 -> 2.
CREATE TEMP TABLE ljc AS
  SELECT hcd_r.g AS g, count(DISTINCT hcd_q.y) AS c
  FROM hcd_r LEFT JOIN hcd_q ON hcd_q.g = hcd_r.g GROUP BY hcd_r.g;
SELECT remove_provenance('ljc');
SELECT 'lj count' AS shape, g, c FROM ljc ORDER BY g;
DROP TABLE ljc;

-- HAVING count(DISTINCT y) <= 1 over the LEFT JOIN: g=2 (same value) is admitted
-- (p=1); g=3 (two distinct) is gated when both present -> p = 1 - 0.25 = 0.75.
CREATE TEMP TABLE lj AS
  SELECT hcd_r.g AS g, probability_evaluate(provenance()) AS p
  FROM hcd_r LEFT JOIN hcd_q ON hcd_q.g = hcd_r.g
  GROUP BY hcd_r.g HAVING count(DISTINCT hcd_q.y) <= 1;
SELECT remove_provenance('lj');
SELECT 'lj having' AS shape, g, ROUND(p::numeric,4) AS p FROM lj ORDER BY g;
DROP TABLE lj;
DROP TABLE hcd_q; DROP TABLE hcd_r;
