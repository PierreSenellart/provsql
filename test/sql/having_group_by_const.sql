\set ECHO none
\pset format unaligned

-- ----------------------------------------------------------------------
-- Regression: GROUP BY a constant grouping key + a HAVING comparison
-- whose literal equals that constant.  On PostgreSQL 18 the planner
-- rewrites the matching HAVING literal into a Var referencing the
-- synthetic RTE_GROUP entry; strip_group_rte_pg18() must resolve that
-- grouped Var in q->havingQual (not only in the target list / WHERE) or
-- the HAVING converter bails with "cannot handle complex HAVING
-- expressions".  The control case (literal != grouping constant) never
-- triggered the substitution and is included to show it is unaffected.
--
-- Results are materialised and stripped of their provenance token
-- (per-run random) before display, so only the stable probability prints.
-- ----------------------------------------------------------------------

CREATE TABLE hgc(v int);
INSERT INTO hgc VALUES (1),(2),(3);
SELECT add_provenance('hgc');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM hgc; END $$;

-- GROUP BY the constant 1; HAVING count(*) = 1 -- the literal 1 collides
-- with the grouping constant and is the case that used to error.
CREATE TEMP TABLE hgc1 AS
  SELECT 1 AS g, probability_evaluate(provenance()) AS p
  FROM hgc GROUP BY 1 HAVING count(*) = 1;
SELECT remove_provenance('hgc1');
SELECT 'count=1' AS shape, g, round(p::numeric, 4) AS p FROM hgc1;

-- GROUP BY 1; HAVING count(*) >= 1 -- same collision on the threshold.
CREATE TEMP TABLE hgc2 AS
  SELECT 1 AS g, probability_evaluate(provenance()) AS p
  FROM hgc GROUP BY 1 HAVING count(*) >= 1;
SELECT remove_provenance('hgc2');
SELECT 'count>=1' AS shape, g, round(p::numeric, 4) AS p FROM hgc2;

-- Control: literal 2 differs from the grouping constant 1, so no
-- substitution occurred even before the fix.
CREATE TEMP TABLE hgc3 AS
  SELECT 1 AS g, probability_evaluate(provenance()) AS p
  FROM hgc GROUP BY 1 HAVING count(*) = 2;
SELECT remove_provenance('hgc3');
SELECT 'count=2' AS shape, g, round(p::numeric, 4) AS p FROM hgc3;

-- The collision is independent of the aggregate: SUM(v) = 1 with the
-- same constant grouping key.
CREATE TEMP TABLE hgc4 AS
  SELECT 1 AS g, probability_evaluate(provenance()) AS p
  FROM hgc GROUP BY 1 HAVING sum(v) = 1;
SELECT remove_provenance('hgc4');
SELECT 'sum=1' AS shape, g, round(p::numeric, 4) AS p FROM hgc4;

DROP TABLE hgc1, hgc2, hgc3, hgc4;
SELECT remove_provenance('hgc');
DROP TABLE hgc;
