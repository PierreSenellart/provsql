\set ECHO none
\pset format unaligned

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

-- Subquery arms: the lowering also fires when an outer-join arm is a subquery
-- over tracked relations (not only a base relation).
CREATE TABLE oj_subl(k int);
CREATE TABLE oj_subr(k int, v int);
INSERT INTO oj_subl VALUES (1),(2);
INSERT INTO oj_subr VALUES (1,10),(1,20);
SELECT add_provenance('oj_subl');
SELECT add_provenance('oj_subr');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM oj_subl;
  PERFORM set_prob(provsql, 0.5) FROM oj_subr;
END $$;

-- (SELECT k FROM oj_subl) LEFT JOIN oj_subr : group existence k=1 -> 1, k=2 -> 1.
CREATE TABLE oj_t AS
  SELECT s.k AS k, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM (SELECT k FROM oj_subl) s LEFT JOIN oj_subr ON oj_subr.k = s.k
  GROUP BY s.k;
SELECT remove_provenance('oj_t');
SELECT 'SUBQ-ARM exists' AS q, k, p FROM oj_t ORDER BY k;
DROP TABLE oj_t;

-- HAVING count(oj_subr.k)=0 : k=1 -> 0.25 (no match), k=2 -> 1 (never matched).
CREATE TABLE oj_t AS
  SELECT s.k AS k, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM (SELECT k FROM oj_subl) s LEFT JOIN oj_subr ON oj_subr.k = s.k
  GROUP BY s.k HAVING count(oj_subr.k) = 0;
SELECT remove_provenance('oj_t');
SELECT 'SUBQ-ARM count=0' AS q, k, p FROM oj_t ORDER BY k;
DROP TABLE oj_t;

DROP TABLE oj_subl;
DROP TABLE oj_subr;

-- The displayed value of an aggregate is the one plain SQL computes: the
-- null-padded rows kept for the worlds without a match are not counted
-- where the row does have a match.
CREATE TABLE oj_da(id int, v int);
CREATE TABLE oj_db(id int, w int);
INSERT INTO oj_da VALUES (1,10),(2,20),(3,30);
INSERT INTO oj_db VALUES (1,5),(1,7),(2,8);
CREATE TABLE oj_da_plain AS SELECT * FROM oj_da;
CREATE TABLE oj_db_plain AS SELECT * FROM oj_db;
SELECT add_provenance('oj_da');
SELECT add_provenance('oj_db');
CREATE TABLE oj_t AS
  SELECT oj_da.id, count(*) AS c, count(w) AS cw, sum(w) AS s
  FROM oj_da LEFT JOIN oj_db ON oj_da.id = oj_db.id GROUP BY oj_da.id;
SELECT remove_provenance('oj_t');
SELECT 'DISPLAY left' AS q, id, c, cw, s FROM oj_t ORDER BY id;
DROP TABLE oj_t;
SELECT 'PLAIN left' AS q, oj_da_plain.id, count(*) AS c, count(w) AS cw,
       sum(w) AS s
FROM oj_da_plain LEFT JOIN oj_db_plain ON oj_da_plain.id = oj_db_plain.id
GROUP BY oj_da_plain.id ORDER BY 2;
CREATE TABLE oj_t AS
  SELECT count(*) AS c
  FROM oj_da FULL JOIN oj_db ON oj_da.id = oj_db.id
  WHERE oj_da.id IS NULL OR oj_db.id IS NULL;
SELECT remove_provenance('oj_t');
SELECT 'DISPLAY full' AS q, c FROM oj_t;
DROP TABLE oj_t;
-- The rows the padding keeps are false in the database as it is.
CREATE TABLE oj_t AS
  SELECT oj_da.id, w, sr_boolean(provenance()) AS holds
  FROM oj_da LEFT JOIN oj_db ON oj_da.id = oj_db.id;
SELECT remove_provenance('oj_t');
SELECT 'HOLDS left' AS q, id, w, holds FROM oj_t ORDER BY id, w;
DROP TABLE oj_t;
-- An aggregate of the null-padded side, read through COALESCE and in
-- arithmetic above the lowered join.
CREATE TABLE oj_t AS
  SELECT oj_da.id, COALESCE(t.c, 0) AS c, t.c + 1 AS c1,
         sr_boolean(provenance()) AS holds
  FROM oj_da LEFT JOIN (SELECT id, count(*) AS c FROM oj_db GROUP BY id) t
       ON oj_da.id = t.id;
SELECT remove_provenance('oj_t');
SELECT 'AGG left' AS q, id, c, c1 FROM oj_t WHERE holds ORDER BY id;
DROP TABLE oj_t;
-- A group absent from the database as it is (its padded row has a match)
-- keeps its aggregate's circuit, with a NULL value: sum is 1500 in the
-- worlds where the match is absent, so its expectation at p = 0.5 is 750.
CREATE TABLE oj_l4(id int, v float8);
INSERT INTO oj_l4 VALUES (1,1100),(1,400);
CREATE TABLE oj_r4(id int NOT NULL UNIQUE);
INSERT INTO oj_r4 VALUES (1);
SELECT add_provenance('oj_l4');
SELECT add_provenance('oj_r4');
SELECT set_prob(provsql, 0.5) FROM oj_r4 \g /dev/null
CREATE TABLE oj_t AS
  SELECT r.id AS rid, l.id AS lid, expected(sum(l.v)) AS e,
         sum(l.v) IS NULL AS no_token
  FROM oj_l4 l LEFT JOIN oj_r4 r ON r.id = l.id GROUP BY r.id, l.id;
SELECT remove_provenance('oj_t');
SELECT 'PADDED GROUP' AS q, rid, lid, e, no_token FROM oj_t ORDER BY rid;
DROP TABLE oj_t, oj_l4, oj_r4;
-- A correlated EXISTS in WHERE above the lowered join reads the joined
-- columns through the subquery that replaces them.
CREATE TABLE oj_t AS
  SELECT oj_da.id, w, sr_boolean(provenance()) AS holds
  FROM oj_da LEFT JOIN oj_db ON oj_da.id = oj_db.id
  WHERE EXISTS (SELECT 1 FROM (VALUES (1), (3)) v(x) WHERE v.x = oj_da.id);
SELECT remove_provenance('oj_t');
SELECT 'EXISTS left' AS q, id, w FROM oj_t WHERE holds ORDER BY id, w;
DROP TABLE oj_t;
DROP TABLE oj_da, oj_db, oj_da_plain, oj_db_plain;
