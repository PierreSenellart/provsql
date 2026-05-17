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

DROP TABLE ccmp_off, ccmp_on;
