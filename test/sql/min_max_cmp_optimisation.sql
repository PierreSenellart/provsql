\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- ----------------------------------------------------------------------
-- Pin the MIN / MAX closed-form pre-pass
-- (provsql.cmp_probability_evaluation) against the unoptimised
-- possible-worlds path.  For MIN(id) and MAX(id) and every supported
-- operator (>=, >, <=, <, =, <>), the optimised probability must match
-- the unoptimised one to four decimal places.  personnel carries
-- prob = id/10 (probability_setup), id values per city group:
--   New York {1,2}, Paris {3,5,6}, Berlin {4,7}.
-- The two decline cases (shared gate_agg, shared leaf) must fall back
-- to the exact enumerator and still match the off-path.
-- ----------------------------------------------------------------------

-- Helper: run "<agg>(id) <op> <c>" with the pre-pass off then on and
-- print the per-city probabilities side by side.  Implemented inline
-- per shape (psql has no parameterised blocks for SET); the comparison
-- query is identical throughout.

-- (1) MAX(id) >= 4
DROP TABLE IF EXISTS mcmp_off, mcmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING max(id) >= 4;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING max(id) >= 4;
SELECT remove_provenance('mcmp_on');
SELECT 'max >= 4' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (city) ORDER BY o.city;

-- (2) MAX(id) > 4
DROP TABLE mcmp_off, mcmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING max(id) > 4;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING max(id) > 4;
SELECT remove_provenance('mcmp_on');
SELECT 'max > 4' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (city) ORDER BY o.city;

-- (3) MAX(id) <= 4
DROP TABLE mcmp_off, mcmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING max(id) <= 4;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING max(id) <= 4;
SELECT remove_provenance('mcmp_on');
SELECT 'max <= 4' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (city) ORDER BY o.city;

-- (4) MAX(id) < 4
DROP TABLE mcmp_off, mcmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING max(id) < 4;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING max(id) < 4;
SELECT remove_provenance('mcmp_on');
SELECT 'max < 4' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (city) ORDER BY o.city;

-- (5) MAX(id) = 4
DROP TABLE mcmp_off, mcmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING max(id) = 4;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING max(id) = 4;
SELECT remove_provenance('mcmp_on');
SELECT 'max = 4' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (city) ORDER BY o.city;

-- (6) MAX(id) <> 4
DROP TABLE mcmp_off, mcmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING max(id) <> 4;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING max(id) <> 4;
SELECT remove_provenance('mcmp_on');
SELECT 'max <> 4' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (city) ORDER BY o.city;

-- (7) MIN(id) <= 4
DROP TABLE mcmp_off, mcmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING min(id) <= 4;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING min(id) <= 4;
SELECT remove_provenance('mcmp_on');
SELECT 'min <= 4' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (city) ORDER BY o.city;

-- (8) MIN(id) < 4
DROP TABLE mcmp_off, mcmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING min(id) < 4;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING min(id) < 4;
SELECT remove_provenance('mcmp_on');
SELECT 'min < 4' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (city) ORDER BY o.city;

-- (9) MIN(id) >= 4
DROP TABLE mcmp_off, mcmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING min(id) >= 4;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING min(id) >= 4;
SELECT remove_provenance('mcmp_on');
SELECT 'min >= 4' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (city) ORDER BY o.city;

-- (10) MIN(id) > 4
DROP TABLE mcmp_off, mcmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING min(id) > 4;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING min(id) > 4;
SELECT remove_provenance('mcmp_on');
SELECT 'min > 4' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (city) ORDER BY o.city;

-- (11) MIN(id) = 4
DROP TABLE mcmp_off, mcmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING min(id) = 4;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING min(id) = 4;
SELECT remove_provenance('mcmp_on');
SELECT 'min = 4' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (city) ORDER BY o.city;

-- (12) MIN(id) <> 4
DROP TABLE mcmp_off, mcmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING min(id) <> 4;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING min(id) <> 4;
SELECT remove_provenance('mcmp_on');
SELECT 'min <> 4' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (city) ORDER BY o.city;

