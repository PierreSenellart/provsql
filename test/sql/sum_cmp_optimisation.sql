\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- ----------------------------------------------------------------------
-- Pin the weighted-sum DP pre-pass
-- (provsql.cmp_probability_evaluation) against the unoptimised
-- possible-worlds path.  For SUM(id) and every supported operator
-- (>=, >, <=, <, =, <>), the optimised probability must match the
-- unoptimised one to four decimal places.  personnel carries
-- prob = id/10 (probability_setup), id values per city group:
--   New York {1,2} (sum 3), Paris {3,5,6} (sum 14), Berlin {4,7} (sum 11).
-- The two decline cases (shared gate_agg, shared leaf) must fall back
-- to the exact enumerator and still match the off-path.
-- ----------------------------------------------------------------------

-- (1) SUM(id) >= 8
DROP TABLE IF EXISTS scmp_off, scmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE scmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING sum(id) >= 8;
SELECT remove_provenance('scmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE scmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING sum(id) >= 8;
SELECT remove_provenance('scmp_on');
SELECT 'sum >= 8' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM scmp_off o JOIN scmp_on n USING (city) ORDER BY o.city;

-- (2) SUM(id) > 8
DROP TABLE scmp_off, scmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE scmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING sum(id) > 8;
SELECT remove_provenance('scmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE scmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING sum(id) > 8;
SELECT remove_provenance('scmp_on');
SELECT 'sum > 8' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM scmp_off o JOIN scmp_on n USING (city) ORDER BY o.city;

-- (3) SUM(id) <= 8
DROP TABLE scmp_off, scmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE scmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING sum(id) <= 8;
SELECT remove_provenance('scmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE scmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING sum(id) <= 8;
SELECT remove_provenance('scmp_on');
SELECT 'sum <= 8' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM scmp_off o JOIN scmp_on n USING (city) ORDER BY o.city;

-- (4) SUM(id) < 8
DROP TABLE scmp_off, scmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE scmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING sum(id) < 8;
SELECT remove_provenance('scmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE scmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING sum(id) < 8;
SELECT remove_provenance('scmp_on');
SELECT 'sum < 8' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM scmp_off o JOIN scmp_on n USING (city) ORDER BY o.city;

-- (5) SUM(id) = 11
DROP TABLE scmp_off, scmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE scmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING sum(id) = 11;
SELECT remove_provenance('scmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE scmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING sum(id) = 11;
SELECT remove_provenance('scmp_on');
SELECT 'sum = 11' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM scmp_off o JOIN scmp_on n USING (city) ORDER BY o.city;

-- (6) SUM(id) <> 11
DROP TABLE scmp_off, scmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE scmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING sum(id) <> 11;
SELECT remove_provenance('scmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE scmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city HAVING sum(id) <> 11;
SELECT remove_provenance('scmp_on');
SELECT 'sum <> 11' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM scmp_off o JOIN scmp_on n USING (city) ORDER BY o.city;

-- (7) Multi-cmp HAVING : the pre-pass must NOT fire because both cmps
-- share the same gate_agg, so the optimised result still matches the
-- off-path (the pre-pass declines and pw_from_cmp_gate runs).
DROP TABLE scmp_off, scmp_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE scmp_off AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city
  HAVING sum(id) >= 4 AND sum(id) <= 12;
SELECT remove_provenance('scmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE scmp_on AS
  SELECT city, probability_evaluate(provenance()) AS p
  FROM personnel GROUP BY city
  HAVING sum(id) >= 4 AND sum(id) <= 12;
SELECT remove_provenance('scmp_on');
SELECT 'multi-cmp (must match off-path)' AS shape, o.city,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM scmp_off o JOIN scmp_on n USING (city) ORDER BY o.city;

-- (8) Generalized contributors : each summed row is a PRODUCT of two
-- tracked tuples (a join), the leaf sets pairwise disjoint and private,
-- so the pre-pass fires on the products and must match the off-path.
-- scmp_r.w carries the summed weight.
DROP TABLE scmp_off, scmp_on;
DROP TABLE IF EXISTS scmp_r, scmp_s CASCADE;
CREATE TABLE scmp_r(g int, k int, w int);
CREATE TABLE scmp_s(k int);
INSERT INTO scmp_r VALUES (1,1,2),(1,2,5),(1,3,7),(2,4,1),(2,5,3);
INSERT INTO scmp_s VALUES (1),(2),(3),(4),(5);
SELECT add_provenance('scmp_r');
SELECT add_provenance('scmp_s');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM scmp_r;
            PERFORM set_prob(provenance(), 0.6) FROM scmp_s; END $$;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE scmp_off AS
  SELECT r.g, probability_evaluate(provenance()) AS p
  FROM scmp_r r JOIN scmp_s s USING (k) GROUP BY r.g HAVING sum(r.w) >= 10;
SELECT remove_provenance('scmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE scmp_on AS
  SELECT r.g, probability_evaluate(provenance()) AS p
  FROM scmp_r r JOIN scmp_s s USING (k) GROUP BY r.g HAVING sum(r.w) >= 10;
SELECT remove_provenance('scmp_on');
SELECT 'product contributors' AS shape, o.g,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM scmp_off o JOIN scmp_on n USING (g) ORDER BY o.g;

-- Confirm the pre-pass actually FIRES on the product SUM shape.
SET provsql.verbose_level = 5;
CREATE TEMP TABLE scmp_fires AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p_fires
  FROM (SELECT r.g, provenance() FROM scmp_r r JOIN scmp_s s USING (k)
        GROUP BY r.g HAVING sum(r.w) >= 10) q(g, provenance) WHERE g = 1;
SET provsql.verbose_level = 0;
SELECT remove_provenance('scmp_fires');
SELECT p_fires FROM scmp_fires;
DROP TABLE scmp_fires;

-- (9) Shared leaf : every summed row references the SAME scmp_one tuple,
-- so the contributors are not independent.  The pre-pass must DECLINE
-- and the on-path falls back to the exact enumerator, still matching the
-- off-path.
DROP TABLE scmp_off, scmp_on;
DROP TABLE IF EXISTS scmp_one CASCADE;
CREATE TABLE scmp_one(z int);
INSERT INTO scmp_one VALUES (0);
SELECT add_provenance('scmp_one');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.7) FROM scmp_one; END $$;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE scmp_off AS
  SELECT r.g, probability_evaluate(provenance()) AS p
  FROM scmp_r r, scmp_one GROUP BY r.g HAVING sum(r.w) >= 10;
SELECT remove_provenance('scmp_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE scmp_on AS
  SELECT r.g, probability_evaluate(provenance()) AS p
  FROM scmp_r r, scmp_one GROUP BY r.g HAVING sum(r.w) >= 10;
SELECT remove_provenance('scmp_on');
SELECT 'shared leaf (must match off-path)' AS shape, o.g,
       ROUND(o.p::numeric, 4) AS p_off, ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM scmp_off o JOIN scmp_on n USING (g) ORDER BY o.g;

DROP TABLE scmp_off, scmp_on;
DROP TABLE scmp_r, scmp_s, scmp_one CASCADE;
