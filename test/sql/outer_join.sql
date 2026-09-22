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

-- A json column on the preserved side: json has no equality, the lowering
-- matches it on its text.
CREATE TABLE oj_j(id int, data jsonb);
INSERT INTO oj_j VALUES (1, '{"c": [2, 3]}'), (2, '{"c": [4]}'), (3, '{"c": []}');
SELECT add_provenance('oj_j');
CREATE TABLE oj_t AS
  SELECT x1.id, x1.child::text AS child, x2.id AS id2,
         present(provenance()) AS present
  FROM (SELECT *, json_array_elements((data->>'c')::json) child FROM oj_j) x1
  LEFT JOIN oj_j x2 ON x1.child::text::int = x2.id;
SELECT remove_provenance('oj_t');
SELECT * FROM oj_t ORDER BY id, child, id2;
DROP TABLE oj_t;
DROP TABLE oj_j;

-- The preserved side reads a CTE kept as a CTE (untracked): its copies in the
-- arms of the lowering still find it.
CREATE TABLE oj_c(l int, d int);
INSERT INTO oj_c VALUES (1, 1), (1, 2);
SELECT add_provenance('oj_c');
CREATE TABLE oj_t AS
  WITH c AS (SELECT generate_series(1, 3) AS x)
  SELECT t.l, t.x, m2.d, present(provenance()) AS present
  FROM (SELECT m.l, c.x FROM c JOIN oj_c m ON c.x = m.d) t
  LEFT JOIN oj_c m2 ON m2.l = t.l AND m2.d = t.x + 1;
SELECT remove_provenance('oj_t');
SELECT * FROM oj_t ORDER BY x, d;
DROP TABLE oj_t;
DROP TABLE oj_c;

-- A FROM function with several columns (a record from a jsonb value): it adds
-- no provenance; each row has that of the row it is computed from.
CREATE TABLE oj_rec(id int, data jsonb);
INSERT INTO oj_rec VALUES (1, '{"a": 1, "b": "x"}'), (2, '{"a": 2, "b": "y"}');
SELECT add_provenance('oj_rec');
CREATE TABLE oj_t AS
  SELECT j.id, r.a, r.b,
         provenance() = (SELECT provenance() FROM oj_rec k WHERE k.id = j.id)
           AS own
  FROM oj_rec j, LATERAL jsonb_to_record(j.data) AS r(a int, b text);
SELECT remove_provenance('oj_t');
SELECT * FROM oj_t ORDER BY id;
DROP TABLE oj_t;
DROP TABLE oj_rec;

-- Chains of outer joins, and an outer join beside other FROM items: each
-- outer join is lowered in a subquery of its own.  present(provenance())
-- gives the rows of the plain result; the probabilities of the candidate
-- rows are those of a count over the worlds.
CREATE TABLE ojc_a(id int, u int);
CREATE TABLE ojc_b(aid int, t int);
CREATE TABLE ojc_c(aid int, w int);
INSERT INTO ojc_a VALUES (1, 1), (2, 2), (3, 1);
INSERT INTO ojc_b VALUES (1, 10), (2, 20), (2, 21);
INSERT INTO ojc_c VALUES (1, 100), (3, 300);
SELECT add_provenance('ojc_a');
SELECT add_provenance('ojc_b');
SELECT add_provenance('ojc_c');
DO $$ BEGIN
  PERFORM set_prob(provenance(), 0.5) FROM ojc_b;
  PERFORM set_prob(provenance(), 0.5) FROM ojc_c;
END $$;
CREATE TABLE ojc_r AS
  SELECT 'chain' AS q, a.id, b.t, c.w,
         round(probability_evaluate(provenance())::numeric, 4) AS p,
         present(provenance()) AS present
  FROM ojc_a a LEFT JOIN ojc_b b ON b.aid = a.id LEFT JOIN ojc_c c ON c.aid = a.id
  UNION ALL
  SELECT 'through the padded side', a.id, b.t, c.w,
         round(probability_evaluate(provenance())::numeric, 4),
         present(provenance())
  FROM ojc_a a LEFT JOIN ojc_b b ON b.aid = a.id AND b.t < 21
               LEFT JOIN ojc_c c ON c.aid = b.aid
  UNION ALL
  SELECT 'beside a FROM item', a.id, b.t, x.w,
         round(probability_evaluate(provenance())::numeric, 4),
         present(provenance())
  FROM ojc_a a LEFT JOIN ojc_b b ON b.aid = a.id, ojc_c x WHERE x.aid = a.u;
SELECT remove_provenance('ojc_r');
SELECT * FROM ojc_r ORDER BY q, id, t NULLS FIRST, w NULLS FIRST;
DROP TABLE ojc_r;
-- a.* over a chain: the provsql column of the relation is dropped
CREATE TABLE ojc_r AS
  SELECT a.*, b.t, c.w FROM ojc_a a LEFT JOIN ojc_b b ON b.aid = a.id
                                    LEFT JOIN ojc_c c ON c.aid = a.id;
