\set ECHO none
\pset format unaligned

-- Rows the rewriting keeps for their provenance alone (a selection on
-- aggregate values keeps every row, annotated by the comparison) are rows
-- SQL never computes: the query's expressions must not fail on them where
-- SQL, on the database as it is, does not.  An error in one world is not an
-- answer for the others.
CREATE TABLE pr(x int, t int);
INSERT INTO pr VALUES (1, 10), (2, 25), (3, 45);
SELECT add_provenance('pr');

-- Consecutive ranks: a self-pair compares a rank with itself plus 1, false
-- in every world, and is dropped.  Plain SQL: (1, 100/15 = 6), (2, 100/20 = 5).
CREATE TABLE pr_r AS
  WITH n AS (SELECT x, t, ROW_NUMBER() OVER (ORDER BY t) AS rn FROM pr),
       p AS (SELECT a.x, b.t - a.t AS dt FROM n a JOIN n b ON a.rn + 1 = b.rn)
  SELECT x, dt, 100 / dt AS speed, plain_truth(provenance()) AS holds FROM p;
SELECT remove_provenance('pr_r');
SELECT x, speed FROM pr_r WHERE holds ORDER BY x;
SELECT count(*) FILTER (WHERE dt = 0) AS self_pairs FROM pr_r;
DROP TABLE pr_r;

-- Ranks in order: a self-pair is false in every world too, but not
-- recognised as such; it is kept, and its division by zero is NULL, as a
-- function undefined on its arguments is in the semantics.  Plain SQL:
-- (1, 100/15 = 6), (1, 100/35 = 2), (2, 100/20 = 5).
CREATE TABLE pr_r AS
  WITH n AS (SELECT x, t, ROW_NUMBER() OVER (ORDER BY t) AS rn FROM pr),
       p AS (SELECT a.x, b.t - a.t AS dt FROM n a JOIN n b ON a.rn < b.rn)
  SELECT x, dt, 100 / dt AS speed, plain_truth(provenance()) AS holds FROM p;
SELECT remove_provenance('pr_r');
SELECT x, speed FROM pr_r WHERE holds ORDER BY x, speed DESC;
SELECT count(*) FILTER (WHERE dt = 0) AS self_pairs,
       bool_and(speed IS NULL) FILTER (WHERE dt = 0) AS divided_by_zero_is_null
FROM pr_r;
DROP TABLE pr_r;

-- On the database as it is, where SQL itself divides by zero, the semantics
-- says nothing; a tracked query gives NULL there.
CREATE TABLE pr_r AS SELECT x, 1 / (x - x) AS undefined FROM pr;
SELECT remove_provenance('pr_r');
SELECT x, undefined FROM pr_r ORDER BY x;
DROP TABLE pr_r;

SELECT remove_provenance('pr');
DROP TABLE pr;