-- (13) Multi-cmp HAVING : the pre-pass must NOT fire because both cmps
-- share the same gate_agg, so the optimised result still matches the
-- off-path (the pre-pass declines and pw_from_cmp_gate runs).
DROP TABLE mcmp_off, mcmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city
  HAVING max(id) >= 2 AND max(id) <= 6;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city
  HAVING max(id) >= 2 AND max(id) <= 6;
SELECT remove_provenance('mcmp_on');
SELECT 'multi-cmp (must match off-path)' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (city) ORDER BY o.city;

-- (14) Generalized contributors : each aggregated row is a PRODUCT of
-- two tracked tuples (a join), the leaf sets pairwise disjoint and
-- private, so the pre-pass fires on the products and must match the
-- off-path.  mcmp_r.v carries the aggregated value.
DROP TABLE mcmp_off, mcmp_on;
DROP TABLE IF EXISTS mcmp_r, mcmp_s CASCADE;
CREATE TABLE mcmp_r(g int, k int, v int);
CREATE TABLE mcmp_s(k int);
INSERT INTO mcmp_r VALUES (1,1,2),(1,2,5),(1,3,7),(2,4,1),(2,5,3);
INSERT INTO mcmp_s VALUES (1),(2),(3),(4),(5);
SELECT add_provenance('mcmp_r');
SELECT add_provenance('mcmp_s');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM mcmp_r;
            PERFORM set_prob(provenance(), 0.6) FROM mcmp_s; END $$;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT r.g, probability_evaluate(provenance()) AS p
  FROM mcmp_r r JOIN mcmp_s s USING (k) GROUP BY r.g HAVING max(r.v) >= 5;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT r.g, probability_evaluate(provenance()) AS p
  FROM mcmp_r r JOIN mcmp_s s USING (k) GROUP BY r.g HAVING max(r.v) >= 5;
SELECT remove_provenance('mcmp_on');
SELECT 'product contributors' AS shape, o.g,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (g) ORDER BY o.g;

-- Confirm the pre-pass actually FIRES on the product MAX shape.
SET provsql.verbose_level = 5;
CREATE TEMP TABLE mcmp_fires AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p_fires
  FROM (SELECT r.g, provenance() FROM mcmp_r r JOIN mcmp_s s USING (k)
        GROUP BY r.g HAVING max(r.v) >= 5) q(g, provenance) WHERE g = 1;
SET provsql.verbose_level = 0;
SELECT remove_provenance('mcmp_fires');
SELECT p_fires FROM mcmp_fires;
DROP TABLE mcmp_fires;

-- (15) Shared leaf : every aggregated row references the SAME mcmp_one
-- tuple, so the contributors are not independent.  The pre-pass must
-- DECLINE and the on-path falls back to the exact enumerator, still
-- matching the off-path.
DROP TABLE mcmp_off, mcmp_on;
DROP TABLE IF EXISTS mcmp_one CASCADE;
CREATE TABLE mcmp_one(z int);
INSERT INTO mcmp_one VALUES (0);
SELECT add_provenance('mcmp_one');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.7) FROM mcmp_one; END $$;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE mcmp_off AS
  SELECT r.g, probability_evaluate(provenance()) AS p
  FROM mcmp_r r, mcmp_one GROUP BY r.g HAVING max(r.v) >= 5;
SELECT remove_provenance('mcmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE mcmp_on AS
  SELECT r.g, probability_evaluate(provenance()) AS p
  FROM mcmp_r r, mcmp_one GROUP BY r.g HAVING max(r.v) >= 5;
SELECT remove_provenance('mcmp_on');
SELECT 'shared leaf (must match off-path)' AS shape, o.g,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM mcmp_off o JOIN mcmp_on n USING (g) ORDER BY o.g;

DROP TABLE mcmp_off, mcmp_on;
DROP TABLE mcmp_r, mcmp_s, mcmp_one CASCADE;
