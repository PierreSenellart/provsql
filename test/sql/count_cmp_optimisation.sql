\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- ----------------------------------------------------------------------
-- Pin the Poisson-binomial pre-pass (provsql.cmp_probability_evaluation)
-- against the unoptimised path.  For every supported operator
-- (>=, >, <=, <, =, <>), and for a multi-cmp HAVING where the
-- pre-pass must decline to fire (shared gate_agg), the optimised
-- probability must match the unoptimised one to four decimal places.
-- ----------------------------------------------------------------------

-- (1) COUNT(*) >= 2
DROP TABLE IF EXISTS ccmp_off, ccmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE ccmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING count(*) >= 2;
SELECT remove_provenance('ccmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE ccmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING count(*) >= 2;
SELECT remove_provenance('ccmp_on');
SELECT 'count >= 2' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off,
       ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM ccmp_off o JOIN ccmp_on n USING (city) ORDER BY o.city;

-- (2) COUNT(*) > 1
DROP TABLE ccmp_off, ccmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE ccmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING count(*) > 1;
SELECT remove_provenance('ccmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE ccmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING count(*) > 1;
SELECT remove_provenance('ccmp_on');
SELECT 'count > 1' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off,
       ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM ccmp_off o JOIN ccmp_on n USING (city) ORDER BY o.city;

-- (3) COUNT(*) <= 2
DROP TABLE ccmp_off, ccmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE ccmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING count(*) <= 2;
SELECT remove_provenance('ccmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE ccmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING count(*) <= 2;
SELECT remove_provenance('ccmp_on');
SELECT 'count <= 2' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off,
       ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM ccmp_off o JOIN ccmp_on n USING (city) ORDER BY o.city;

-- (4) COUNT(*) < 3
DROP TABLE ccmp_off, ccmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE ccmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING count(*) < 3;
SELECT remove_provenance('ccmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE ccmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING count(*) < 3;
SELECT remove_provenance('ccmp_on');
SELECT 'count < 3' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off,
       ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM ccmp_off o JOIN ccmp_on n USING (city) ORDER BY o.city;

-- (5) COUNT(*) = 2
DROP TABLE ccmp_off, ccmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE ccmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING count(*) = 2;
SELECT remove_provenance('ccmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE ccmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING count(*) = 2;
SELECT remove_provenance('ccmp_on');
SELECT 'count = 2' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off,
       ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM ccmp_off o JOIN ccmp_on n USING (city) ORDER BY o.city;

-- (6) COUNT(*) <> 2
DROP TABLE ccmp_off, ccmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE ccmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING count(*) <> 2;
SELECT remove_provenance('ccmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE ccmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING count(*) <> 2;
SELECT remove_provenance('ccmp_on');
SELECT 'count <> 2' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off,
       ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM ccmp_off o JOIN ccmp_on n USING (city) ORDER BY o.city;

-- (7) Multi-cmp HAVING : the pre-pass must NOT fire because both
-- cmps share the same gate_agg, so the optimised result still
-- matches the off-path (the pre-pass declines and the original
-- pw_from_cmp_gate runs).
DROP TABLE ccmp_off, ccmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE ccmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city
  HAVING count(*) >= 1 AND count(*) <= 2;
SELECT remove_provenance('ccmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE ccmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city
  HAVING count(*) >= 1 AND count(*) <= 2;
SELECT remove_provenance('ccmp_on');
SELECT 'multi-cmp (must match off-path)' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off,
       ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM ccmp_off o JOIN ccmp_on n USING (city) ORDER BY o.city;

-- (8) Generalized contributors : each counted row is a PRODUCT of two
-- tracked tuples (a join), not a single leaf.  The leaf sets are
-- pairwise disjoint and private (one ccmp_s row per ccmp_r row), so the
-- pre-pass fires on the products and must match the off-path.
DROP TABLE ccmp_off, ccmp_on;
DROP TABLE IF EXISTS ccmp_r, ccmp_s CASCADE;
CREATE TABLE ccmp_r(g int, k int);
CREATE TABLE ccmp_s(k int);
INSERT INTO ccmp_r VALUES (1,1),(1,2),(1,3),(2,4),(2,5);
INSERT INTO ccmp_s VALUES (1),(2),(3),(4),(5);
SELECT add_provenance('ccmp_r');
SELECT add_provenance('ccmp_s');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ccmp_r;
            PERFORM set_prob(provenance(), 0.6) FROM ccmp_s; END $$;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE ccmp_off AS
  SELECT r.g, probability_evaluate(provenance()) AS p
  FROM ccmp_r r JOIN ccmp_s s USING (k) GROUP BY r.g HAVING count(*) >= 2;
SELECT remove_provenance('ccmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE ccmp_on AS
  SELECT r.g, probability_evaluate(provenance()) AS p
  FROM ccmp_r r JOIN ccmp_s s USING (k) GROUP BY r.g HAVING count(*) >= 2;
SELECT remove_provenance('ccmp_on');
SELECT 'product contributors' AS shape, o.g,
       ROUND(o.p::numeric, 4) AS p_off,
       ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM ccmp_off o JOIN ccmp_on n USING (g) ORDER BY o.g;

-- Confirm the pre-pass actually FIRES on the product shape (the NOTICE
-- at verbose_level >= 5 guards against silently losing the
-- optimisation for products).
SET provsql.verbose_level = 5;
CREATE TEMP TABLE ccmp_fires AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p_fires
  FROM (SELECT r.g, provenance() FROM ccmp_r r JOIN ccmp_s s USING (k)
        GROUP BY r.g HAVING count(*) >= 2) q(g, provenance) WHERE g = 1;
SET provsql.verbose_level = 0;
SELECT remove_provenance('ccmp_fires');
SELECT p_fires FROM ccmp_fires;
DROP TABLE ccmp_fires;

-- (9) Shared leaf : every counted row references the SAME ccmp_one
-- tuple, so the contributors are not independent.  The pre-pass must
-- DECLINE (no shortcut) and the on-path falls back to the exact
-- enumerator, still matching the off-path.
DROP TABLE ccmp_off, ccmp_on;
DROP TABLE IF EXISTS ccmp_one CASCADE;
CREATE TABLE ccmp_one(z int);
INSERT INTO ccmp_one VALUES (0);
SELECT add_provenance('ccmp_one');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.7) FROM ccmp_one; END $$;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE ccmp_off AS
  SELECT r.g, probability_evaluate(provenance()) AS p
  FROM ccmp_r r, ccmp_one GROUP BY r.g HAVING count(*) >= 2;
SELECT remove_provenance('ccmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE ccmp_on AS
  SELECT r.g, probability_evaluate(provenance()) AS p
  FROM ccmp_r r, ccmp_one GROUP BY r.g HAVING count(*) >= 2;
SELECT remove_provenance('ccmp_on');
SELECT 'shared leaf (must match off-path)' AS shape, o.g,
       ROUND(o.p::numeric, 4) AS p_off,
       ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM ccmp_off o JOIN ccmp_on n USING (g) ORDER BY o.g;

DROP TABLE ccmp_off, ccmp_on;
DROP TABLE ccmp_r, ccmp_s, ccmp_one CASCADE;