SELECT remove_provenance('ojc_r');
SELECT count(*) AS n_rows FROM ojc_r;
DROP TABLE ojc_r;
-- A join as an arm of the last outer join of a chain, and joins as both
-- arms: each moves into a subquery in turn.
CREATE TABLE ojc_r AS
  SELECT 'join on the padded side' AS q, a.id, b.t, c.w,
         round(probability_evaluate(provenance())::numeric, 4) AS p,
         present(provenance()) AS present
  FROM ojc_a a LEFT JOIN ojc_b b ON b.aid = a.id
               LEFT JOIN (ojc_c c JOIN ojc_b b2 ON b2.aid = c.aid) ON c.aid = a.id
  UNION ALL
  SELECT 'joins on both sides', a.id, b.t, c.w,
         round(probability_evaluate(provenance())::numeric, 4),
         present(provenance())
  FROM (ojc_a a JOIN ojc_c x ON x.aid = a.u)
       LEFT JOIN (ojc_b b JOIN ojc_c c ON c.aid = b.aid - 1) ON b.aid = a.id + 1;
SELECT remove_provenance('ojc_r');
SELECT * FROM ojc_r ORDER BY q, id, t NULLS FIRST, w NULLS FIRST, p;
DROP TABLE ojc_r;
DROP TABLE ojc_a, ojc_b, ojc_c;

-- An outer join beside a LATERAL item.  The lowering moves the join into a
-- subquery of its own, which a LATERAL reading one of its rows cannot follow:
-- that one is refused, and only that one.  A LATERAL over constants, or over
-- another item of the same FROM, stays where it is and the join is lowered as
-- usual.
CREATE TABLE ojl_l(id int, m text);
CREATE TABLE ojl_r(id int, m text);
CREATE TABLE ojl_o(k int);
INSERT INTO ojl_l VALUES (1, 'a'), (2, 'b');
INSERT INTO ojl_r VALUES (1, 'a');
INSERT INTO ojl_o VALUES (7);
SELECT add_provenance('ojl_l');
SELECT add_provenance('ojl_r');
SELECT add_provenance('ojl_o');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ojl_l;
        PERFORM set_prob(provenance(), 0.5) FROM ojl_r;
        PERFORM set_prob(provenance(), 0.5) FROM ojl_o; END $$;

-- A LATERAL over constants: the rows of the left join, each with its own
-- probability (the matched row needs both, a padded one the left row without
-- the right, and the row that matches nothing only itself).
CREATE TABLE ojl_res AS
  SELECT 'lateral over constants' AS q, l.id, r.id AS rid, x.k,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ojl_l l LEFT JOIN ojl_r r ON r.m = l.m, LATERAL (SELECT 1 AS k) x
  UNION ALL
  -- A LATERAL reading another item of the same FROM: every row also needs
  -- that item, so each probability is halved.
  SELECT 'lateral over a sibling', l.id, r.id, y.k,
         round(probability_evaluate(provenance())::numeric, 4)
  FROM ojl_l l LEFT JOIN ojl_r r ON r.m = l.m, ojl_o o,
       LATERAL (SELECT o.k + 1 AS k) y;
SELECT remove_provenance('ojl_res');
SELECT * FROM ojl_res ORDER BY q, id, rid NULLS FIRST;
DROP TABLE ojl_res;

-- A LATERAL reading a row of the join itself, as a subquery and as a
-- function: refused, rather than lowered into something that cannot read it.
SELECT l.id, x.k
  FROM ojl_l l LEFT JOIN ojl_r r ON r.m = l.m,
       LATERAL (SELECT l.id + 1 AS k) x;
SELECT l.id, z.k
  FROM ojl_l l LEFT JOIN ojl_r r ON r.m = l.m,
       LATERAL unnest(ARRAY[l.id]) z(k);

-- A whole-row value, or a system column, of a relation of an outer join: the
-- lowering puts that relation in a subquery, which has neither, so it is
-- refused rather than lowered into a tree that cannot be read.  Each of these
-- used to come out as something other than a refusal -- "attribute 24 of
-- relation (null) does not exist", "type tid is not composite", "ROW() column
-- has type integer instead of type text" -- and the first one segfaulted the
-- planner, writing past the attr_needed array of the wrong relation.
SELECT l.id FROM ojl_l l LEFT JOIN ojl_r r ON r.m = l.m
  LEFT JOIN ojl_o o ON o.k = r.id GROUP BY l.id
HAVING count(DISTINCT r.ctid) = count(DISTINCT CASE WHEN o.k IS NOT NULL THEN r.ctid END);
SELECT l.id, count(DISTINCT r.ctid) FROM ojl_l l LEFT JOIN ojl_r r ON r.m = l.m
  GROUP BY l.id;
SELECT l.id, count(CASE WHEN r.m = 'a' THEN r END) FROM ojl_l l
  LEFT JOIN ojl_r r ON r.m = l.m GROUP BY l.id;
-- A whole row the rewriting reads as an anonymous record is replaced before
-- the lowering, so it is not one of these and still answers.
CREATE TABLE ojl_res AS
  SELECT l.id, count(DISTINCT r) AS n FROM ojl_l l LEFT JOIN ojl_r r ON r.m = l.m
  GROUP BY l.id;
SELECT remove_provenance('ojl_res');
SELECT * FROM ojl_res ORDER BY id;
DROP TABLE ojl_res;
-- A system column without an outer join is not relocated, so it is read.
SELECT count(r.ctid) FROM ojl_r r;
SELECT count(r.ctid) FROM ojl_r r JOIN ojl_l l ON r.m = l.m;

DROP TABLE ojl_l, ojl_r, ojl_o;
