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

-- Part 13: an UNcorrelated scalar subquery is a single constant value, so it is
-- moved out of the SELECT list into a one-row derived aggregate cross-joined
-- into the outer FROM (no correlation key, so the LEFT-JOIN path does not apply).
-- An aggregate body goes as-is; a value body becomes choose(val) with a baked-in
-- HAVING count(*) <= 1.  Faithful to ProvSQL aggregates: an empty group yields a
-- gate_zero row that drops, so the empty world is not kept (unlike the
-- correlated path's 0-match NULL row).
CREATE TABLE uc_r(a int);
CREATE TABLE uc_q(x int);
INSERT INTO uc_r VALUES (10),(20);
INSERT INTO uc_q VALUES (100),(200),(201);
SELECT add_provenance('uc_r');
SELECT add_provenance('uc_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM uc_r;
  PERFORM set_prob(provsql, 0.5) FROM uc_q;
END $$;

-- Aggregate body: count(*) over all of uc_q.  Value 3.  (SELECT count(*) FROM
-- uc_q) is a SCALAR aggregation -- it always returns one row (count 0 in the
-- all-absent world), so the outer row always exists: p = 1.0.  (uc_r is certain;
-- the count subquery's existence does not gate it -- the scalar-aggregation
-- existence = gate_one fix.)
CREATE TABLE uc1 AS
  SELECT uc_r.a AS a, (SELECT count(*) FROM uc_q) AS c,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM uc_r;
SELECT remove_provenance('uc1');
SELECT a, c, p FROM uc1 ORDER BY a;
DROP TABLE uc1;

-- Value body: (SELECT uc_q.x FROM uc_q) -> choose(x) + count(*) <= 1.  The row
-- exists whenever the scalar subquery is well-defined: the empty world returns a
-- (valid) NULL and the single-row worlds return the value; only the >1-row worlds
-- are the SQL runtime error that count(*) <= 1 gates out.  So p = P(count<=1) =
-- P(0 present) + P(exactly 1 of 3) = 0.125 + 0.375 = 0.5.  (The empty-input world
-- is real here because count is a scalar aggregation -- the agg-gate scalar flag.)
CREATE TABLE uc2 AS
  SELECT uc_r.a AS a,
         (SELECT uc_q.x FROM uc_q) AS v,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM uc_r;
SELECT remove_provenance('uc2');
SELECT a, p FROM uc2 ORDER BY a;
DROP TABLE uc2;

-- Value body, single matching row: the value flows straight through.
CREATE TABLE uc3 AS
  SELECT uc_r.a AS a, (SELECT uc_q.x FROM uc_q WHERE uc_q.x = 100) AS v
  FROM uc_r;
SELECT remove_provenance('uc3');
SELECT a, v FROM uc3 ORDER BY a;
DROP TABLE uc3;

DROP TABLE uc_q;
DROP TABLE uc_r;

-- Part 14: UNcorrelated EXISTS and uncorrelated aggregate comparisons in WHERE.
-- The predicate is pushed into a one-row HAVING-gated subquery cross-joined into
-- the FROM, so the conjunct becomes R (x) [predicate].  ProvSQL's HAVING
-- annotates (the aggregate row is always materialised, gated), so no
-- actual-instance row is needed.  Faithful to ProvSQL aggregates: the empty-Q
-- world drops, so NOT EXISTS (satisfied only by the empty group) stays rejected.
CREATE TABLE ue_r(a int);
CREATE TABLE ue_q(x int);
INSERT INTO ue_r VALUES (10),(20);
INSERT INTO ue_q VALUES (100),(200),(201);
SELECT add_provenance('ue_r');
SELECT add_provenance('ue_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM ue_r;
  PERFORM set_prob(provsql, 0.5) FROM ue_q;
END $$;

-- EXISTS: row exists iff >=1 of the three ue_q rows present, p = 1 - 0.5^3 = 0.875.
CREATE TABLE ue1 AS
  SELECT ue_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ue_r WHERE EXISTS (SELECT 1 FROM ue_q);
SELECT remove_provenance('ue1');
SELECT a, p FROM ue1 ORDER BY a;
DROP TABLE ue1;

-- EXISTS with a body filter: P(>=1 of {200,201}) = 1 - 0.25 = 0.75.
CREATE TABLE ue2 AS
  SELECT ue_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ue_r WHERE EXISTS (SELECT 1 FROM ue_q WHERE ue_q.x >= 200);
SELECT remove_provenance('ue2');
SELECT a, p FROM ue2 ORDER BY a;
DROP TABLE ue2;

-- count(*) >= 2: P(>=2 of 3 present) = 0.5.
CREATE TABLE ue3 AS
  SELECT ue_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ue_r WHERE (SELECT count(*) FROM ue_q) >= 2;
SELECT remove_provenance('ue3');
SELECT a, p FROM ue3 ORDER BY a;
DROP TABLE ue3;

-- count(*) > 5 is unsatisfiable (only 3 rows): every row is gated to probability
-- 0 (the rows are still materialised, annotated, as ProvSQL's HAVING does).
CREATE TABLE ue4 AS
  SELECT ue_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ue_r WHERE (SELECT count(*) FROM ue_q) > 5;
SELECT remove_provenance('ue4');
SELECT a, p FROM ue4 ORDER BY a;
DROP TABLE ue4;

-- max(x) > 150: P(>=1 of {200,201} present) = 0.75.
CREATE TABLE ue5 AS
  SELECT ue_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ue_r WHERE (SELECT max(ue_q.x) FROM ue_q) > 150;
SELECT remove_provenance('ue5');
SELECT a, p FROM ue5 ORDER BY a;
DROP TABLE ue5;

DROP TABLE ue_q;
DROP TABLE ue_r;

-- Part 15: an UNcorrelated NOT EXISTS is the m-semiring antijoin.  It is rewritten
-- to the EXCEPT-ALL difference  R EXCEPT ALL π_R(R × σ_w(Q)), replacing R in the
-- FROM, so each kept tuple gets  R(r) ⊖ (R(r) ⊗ ⊕_{q:w} Q(q)) = R(r) ⊗ (1 ⊖ ⊕Q)
-- -- multiplicity preserved (EXCEPT ALL, no GROUP BY), correct in every semiring,
-- and it materialises every R row from the always-present left arm (so the empty-Q
-- world a count(*)=0 HAVING would drop is kept).
CREATE TABLE ne_r(a int);
CREATE TABLE ne_q(x int);
INSERT INTO ne_r VALUES (10),(20),(10);     -- duplicate a=10 tests multiplicity
INSERT INTO ne_q VALUES (100),(200),(201);
SELECT add_provenance('ne_r');
SELECT add_provenance('ne_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM ne_r;
  PERFORM set_prob(provsql, 0.5) FROM ne_q;
END $$;

-- bare NOT EXISTS: each of the 3 R rows survives iff ne_q is empty,
-- p = P(all three absent) = 0.5^3 = 0.125.  All three rows are kept (multiplicity).
CREATE TABLE ne1 AS
  SELECT ne_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ne_r WHERE NOT EXISTS (SELECT 1 FROM ne_q);
SELECT remove_provenance('ne1');
SELECT a, p FROM ne1 ORDER BY a;
DROP TABLE ne1;

-- NOT EXISTS with a body filter (x > 150): survives iff no ne_q with x>150 present,
-- p = P(200 absent AND 201 absent) = 0.25.
CREATE TABLE ne2 AS
  SELECT ne_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ne_r WHERE NOT EXISTS (SELECT 1 FROM ne_q WHERE ne_q.x > 150);
SELECT remove_provenance('ne2');
SELECT a, p FROM ne2 ORDER BY a;
DROP TABLE ne2;

-- A retained ordinary conjunct (a > 15) applies on top of the antijoin.
CREATE TABLE ne3 AS
  SELECT ne_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ne_r WHERE ne_r.a > 15 AND NOT EXISTS (SELECT 1 FROM ne_q);
SELECT remove_provenance('ne3');
SELECT a, p FROM ne3 ORDER BY a;
DROP TABLE ne3;

DROP TABLE ne_q;
DROP TABLE ne_r;

-- Part 16: an uncorrelated count(*) comparison in WHERE.  A predicate FALSE on
-- the empty group (count(*) >= k, = k for k>=1, …) is the safe HAVING-gate
-- (move_uncorrelated_where_predicates); one TRUE on the empty group (count(*)
-- < k, <= k, = 0) is "NOT (a false-on-empty predicate)", so it routes through
-- the same EXCEPT-ALL antijoin as NOT EXISTS -- otherwise the empty-Q world
-- (gate_zero) would be silently dropped, under-counting the predicate.
CREATE TABLE ce_r(a int);
CREATE TABLE ce_q(x int);
INSERT INTO ce_r VALUES (10),(20);
INSERT INTO ce_q VALUES (100),(200),(201);   -- 3 i.i.d. rows at 0.5
SELECT add_provenance('ce_r');
SELECT add_provenance('ce_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM ce_r;
  PERFORM set_prob(provsql, 0.5) FROM ce_q;
END $$;

-- count(*) < 2 (true on empty) -> P(<=1 of 3) = 0.5  (not the 0.375 a HAVING-gate
-- would give by dropping the count=0 world).
CREATE TABLE ce1 AS
  SELECT ce_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ce_r WHERE (SELECT count(*) FROM ce_q) < 2;
SELECT remove_provenance('ce1');
SELECT a, p FROM ce1 ORDER BY a;
DROP TABLE ce1;

-- count(*) = 0 -> P(empty) = 0.125  (NOT EXISTS spelled as a count).
CREATE TABLE ce2 AS
  SELECT ce_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ce_r WHERE (SELECT count(*) FROM ce_q) = 0;
SELECT remove_provenance('ce2');
SELECT a, p FROM ce2 ORDER BY a;
DROP TABLE ce2;

-- count(*) >= 2 (false on empty) -> the complement, P(>=2 of 3) = 0.5.
CREATE TABLE ce3 AS
  SELECT ce_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ce_r WHERE (SELECT count(*) FROM ce_q) >= 2;
SELECT remove_provenance('ce3');
SELECT a, p FROM ce3 ORDER BY a;
DROP TABLE ce3;

-- count(*) = 2 (false on empty) -> P(exactly 2 of 3) = 0.375.
CREATE TABLE ce4 AS
  SELECT ce_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ce_r WHERE (SELECT count(*) FROM ce_q) = 2;
SELECT remove_provenance('ce4');
SELECT a, p FROM ce4 ORDER BY a;
DROP TABLE ce4;

DROP TABLE ce_q;
DROP TABLE ce_r;

-- Part 17: count(col) (not count(*)) takes the same true-on-empty antijoin, but
-- D's HAVING reuses the original count aggregate, so count(col)'s NULL semantics
-- hold.  cn_q has a NULL x, so count(x) counts only the two non-NULL rows;
-- count(x) < 1 (true on empty) -> P(both non-NULL rows absent) = 0.25.
CREATE TABLE cn_r(a int);
CREATE TABLE cn_q(x int);
INSERT INTO cn_r VALUES (10),(20);
INSERT INTO cn_q VALUES (100),(200),(NULL);
SELECT add_provenance('cn_r');
SELECT add_provenance('cn_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM cn_r;
  PERFORM set_prob(provsql, 0.5) FROM cn_q;
END $$;

CREATE TABLE cn1 AS
  SELECT cn_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM cn_r WHERE (SELECT count(x) FROM cn_q) < 1;
SELECT remove_provenance('cn1');
SELECT a, p FROM cn1 ORDER BY a;
DROP TABLE cn1;

DROP TABLE cn_q;
DROP TABLE cn_r;

-- Part 18: a subquery whose body touches NO provenance-tracked relation is a
-- deterministic condition/value (untracked data is certain -- the same in every
-- possible world), so it is passed through to Postgres as an ordinary filter /
-- value and the row keeps R's provenance.  Only sublinks over a tracked relation
-- are rejected.
CREATE TABLE pt_r(a int, k int);   -- tracked
CREATE TABLE pt_u(k int);           -- NOT provenance-tracked
INSERT INTO pt_r VALUES (10,1),(20,2),(30,3);
INSERT INTO pt_u VALUES (1),(2);
SELECT add_provenance('pt_r');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM pt_r; END $$;

-- EXISTS over the untracked table just filters: k=1,2 match (kept), k=3 dropped;
-- the kept rows exist with pt_r's own probability 0.5 (the EXISTS adds nothing).
CREATE TABLE pt1 AS
  SELECT pt_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM pt_r WHERE EXISTS (SELECT 1 FROM pt_u WHERE pt_u.k = pt_r.k);
SELECT remove_provenance('pt1');
SELECT a, p FROM pt1 ORDER BY a;
DROP TABLE pt1;

-- A scalar value over the untracked table is a deterministic column; the row's
-- provenance is still pt_r's, so every pt_r row exists with probability 0.5.
CREATE TABLE pt2 AS
  SELECT pt_r.a AS a,
         (SELECT pt_u.k FROM pt_u WHERE pt_u.k = pt_r.k) AS v,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM pt_r;
SELECT remove_provenance('pt2');
SELECT a, v, p FROM pt2 ORDER BY a;
DROP TABLE pt2;

DROP TABLE pt_u;
DROP TABLE pt_r;

-- Part 19: quantified comparisons.  "op ANY" (IN) is a semijoin; "op ALL" is its
-- universal dual, the antijoin (∀q. x op q = ¬∃q. x ¬op q), so it reuses the
-- NOT-IN lowering with the operator negated in the correlation.  A row IN splits
-- its BoolExpr-AND testexpr into per-column equality conjuncts.
CREATE TABLE qf_r(a int, k int);
CREATE TABLE qf_q(k int, x int);
INSERT INTO qf_r VALUES (100,1),(200,2),(300,3);
INSERT INTO qf_q VALUES (1,100),(2,200),(2,201);   -- q.k in {1,2,2}, each @0.5
SELECT add_provenance('qf_r');
SELECT add_provenance('qf_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM qf_r;
  PERFORM set_prob(provsql, 0.5) FROM qf_q;
END $$;

-- k <> ALL (q.k)  ==  k NOT IN (q.k): k=1 -> 0.5, k=2 -> 0.25, k=3 -> 1.0.
CREATE TABLE qf1 AS
  SELECT qf_r.k AS k, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM qf_r WHERE qf_r.k <> ALL (SELECT qf_q.k FROM qf_q);
SELECT remove_provenance('qf1');
SELECT k, p FROM qf1 ORDER BY k;
DROP TABLE qf1;

-- k > ALL (q.k): true iff k exceeds every present q.k.  k=1 -> 0.125 (only the
-- empty world), k=2 -> 0.25 (both 2's absent), k=3 -> 1.0 (beats {1,2,2} always).
CREATE TABLE qf2 AS
  SELECT qf_r.k AS k, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM qf_r WHERE qf_r.k > ALL (SELECT qf_q.k FROM qf_q);
SELECT remove_provenance('qf2');
SELECT k, p FROM qf2 ORDER BY k;
DROP TABLE qf2;

-- row IN: (k, a) IN (q.k, q.x).  (1,100) and (2,200) each match one q row -> 0.5;
-- (3,300) matches none -> 0.
CREATE TABLE qf3 AS
  SELECT qf_r.k AS k, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM qf_r WHERE (qf_r.k, qf_r.a) IN (SELECT qf_q.k, qf_q.x FROM qf_q);
SELECT remove_provenance('qf3');
SELECT k, p FROM qf3 ORDER BY k;
DROP TABLE qf3;

DROP TABLE qf_q;
DROP TABLE qf_r;

-- Part 20: ORDER BY … LIMIT 1 value body = argmax.  It decorrelates to
-- choose(val ORDER BY key) over the R ⟕ Q group: choose keeps the first row in
-- the aggregate's order, so DESC / ASC give the max / min-key row's value.  No
-- count <= 1 gate (LIMIT 1 legally takes the top of many matches); the
-- subselect's junk sort-key targetList entries + sortClause become the choose
-- Aggref's order arguments.
CREATE TABLE lt_c(id int, name text);
CREATE TABLE lt_o(id int, c int, seq int);
INSERT INTO lt_c VALUES (1,'Alice'),(2,'Bob');
INSERT INTO lt_o VALUES (100,1,30),(200,1,20),(300,2,10);  -- Alice: 100(seq30), 200; Bob: 300
SELECT add_provenance('lt_c');
SELECT add_provenance('lt_o');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM lt_c;
  PERFORM set_prob(provsql, 0.5) FROM lt_o;
END $$;

-- latest (max seq): Alice -> 100, Bob -> 300.  Alice has two orders, but LIMIT 1
-- means no count<=1 gate, so the row exists per lt_c (p = 1).
CREATE TABLE lt1 AS
  SELECT lt_c.name AS name,
         (SELECT o.id FROM lt_o o WHERE o.c = lt_c.id ORDER BY o.seq DESC LIMIT 1) AS latest,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM lt_c;
SELECT remove_provenance('lt1');
SELECT name, latest, p FROM lt1 ORDER BY name;
DROP TABLE lt1;

-- earliest (min seq, ASC): Alice -> 200, Bob -> 300.
CREATE TABLE lt2 AS
  SELECT lt_c.name AS name,
         (SELECT o.id FROM lt_o o WHERE o.c = lt_c.id ORDER BY o.seq ASC LIMIT 1) AS earliest
  FROM lt_c;
SELECT remove_provenance('lt2');
SELECT name, earliest FROM lt2 ORDER BY name;
DROP TABLE lt2;

DROP TABLE lt_o;
DROP TABLE lt_c;

-- Part 21: a SELECT DISTINCT value body.  The at-most-one-row rule of a scalar
-- subquery counts distinct VALUES, not rows, so the only change from the plain
-- value path is the gate: HAVING count(DISTINCT v) <= 1 instead of
-- count(Q.key) <= 1.  This admits "many matching rows, all the same value"
-- (which DISTINCT collapses to one) and still gates the >1-distinct-value worlds.
-- (Relies on the COUNT(DISTINCT)-over-an-outer-join fix.)
CREATE TABLE di_r(a int, k int);
CREATE TABLE di_q(k int, x int);
INSERT INTO di_r VALUES (10,1),(20,2),(30,3);
INSERT INTO di_q VALUES (1,100),(2,200),(2,200),(3,300),(3,301);  -- k=2 same value twice; k=3 distinct
SELECT add_provenance('di_r');
SELECT add_provenance('di_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM di_r;
  PERFORM set_prob(provsql, 0.5) FROM di_q;
END $$;

-- (SELECT DISTINCT x ...): k=1 -> 100 (p=1); k=2 -> 200 (p=1, two same-value rows
-- collapse to one distinct value); k=3 -> p=0.75 (two distinct values, gated when
-- both present: 1 - 0.5^2).
CREATE TABLE di1 AS
  SELECT di_r.a AS a,
         (SELECT DISTINCT di_q.x FROM di_q WHERE di_q.k = di_r.k) AS v,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM di_r;
SELECT remove_provenance('di1');
SELECT a, v, p FROM di1 ORDER BY a;
DROP TABLE di1;

DROP TABLE di_q;
DROP TABLE di_r;

-- Part 22: a scalar subquery nested inside a larger expression (arithmetic) is
-- NOT decorrelatable, but instead of rejecting it ProvSQL lets it through with a
-- warning: Postgres evaluates the sublink (the value is correct), the row keeps
-- only the OUTER relation's provenance, and the subquery's data is treated as
-- certain.  Contrast a direct comparison (no arithmetic), which still decorrelates
-- into a gated cmp.  A genuinely unsupported DIRECT form (a GROUP BY body) still
-- raises the clean error.
CREATE TABLE ne_r(a int, k int);
CREATE TABLE ne_q(k int, x int);
INSERT INTO ne_r VALUES (10,1),(20,2),(30,3);
INSERT INTO ne_q VALUES (1,100),(2,200),(3,300);
SELECT add_provenance('ne_r');
SELECT add_provenance('ne_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM ne_r;
  PERFORM set_prob(provsql, 0.5) FROM ne_q;
END $$;

-- target-list arithmetic: value = x+1 (correct), provenance is outer-only so p=1.
CREATE TABLE ne1 AS
  SELECT ne_r.a AS a,
         (SELECT ne_q.x FROM ne_q WHERE ne_q.k = ne_r.k) + 1 AS v1,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ne_r;
SELECT remove_provenance('ne1');
SELECT a, v1, p FROM ne1 ORDER BY a;
DROP TABLE ne1;

-- WHERE arithmetic: x+1 > 150 keeps a=20,30; provenance outer-only so p=1.
CREATE TABLE ne2 AS
  SELECT ne_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ne_r WHERE (SELECT ne_q.x FROM ne_q WHERE ne_q.k = ne_r.k) + 1 > 150;
SELECT remove_provenance('ne2');
SELECT a, p FROM ne2 ORDER BY a;
DROP TABLE ne2;

DROP TABLE ne_q;
DROP TABLE ne_r;

-- Part 23: several correlated target-list sublinks that share one (Q, corr)
-- coalesce onto a single LEFT JOIN -- one count(Q.key) <= 1 gate, a choose() per
-- sublink.  v1, v2 must match what each sublink alone would decorrelate to; the
-- shared gate still excludes the >=2-match worlds; a missing match leaves both
-- values NULL with the row present (outer-join null-padding).
CREATE TABLE co_r(a int, k int);
CREATE TABLE co_q(k int, x int, y int);
INSERT INTO co_r VALUES (1,10),(2,20),(3,30),(4,99);  -- k=99 has no match
INSERT INTO co_q VALUES (10,100,1000),(20,200,2000),(20,201,2001),(30,300,3000);
SELECT add_provenance('co_r');
SELECT add_provenance('co_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM co_r;
  PERFORM set_prob(provsql, 0.5) FROM co_q;
END $$;

-- k=10,30 -> single match (p=1); k=20 -> two matches, count<=1 gate -> p=0.75
-- (1 - 0.5^2); k=99 -> no match, both NULL, row present (p=1).
CREATE TABLE co1 AS
  SELECT co_r.a AS a,
         (SELECT co_q.x FROM co_q WHERE co_q.k = co_r.k) AS v1,
         (SELECT co_q.y FROM co_q WHERE co_q.k = co_r.k) AS v2,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM co_r;
SELECT remove_provenance('co1');
SELECT a, v1, v2, p FROM co1 ORDER BY a;
DROP TABLE co1;

DROP TABLE co_q;
DROP TABLE co_r;

-- Part 24: an UNcorrelated value-body comparison in WHERE,
-- (SELECT Q.x FROM Q) OP v, is gated like the aggregate-comparison form: a
-- one-row HAVING-gated subquery D = (SELECT 1 FROM Q HAVING (choose(x) OP v) AND
-- count(*) <= 1) cross-joined into the FROM.  The comparison's truth becomes the
-- provenance gate (R ⊗ [choose(x) OP v]); an empty / >1-row Q is gated out.
CREATE TABLE uv_r(a int);
CREATE TABLE uv_q(x int);
INSERT INTO uv_r VALUES (1),(2),(3);
INSERT INTO uv_q VALUES (100);
SELECT add_provenance('uv_r');
SELECT add_provenance('uv_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM uv_r;
  PERFORM set_prob(provsql, 0.5) FROM uv_q;
END $$;

-- (SELECT uv_q.x FROM uv_q) > 50 is true, gated on uv_q present -> p = 0.5.
CREATE TABLE uv1 AS
  SELECT uv_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM uv_r WHERE (SELECT uv_q.x FROM uv_q) > 50;
SELECT remove_provenance('uv1');
SELECT 'gt50' AS t, a, p FROM uv1 ORDER BY a;
DROP TABLE uv1;

-- (SELECT uv_q.x FROM uv_q) > 500 is false -> rows present but provenance-gated
-- to p = 0 (ProvSQL HAVING annotates rather than physically filtering).
CREATE TABLE uv2 AS
  SELECT uv_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM uv_r WHERE (SELECT uv_q.x FROM uv_q) > 500;
SELECT remove_provenance('uv2');
SELECT 'gt500' AS t, a, p FROM uv2 ORDER BY a;
DROP TABLE uv2;

DROP TABLE uv_q;
DROP TABLE uv_r;

-- Part 25: an ORDER BY inside ARRAY(...).  ARRAY(SELECT v FROM Q WHERE corr
-- ORDER BY key) decorrelates to the ordered aggregate array_agg(v ORDER BY key)
-- over the R ⟕ Q group: the body's ORDER BY moves inside the aggregate, where it
-- survives the regroup (the same technique as the LIMIT-1 argmax choose).  The
-- null-padded antijoin row is still dropped by the FILTER.  (An empty match
-- yields a NULL-valued agg_token rather than {}, a pre-existing trait of the
-- FILTER-based array decorrelation, shared with the non-ordered ARRAY path.)
CREATE TABLE aa_r(a int, k int);
CREATE TABLE aa_q(k int, x int);
INSERT INTO aa_r VALUES (1,10),(2,20),(3,99);  -- k=99 has no match
INSERT INTO aa_q VALUES (10,3),(10,1),(10,2),(20,50),(20,40);
SELECT add_provenance('aa_r');
SELECT add_provenance('aa_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM aa_r;
  PERFORM set_prob(provsql, 0.5) FROM aa_q;
END $$;

-- ASC: {1,2,3} / {40,50}; DESC reverses; the provenance is the array_agg's
-- agg_token over the matched aa_q tuples, so probability_evaluate sees them all.
CREATE TABLE aa1 AS
  SELECT aa_r.a AS a,
         ARRAY(SELECT aa_q.x FROM aa_q WHERE aa_q.k = aa_r.k ORDER BY aa_q.x) AS asc_arr
  FROM aa_r;
SELECT remove_provenance('aa1');
SELECT a, asc_arr FROM aa1 ORDER BY a;
DROP TABLE aa1;

CREATE TABLE aa2 AS
  SELECT aa_r.a AS a,
         ARRAY(SELECT aa_q.x FROM aa_q WHERE aa_q.k = aa_r.k ORDER BY aa_q.x DESC) AS desc_arr
  FROM aa_r;
SELECT remove_provenance('aa2');
SELECT a, desc_arr FROM aa2 ORDER BY a;
DROP TABLE aa2;

DROP TABLE aa_q;
DROP TABLE aa_r;

-- Part 26: IN / NOT IN / NOT EXISTS whose body joins SEVERAL tracked relations.
-- predicate_subselect_decorrelatable accepts an all-tracked comma-join body;
-- the count-predicate lowering then collapses it into one derived cross-product
-- subquery D (oj_wrap_body_from), so the semijoin is R⊗⊕(Q1⊗Q2) and the
-- antijoin R⊗(1⊖⊕(Q1⊗Q2)) -- the same provenance as the EXCEPT workaround.
CREATE TABLE mp_r(a int, k int);
CREATE TABLE mp_q1(a int, k int);
CREATE TABLE mp_q2(k int);
INSERT INTO mp_r VALUES (1,1),(2,2),(3,3);
INSERT INTO mp_q1 VALUES (2,1),(4,2);  -- only (2,1) joins mp_q2
INSERT INTO mp_q2 VALUES (1),(3);
SELECT add_provenance('mp_r');
SELECT add_provenance('mp_q1');
SELECT add_provenance('mp_q2');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM mp_r;
  PERFORM set_prob(provsql, 0.5) FROM mp_q1;
  PERFORM set_prob(provsql, 0.5) FROM mp_q2;
END $$;

-- NOT IN: the body's only match is a=2, via mp_q1(2,1)⊗mp_q2(1) (p=0.25), so
-- that row survives at 0.75 and the others at 1.
CREATE TABLE mp1 AS
  SELECT mp_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM mp_r WHERE mp_r.a NOT IN (SELECT mp_q1.a FROM mp_q1, mp_q2 WHERE mp_q1.k = mp_q2.k);
SELECT remove_provenance('mp1');
SELECT a, p FROM mp1 ORDER BY a;
DROP TABLE mp1;

-- IN is the exact complement: a=2 -> 0.25, a=1,3 -> 0.
CREATE TABLE mp2 AS
  SELECT mp_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM mp_r WHERE mp_r.a IN (SELECT mp_q1.a FROM mp_q1, mp_q2 WHERE mp_q1.k = mp_q2.k);
SELECT remove_provenance('mp2');
SELECT a, p FROM mp2 ORDER BY a;
DROP TABLE mp2;

-- NOT EXISTS with the membership expressed as correlation: same antijoin.
CREATE TABLE mp3 AS
  SELECT mp_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM mp_r WHERE NOT EXISTS
    (SELECT 1 FROM mp_q1, mp_q2 WHERE mp_q1.k = mp_q2.k AND mp_q1.a = mp_r.a);
SELECT remove_provenance('mp3');
SELECT a, p FROM mp3 ORDER BY a;
DROP TABLE mp3;

-- The EXCEPT form computes the same probabilities (the historical workaround).
CREATE TABLE mp4 AS
  SELECT a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT a FROM mp_r EXCEPT
        SELECT mp_q1.a FROM mp_q1, mp_q2 WHERE mp_q1.k = mp_q2.k) e;
SELECT remove_provenance('mp4');
SELECT a, p FROM mp4 ORDER BY a;
DROP TABLE mp4;

-- Multi-column (row-wise) NOT IN over the multi-relation body: the testexpr
-- correlation is the row comparison.  (2,1) is the only body row; it matches
-- no (mp_r.a, mp_r.k), so every row survives at 1.
CREATE TABLE mp5 AS
  SELECT mp_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM mp_r WHERE (mp_r.a, mp_r.k) NOT IN
    (SELECT mp_q1.a, mp_q2.k FROM mp_q1, mp_q2 WHERE mp_q1.k = mp_q2.k);
SELECT remove_provenance('mp5');
SELECT a, p FROM mp5 ORDER BY a;
DROP TABLE mp5;

DROP TABLE mp_q2;
DROP TABLE mp_q1;
DROP TABLE mp_r;

-- Part 27: UNcorrelated aggregate bodies compared against an OUTER column, and
-- IN / NOT IN over a single bare-aggregate body.  A bare body (no Q-referencing
-- WHERE) with a non-star aggregate decorrelates to the R ⟕ Q ON TRUE group with
-- the comparison lifted to HAVING (no count key needed: the aggregate itself
-- ignores the null-padded row); and since an aggregate body always returns
-- exactly one row, "x op ANY/ALL (SELECT agg ..)" is first normalized to the
-- scalar comparison "x op (SELECT agg ..)" (negator op under NOT), which also
-- routes agg-vs-constant forms through the earlier uncorrelated passes.
CREATE TABLE ao_r(a int);
CREATE TABLE ao_q(a int);
INSERT INTO ao_r VALUES (1),(2),(3);
INSERT INTO ao_q VALUES (2),(4);
SELECT add_provenance('ao_r');
SELECT add_provenance('ao_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM ao_r;
  PERFORM set_prob(provsql, 0.5) FROM ao_q;
END $$;

-- max over the four ao_q worlds (each 0.25) is NULL / 2 / 4 / 4.
-- max > a: a=1 -> 0.75, a=2 -> 0.5, a=3 -> 0.5.
CREATE TABLE ao1 AS
  SELECT ao_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ao_r WHERE (SELECT max(ao_q.a) FROM ao_q) > ao_r.a;
SELECT remove_provenance('ao1');
SELECT a, p FROM ao1 ORDER BY a;
DROP TABLE ao1;

-- NOT IN ≡ a <> max: a=1 -> 0.75, a=2 -> 0.5, a=3 -> 0.75 (NULL max excluded).
CREATE TABLE ao2 AS
  SELECT ao_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ao_r WHERE ao_r.a NOT IN (SELECT max(ao_q.a) FROM ao_q);
SELECT remove_provenance('ao2');
SELECT a, p FROM ao2 ORDER BY a;
DROP TABLE ao2;

-- IN ≡ a = max: only a=2 in the {2} world -> 0.25.
CREATE TABLE ao3 AS
  SELECT ao_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ao_r WHERE ao_r.a IN (SELECT max(ao_q.a) FROM ao_q);
SELECT remove_provenance('ao3');
SELECT a, p FROM ao3 ORDER BY a;
DROP TABLE ao3;

-- count(col) is a NON-star aggregate, so it takes the same path; counts over
-- the worlds are 0/1/1/2.  count >= a: a=1 -> 0.75, a=2 -> 0.25, a=3 -> 0.
CREATE TABLE ao4 AS
  SELECT ao_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ao_r WHERE (SELECT count(ao_q.a) FROM ao_q) >= ao_r.a;
SELECT remove_provenance('ao4');
SELECT a, p FROM ao4 ORDER BY a;
DROP TABLE ao4;

-- The normalization also covers aggregate-vs-CONSTANT quantified forms, which
-- then ride the uncorrelated passes; count(*)-true-on-empty stays exact
-- because the normalization runs before rewrite_uncorrelated_antijoin.
-- 0 IN (count(*)): true only on the empty world -> 0.25.
CREATE TABLE ao5 AS
  SELECT ao_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ao_r WHERE 0 IN (SELECT count(*) FROM ao_q) AND ao_r.a = 1;
SELECT remove_provenance('ao5');
SELECT a, p FROM ao5 ORDER BY a;
DROP TABLE ao5;

-- 1 NOT IN (count(*)): counts 0/1/1/2 -> true in the {} and {2,4} worlds -> 0.5.
CREATE TABLE ao6 AS
  SELECT ao_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ao_r WHERE 1 NOT IN (SELECT count(*) FROM ao_q) AND ao_r.a = 1;
SELECT remove_provenance('ao6');
SELECT a, p FROM ao6 ORDER BY a;
DROP TABLE ao6;

-- op ALL over the single-row aggregate body is the same scalar comparison.
CREATE TABLE ao7 AS
  SELECT ao_r.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ao_r WHERE ao_r.a < ALL (SELECT max(ao_q.a) FROM ao_q);
SELECT remove_provenance('ao7');
SELECT a, p FROM ao7 ORDER BY a;
DROP TABLE ao7;

DROP TABLE ao_q;
DROP TABLE ao_r;
