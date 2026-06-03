\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Correlated scalar subqueries.  (SELECT Q.x FROM Q WHERE corr) used as a
-- top-level target-list entry, over a single tracked base relation R, is
-- decorrelated to  R LEFT JOIN Q ON corr, GROUP BY R.cols,
-- HAVING count(Q.key) <= 1, with the value picked by choose().  The corrected
-- outer-join lowering supplies the 0-match NULL row, so the result row always
-- exists (driven by R) with the matched value or NULL, and the count<=1 HAVING
-- gates out the (SQL-illegal) >=2-match worlds.

CREATE TABLE ssr(a int, k int);
CREATE TABLE ssq(k int, x int);
INSERT INTO ssr VALUES (10,1),(20,2),(30,3);

-- Part 1: at most one match per key.  k=1,2 match; k=3 has no match (NULL).
INSERT INTO ssq VALUES (1,100),(2,200);
SELECT add_provenance('ssr');
SELECT add_provenance('ssq');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM ssr;
  PERFORM set_prob(provsql, 0.5) FROM ssq;
END $$;

-- Value (choose) and row-existence probability.  Each row exists because its
-- driving ssr row exists (p=1); the value is the matched x or NULL.  sx is the
-- agg_token carrying the value.
CREATE TABLE ss1 AS
  SELECT ssr.a AS a,
         (SELECT ssq.x FROM ssq WHERE ssq.k = ssr.k) AS sx,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ssr;
SELECT remove_provenance('ss1');
SELECT a, sx, p FROM ss1 ORDER BY a;
DROP TABLE ss1;

DROP TABLE ssq;
DROP TABLE ssr;

-- Part 2: TWO matches for key 1 (mutually independent at 0.5).  In SQL the
-- scalar subquery would error "more than one row"; ProvSQL keeps the row but
-- the count(Q.k)<=1 HAVING restricts it to the <=1-match worlds, so the row's
-- probability is P(not both present) = 1 - 0.25 = 0.75.  (The chosen value is
-- order-dependent, so only the probability is pinned here.)
CREATE TABLE ssr(a int, k int);
CREATE TABLE ssq(k int, x int);
INSERT INTO ssr VALUES (10,1);
INSERT INTO ssq VALUES (1,100),(1,101);
SELECT add_provenance('ssr');
SELECT add_provenance('ssq');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM ssr;
  PERFORM set_prob(provsql, 0.5) FROM ssq;
END $$;

CREATE TABLE ss2 AS
  SELECT ssr.a AS a,
         (SELECT ssq.x FROM ssq WHERE ssq.k = ssr.k) AS sx,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ssr;
SELECT remove_provenance('ss2');
SELECT a, p FROM ss2 ORDER BY a;
DROP TABLE ss2;

DROP TABLE ssq;
DROP TABLE ssr;

-- Part 3: a scalar subquery in a WHERE comparison lifts to HAVING on choose().
-- WHERE (SELECT x FROM Q WHERE corr) > 50: the comparison is on the aggregated
-- value, so it becomes HAVING choose(Q.x) > 50 (AND count(Q.k) <= 1).
CREATE TABLE ssr(a int, k int);
CREATE TABLE ssq(k int, x int);
INSERT INTO ssr VALUES (10,1),(20,2);
INSERT INTO ssq VALUES (1,100),(2,40);
SELECT add_provenance('ssr');
SELECT add_provenance('ssq');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM ssr;
  PERFORM set_prob(provsql, 0.5) FROM ssq;
END $$;

-- a=10: value 100 > 50, passes iff ssq(1,100) present -> p = 0.5.
-- a=20: value 40 is never > 50 -> probability 0.
CREATE TABLE ss3 AS
  SELECT ssr.a AS a,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ssr
  WHERE (SELECT ssq.x FROM ssq WHERE ssq.k = ssr.k) > 50;
SELECT remove_provenance('ss3');
SELECT a, p FROM ss3 ORDER BY a;
DROP TABLE ss3;

DROP TABLE ssq;
DROP TABLE ssr;
