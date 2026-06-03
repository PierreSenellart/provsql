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
-- value, so it becomes HAVING choose(Q.x) > 50 AND count(Q.k) = 1 (a comparison
-- needs the subquery to return exactly one row; cf. Part 10's empty group).
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

-- Part 6: a LIMIT/OFFSET body would pick a bounded, order-dependent subset, so
-- it is rejected cleanly (never silently miscomputed or crashing).
CREATE TABLE rj_r(a int, k int);
CREATE TABLE rj_q(k int, x int);
INSERT INTO rj_r VALUES (1,1);
INSERT INTO rj_q VALUES (1,10),(1,11);
SELECT add_provenance('rj_r');
SELECT add_provenance('rj_q');

SELECT rj_r.a, (SELECT rj_q.x FROM rj_q WHERE rj_q.k = rj_r.k LIMIT 1) AS sx,
       provenance() AS p FROM rj_r;

DROP TABLE rj_q;
DROP TABLE rj_r;

-- Part 7: untracked outer FROM with a scalar subquery over a tracked relation
-- (like joining an untracked table -- no warning; the outer just contributes
-- the identity).  The outer rows are certain, so existence is driven by the
-- subquery: k=1 -> 1 (one match); k=2 -> 0.75 (two independent matches gated
-- by count(...) <= 1).
CREATE TABLE su_u(a int, k int);
CREATE TABLE su_q(k int, x int);
INSERT INTO su_u VALUES (10,1),(20,2);
INSERT INTO su_q VALUES (1,100),(2,200),(2,201);
SELECT add_provenance('su_q');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM su_q; END $$;

CREATE TABLE su_res AS
  SELECT su_u.a AS a,
         (SELECT su_q.x FROM su_q WHERE su_q.k = su_u.k) AS sx,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM su_u;
SELECT remove_provenance('su_res');
SELECT a, p FROM su_res ORDER BY a;
DROP TABLE su_res;

DROP TABLE su_q;
DROP TABLE su_u;

-- Part 8: aggregate-body scalar subqueries decorrelate to the aggregate over
-- the LEFT-JOIN group (no choose / count<=1 gate).  count(*) is rewritten to
-- count(Q.key) so an empty correlated group gives 0, not 1; max/sum give NULL
-- on the empty group.
CREATE TABLE ag_r(a int, k int);
CREATE TABLE ag_q(k int, x int);
INSERT INTO ag_r VALUES (10,1),(20,2),(30,3);
INSERT INTO ag_q VALUES (1,100),(2,200),(2,201);
SELECT add_provenance('ag_r');
SELECT add_provenance('ag_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM ag_r;
  PERFORM set_prob(provsql, 0.5) FROM ag_q;
END $$;

-- count(*) value: k=1 -> 1, k=2 -> 2, k=3 -> 0 (empty group, not 1).
CREATE TABLE ag_v AS
  SELECT ag_r.a AS a, (SELECT count(*) FROM ag_q WHERE ag_q.k = ag_r.k) AS c
  FROM ag_r;
SELECT remove_provenance('ag_v');
SELECT a, c FROM ag_v ORDER BY a;
DROP TABLE ag_v;

-- max(x) value: k=1 -> 100, k=2 -> 201, k=3 -> NULL (empty group).
CREATE TABLE ag_v AS
  SELECT ag_r.a AS a, (SELECT max(ag_q.x) FROM ag_q WHERE ag_q.k = ag_r.k) AS m
  FROM ag_r;
SELECT remove_provenance('ag_v');
SELECT a, m FROM ag_v ORDER BY a;
DROP TABLE ag_v;

-- WHERE count(*) >= 2: only k=2 can reach 2; P(both matches present) = 0.25.
CREATE TABLE ag_p AS
  SELECT ag_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ag_r WHERE (SELECT count(*) FROM ag_q WHERE ag_q.k = ag_r.k) >= 2;
SELECT remove_provenance('ag_p');
SELECT a, p FROM ag_p ORDER BY a;
DROP TABLE ag_p;

DROP TABLE ag_q;
DROP TABLE ag_r;

-- Part 9: EXISTS / IN (semijoin) and NOT EXISTS / NOT IN (antijoin) over a
-- single tracked relation decorrelate via the same "(SELECT count(*) ...) >= 1"
-- / "= 0" lowering: count(*) -> count(Q.key) over the "R LEFT JOIN Q" group,
-- comparison lifted to HAVING.  Semijoin keeps R⊗⊕Q; antijoin keeps R⊗(1⊖⊕Q),
-- so EXISTS and NOT EXISTS are exact complements (their probabilities sum to 1).
CREATE TABLE se_r(a int, k int);
CREATE TABLE se_q(k int, x int);
INSERT INTO se_r VALUES (10,1),(20,2),(30,3);
INSERT INTO se_q VALUES (1,100),(2,200),(2,201);  -- k=1 one match, k=2 two, k=3 none
SELECT add_provenance('se_r');
SELECT add_provenance('se_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM se_r;
  PERFORM set_prob(provsql, 0.5) FROM se_q;
END $$;

-- EXISTS: k=1 -> 0.5, k=2 -> 0.75 (>=1 of two i.i.d. 0.5), k=3 -> 0.
CREATE TABLE se1 AS
  SELECT se_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM se_r WHERE EXISTS (SELECT 1 FROM se_q WHERE se_q.k = se_r.k);
SELECT remove_provenance('se1');
SELECT a, p FROM se1 ORDER BY a;
DROP TABLE se1;

-- NOT EXISTS: the complement -- k=1 -> 0.5, k=2 -> 0.25, k=3 -> 1.
CREATE TABLE se2 AS
  SELECT se_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM se_r WHERE NOT EXISTS (SELECT 1 FROM se_q WHERE se_q.k = se_r.k);
SELECT remove_provenance('se2');
SELECT a, p FROM se2 ORDER BY a;
DROP TABLE se2;

-- IN matches EXISTS; the correlation key is taken from the IN testexpr.
CREATE TABLE se3 AS
  SELECT se_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM se_r WHERE se_r.k IN (SELECT se_q.k FROM se_q);
SELECT remove_provenance('se3');
SELECT a, p FROM se3 ORDER BY a;
DROP TABLE se3;

-- NOT IN matches NOT EXISTS.
CREATE TABLE se4 AS
  SELECT se_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM se_r WHERE se_r.k NOT IN (SELECT se_q.k FROM se_q);
SELECT remove_provenance('se4');
SELECT a, p FROM se4 ORDER BY a;
DROP TABLE se4;

-- IN with an extra subselect filter: the testexpr key is ANDed with the body
-- WHERE.  q.x >= 200 drops the k=1 match (x=100), so k=1 -> 0, k=2 -> 0.75.
CREATE TABLE se5 AS
  SELECT se_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM se_r WHERE se_r.k IN (SELECT se_q.k FROM se_q WHERE se_q.x >= 200);
SELECT remove_provenance('se5');
SELECT a, p FROM se5 ORDER BY a;
DROP TABLE se5;

DROP TABLE se_q;
DROP TABLE se_r;

-- Part 10: a WHERE comparison of a scalar subquery against an OUTER column (a
-- per-group variable, not a literal).  The conjunct lifts to HAVING as
-- choose(q.x) = R.col; R.col is a GROUP BY key, so the cmp builder wraps it like
-- a constant.  A WHERE comparison also needs the subquery to return a row, so
-- the antijoin (empty) group is gated out by count(Q.key) >= 1 (not merely <=1).
CREATE TABLE cv_r(a int, k int);
CREATE TABLE cv_q(k int, x int);
INSERT INTO cv_r VALUES (100,1),(200,2),(300,3);
INSERT INTO cv_q VALUES (1,100),(2,999);  -- k=1: x=100 (=r.a), k=2: x=999, k=3: none
SELECT add_provenance('cv_r');
SELECT add_provenance('cv_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM cv_r;
  PERFORM set_prob(provsql, 0.5) FROM cv_q;
END $$;

-- (SELECT cv_q.x ...) = cv_r.a : a=100 -> 0.5 (x=100=a iff present); a=200 -> 0
-- (x=999<>200); a=300 -> 0 (no match: NULL comparison, group gated out).
CREATE TABLE cv1 AS
  SELECT cv_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM cv_r WHERE (SELECT cv_q.x FROM cv_q WHERE cv_q.k = cv_r.k) = cv_r.a;
SELECT remove_provenance('cv1');
SELECT a, p FROM cv1 ORDER BY a;
DROP TABLE cv1;

DROP TABLE cv_q;
DROP TABLE cv_r;

-- Part 11: ARRAY(SELECT Q.x FROM Q WHERE corr) collects the correlated rows, so
-- it decorrelates to array_agg(Q.x) over the "R LEFT JOIN Q" group (no count
-- gate -- an array may hold zero, one, or many elements).  array_agg keeps
-- NULLs, so the null-padded antijoin row is excluded with a FILTER on the
-- correlation key (Q.key IS NULL only on that row); a genuinely-NULL matched
-- element is still collected.  The result is an agg_token; its array value is
-- extracted and sorted below for a stable comparison.
CREATE TABLE av_r(a int, k int);
CREATE TABLE av_q(k int, x int);
INSERT INTO av_r VALUES (10,1),(20,2),(30,3);
-- k=1: one value 100 plus a NULL element; k=2: two values; k=3: no match.
INSERT INTO av_q VALUES (1,100),(1,NULL),(2,200),(2,201);
SELECT add_provenance('av_r');
SELECT add_provenance('av_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM av_r;
  PERFORM set_prob(provsql, 0.5) FROM av_q;
END $$;

-- k=1 -> {100,NULL} (NULL matched element kept), k=2 -> {200,201}, k=3 -> NULL
-- (empty correlated group).  Each row exists (av_r certain), so p = 1.
CREATE TABLE av1 AS
  SELECT av_r.a AS a,
         ARRAY(SELECT av_q.x FROM av_q WHERE av_q.k = av_r.k) AS arr,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM av_r;
SELECT remove_provenance('av1');
SELECT a,
       (SELECT array_agg(u ORDER BY u NULLS LAST)
          FROM unnest(split_part(agg_token_value_text(arr::uuid), ' ', 1)::int[]) u)
         AS sorted,
       p
FROM av1 ORDER BY a;
DROP TABLE av1;

DROP TABLE av_q;
DROP TABLE av_r;

-- Part 12: a multi-table subquery body.  (SELECT val FROM Q1, Q2 WHERE corr) has
-- its comma-join collapsed into a single derived cross-product subquery D, after
-- which the body is "SELECT val FROM D WHERE corr" and the usual single-Q path
-- runs (R LEFT JOIN D, with corr -- correlation + inter-table join -- as the ON).
-- D is processed recursively, so both Q1 and Q2 contribute provenance.
CREATE TABLE mt_r(a int, k int);
CREATE TABLE mt_q(k int, x int);
CREATE TABLE mt_s(k int, w int);
INSERT INTO mt_r VALUES (10,1),(20,2),(30,3);
INSERT INTO mt_q VALUES (1,100),(2,200);
INSERT INTO mt_s VALUES (1,5),(2,5);  -- one s per k, so the body yields <=1 row
SELECT add_provenance('mt_r');
SELECT add_provenance('mt_q');
SELECT add_provenance('mt_s');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM mt_r;
  PERFORM set_prob(provsql, 0.5) FROM mt_q;
  PERFORM set_prob(provsql, 0.5) FROM mt_s;
END $$;

-- WHERE (SELECT mt_q.x FROM mt_q, mt_s WHERE mt_q.k=r.k AND mt_s.k=mt_q.k) > 50:
-- the value (100 / 200) clears 50, so the row exists iff BOTH its mt_q and mt_s
-- rows are present -> p = 0.5 * 0.5 = 0.25; k=3 has no match -> 0.
CREATE TABLE mt1 AS
  SELECT mt_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM mt_r
  WHERE (SELECT mt_q.x FROM mt_q, mt_s
         WHERE mt_q.k = mt_r.k AND mt_s.k = mt_q.k) > 50;
SELECT remove_provenance('mt1');
SELECT a, p FROM mt1 ORDER BY a;
DROP TABLE mt1;

-- Aggregate body over the same multi-table FROM: count(*) over the join.
CREATE TABLE mt2 AS
  SELECT mt_r.a AS a,
         (SELECT count(*) FROM mt_q, mt_s
          WHERE mt_q.k = mt_r.k AND mt_s.k = mt_q.k) AS c
  FROM mt_r;
SELECT remove_provenance('mt2');
SELECT a, c FROM mt2 ORDER BY a;
DROP TABLE mt2;

DROP TABLE mt_s;
DROP TABLE mt_q;
DROP TABLE mt_r;
