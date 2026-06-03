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

-- Part 4: scalar subquery over a (multi-table) subquery FROM.  The subquery in
-- FROM is the R arm of the decorrelated LEFT JOIN, lowered via the subquery-arm
-- path; the scalar subquery over ssq decorrelates onto it.
CREATE TABLE ss_r(a int, k int);
CREATE TABLE ss_s(k int, w int);
CREATE TABLE ss_q(k int, x int);
INSERT INTO ss_r VALUES (10,1),(20,2);
INSERT INTO ss_s VALUES (1,100),(2,200);
INSERT INTO ss_q VALUES (1,7),(2,8);
SELECT add_provenance('ss_r');
SELECT add_provenance('ss_s');
SELECT add_provenance('ss_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM ss_r;
  PERFORM set_prob(provsql, 1.0) FROM ss_s;
  PERFORM set_prob(provsql, 0.5) FROM ss_q;
END $$;

-- a=10 (k=1) -> x=7 ; a=20 (k=2) -> x=8 ; both rows always exist (ss_r, ss_s p=1).
CREATE TABLE ss4 AS
  SELECT r.a AS a,
         (SELECT ss_q.x FROM ss_q WHERE ss_q.k = r.k) AS sx,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT ss_r.a, ss_r.k, ss_s.w FROM ss_r, ss_s WHERE ss_s.k = ss_r.k) r;
SELECT remove_provenance('ss4');
SELECT a, sx, p FROM ss4 ORDER BY a;
DROP TABLE ss4;

DROP TABLE ss_q;
DROP TABLE ss_s;
DROP TABLE ss_r;

-- Part 5: scalar subquery over an un-wrapped multi-table FROM.  The outer FROM
-- (ss_r5, ss_s5 joined in WHERE) is auto-wrapped into a derived subquery on
-- which the scalar subquery over ss_q5 decorrelates.
CREATE TABLE ss_r5(a int, k int);
CREATE TABLE ss_s5(k int, w int);
CREATE TABLE ss_q5(k int, x int);
INSERT INTO ss_r5 VALUES (10,1),(20,2);
INSERT INTO ss_s5 VALUES (1,100),(2,200);
INSERT INTO ss_q5 VALUES (1,7),(2,8);
SELECT add_provenance('ss_r5');
SELECT add_provenance('ss_s5');
SELECT add_provenance('ss_q5');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM ss_r5;
  PERFORM set_prob(provsql, 1.0) FROM ss_s5;
  PERFORM set_prob(provsql, 0.5) FROM ss_q5;
END $$;

-- target-list subquery: a=10 -> 7, a=20 -> 8 ; rows always exist (p=1).
CREATE TABLE ss5 AS
  SELECT ss_r5.a AS a,
         (SELECT ss_q5.x FROM ss_q5 WHERE ss_q5.k = ss_r5.k) AS sx,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ss_r5, ss_s5 WHERE ss_s5.k = ss_r5.k;
SELECT remove_provenance('ss5');
SELECT a, sx, p FROM ss5 ORDER BY a;
DROP TABLE ss5;

-- WHERE subquery: the value 7/8 is > 5 whenever present, so each row passes
-- with probability P(its ss_q5 row present) = 0.5.
CREATE TABLE ss6 AS
  SELECT ss_r5.a AS a,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ss_r5, ss_s5
  WHERE ss_s5.k = ss_r5.k AND (SELECT ss_q5.x FROM ss_q5 WHERE ss_q5.k = ss_r5.k) > 5;
SELECT remove_provenance('ss6');
SELECT a, p FROM ss6 ORDER BY a;
DROP TABLE ss6;

DROP TABLE ss_q5;
DROP TABLE ss_s5;
DROP TABLE ss_r5;
