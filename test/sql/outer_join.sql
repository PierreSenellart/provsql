\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Outer-join provenance (LEFT / RIGHT / FULL).  The planner lowers an outer
-- join of two base relations into (matched) UNION ALL (null-padded antijoin
-- branch(es)), so the non-monotone null-padded rows -- which appear only in the
-- smaller worlds where a side has no match -- are captured.  Probabilities are
-- pinned across possible worlds (existence, count=0, count>=2 / <=1).

-- Tuple-independent setup:
--   oj_l(k) = {1, 2}, present with probability 1
--   oj_r(k,v) = {(1,10),(1,20),(3,30)}, each independent at 0.5
-- so left key 2 is unmatched, right key 3 is unmatched, and key 1 has two
-- independent matches.
CREATE TABLE oj_l(k int);
CREATE TABLE oj_r(k int, v int);
INSERT INTO oj_l VALUES (1),(2);
INSERT INTO oj_r VALUES (1,10),(1,20),(3,30);
SELECT add_provenance('oj_l');
SELECT add_provenance('oj_r');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM oj_l;
  PERFORM set_prob(provsql, 0.5) FROM oj_r;
END $$;

-- LEFT JOIN, group existence: every left row survives, so both groups always
-- exist (oj_l present at 1): k=1 -> 1, k=2 -> 1.
CREATE TABLE oj_t AS
  SELECT oj_l.k AS k, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM oj_l LEFT JOIN oj_r ON oj_r.k = oj_l.k GROUP BY oj_l.k;
SELECT remove_provenance('oj_t');
SELECT 'LEFT exists' AS q, k, p FROM oj_t ORDER BY k;
DROP TABLE oj_t;

-- LEFT JOIN, HAVING count(oj_r.k)=0 : the no-match world.
--   k=1 -> P(neither match) = 0.25 ; k=2 -> 1 (never matched).
CREATE TABLE oj_t AS
  SELECT oj_l.k AS k, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM oj_l LEFT JOIN oj_r ON oj_r.k = oj_l.k GROUP BY oj_l.k
  HAVING count(oj_r.k) = 0;
SELECT remove_provenance('oj_t');
SELECT 'LEFT count=0' AS q, k, p FROM oj_t ORDER BY k;
DROP TABLE oj_t;

-- LEFT JOIN, HAVING count(oj_r.k)>=2 : both matches present.
--   k=1 -> P(both) = 0.25 ; k=2 excluded.
CREATE TABLE oj_t AS
  SELECT oj_l.k AS k, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM oj_l LEFT JOIN oj_r ON oj_r.k = oj_l.k GROUP BY oj_l.k
  HAVING count(oj_r.k) >= 2;
SELECT remove_provenance('oj_t');
SELECT 'LEFT count>=2' AS q, k, p FROM oj_t ORDER BY k;
DROP TABLE oj_t;

-- RIGHT JOIN, group existence (keyed by the right side):
--   k=1 -> P(m10 or m20) = 0.75 ; k=3 -> P(m30) = 0.5 (left NULL-padded).
CREATE TABLE oj_t AS
  SELECT oj_r.k AS k, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM oj_l RIGHT JOIN oj_r ON oj_r.k = oj_l.k GROUP BY oj_r.k;
SELECT remove_provenance('oj_t');
SELECT 'RIGHT exists' AS q, k, p FROM oj_t ORDER BY k;
DROP TABLE oj_t;

-- FULL JOIN, group existence over both sides:
--   k=1 -> 1 (matched, oj_l present) ; k=2 -> 1 (left-unmatched) ;
--   k=3 -> 0.5 (right-unmatched).
CREATE TABLE oj_t AS
  SELECT coalesce(oj_l.k, oj_r.k) AS k,
         round(probability_evaluate(provenance())::numeric,4) AS p
  FROM oj_l FULL JOIN oj_r ON oj_r.k = oj_l.k
  GROUP BY coalesce(oj_l.k, oj_r.k);
SELECT remove_provenance('oj_t');
SELECT 'FULL exists' AS q, k, p FROM oj_t ORDER BY k;
DROP TABLE oj_t;

DROP TABLE oj_l;
DROP TABLE oj_r;

-- BID (repair_key) right side: two matches for left key 1 in one block, so they
-- are MUTUALLY EXCLUSIVE -- at most one can match.
--   oj_b block 1: (k=1,v=10 @0.5), (k=1,v=20 @0.3); P(neither) = 0.2.
CREATE TABLE oj_l2(k int);
INSERT INTO oj_l2 VALUES (1);
SELECT add_provenance('oj_l2');
CREATE TABLE oj_b(blk int, k int, v int, p float);
INSERT INTO oj_b VALUES (1,1,10,0.5),(1,1,20,0.3);
SELECT repair_key('oj_b','blk');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM oj_l2;
  PERFORM set_prob(provenance(), p) FROM oj_b;
END $$;

-- count(oj_b.k) over the k=1 group can only be 0 or 1 (the two matches exclude
-- each other): count<=1 -> 1, count>=1 -> 0.8, count=0 -> 0.2.
CREATE TABLE oj_t AS
  SELECT oj_l2.k AS k, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM oj_l2 LEFT JOIN oj_b ON oj_b.k = oj_l2.k GROUP BY oj_l2.k
  HAVING count(oj_b.k) <= 1;
SELECT remove_provenance('oj_t');
SELECT 'BID count<=1' AS q, k, p FROM oj_t ORDER BY k;
DROP TABLE oj_t;

CREATE TABLE oj_t AS
  SELECT oj_l2.k AS k, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM oj_l2 LEFT JOIN oj_b ON oj_b.k = oj_l2.k GROUP BY oj_l2.k
  HAVING count(oj_b.k) >= 1;
SELECT remove_provenance('oj_t');
SELECT 'BID count>=1' AS q, k, p FROM oj_t ORDER BY k;
DROP TABLE oj_t;

CREATE TABLE oj_t AS
  SELECT oj_l2.k AS k, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM oj_l2 LEFT JOIN oj_b ON oj_b.k = oj_l2.k GROUP BY oj_l2.k
  HAVING count(oj_b.k) = 0;
SELECT remove_provenance('oj_t');
SELECT 'BID count=0' AS q, k, p FROM oj_t ORDER BY k;
DROP TABLE oj_t;

DROP TABLE oj_l2;
DROP TABLE oj_b;
