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
SELECT 'PADDED GROUP' AS q, rid, lid, e, no_token FROM oj_t ORDER BY rid, no_token;
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
-- An aggregate over a whole row, with a sublink: the aggregate is split from
-- the sublink, which sends the whole row to the inner query, where the join
-- the sublink becomes is rewritten.  The record of the columns replaces it
-- before the split, so these answer -- and the token is not in the record.
-- Only (1, 'a') passes the EXISTS, and its row needs ojl_r's row too: 1/4.
CREATE TABLE ojl_res AS
  SELECT l.m, json_agg(l) AS j, count(l) AS n,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ojl_l l WHERE EXISTS (SELECT 1 FROM ojl_r r WHERE r.m = l.m) GROUP BY l.m;
SELECT remove_provenance('ojl_res');
SELECT * FROM ojl_res ORDER BY m;
DROP TABLE ojl_res;
-- The same over a derived table, whose row type is not a relation's.
CREATE TABLE ojl_res AS
  SELECT json_agg(base) AS j FROM (SELECT id, m FROM ojl_l) base
  WHERE EXISTS (SELECT 1 FROM ojl_r r WHERE r.m = base.m);
SELECT remove_provenance('ojl_res');
SELECT * FROM ojl_res;
DROP TABLE ojl_res;

DROP TABLE ojl_l, ojl_r, ojl_o;

-- A chain of outer joins is lowered one join at a time, each reading the
-- joins before it three times: from a CTE, rewritten once, as the joins
-- before a copy would otherwise be lowered again in each copy, and the query
-- grow as three to the power of their number.  Twelve, as a generated query
-- writes them (difftest's BEAVER), exhausted the server's memory.
CREATE TABLE oj_chain(id int, p int);
INSERT INTO oj_chain VALUES (1, 1), (2, 1);
SELECT add_provenance('oj_chain');
CREATE TABLE oj_chain_r AS SELECT t0.id FROM oj_chain t0
  LEFT JOIN oj_chain t1 ON t1.p = t0.id + 9 LEFT JOIN oj_chain t2 ON t2.p = t0.id + 9
  LEFT JOIN oj_chain t3 ON t3.p = t0.id + 9 LEFT JOIN oj_chain t4 ON t4.p = t0.id + 9
  LEFT JOIN oj_chain t5 ON t5.p = t0.id + 9 LEFT JOIN oj_chain t6 ON t6.p = t0.id + 9
  LEFT JOIN oj_chain t7 ON t7.p = t0.id + 9 LEFT JOIN oj_chain t8 ON t8.p = t0.id + 9
  LEFT JOIN oj_chain t9 ON t9.p = t0.id + 9 LEFT JOIN oj_chain t10 ON t10.p = t0.id + 9
  LEFT JOIN oj_chain t11 ON t11.p = t0.id + 9 LEFT JOIN oj_chain t12 ON t12.p = t0.id + 9;
SELECT remove_provenance('oj_chain_r');
SELECT id FROM oj_chain_r ORDER BY id;
DROP TABLE oj_chain_r;
SELECT remove_provenance('oj_chain');
DROP TABLE oj_chain;

-- The probabilities through a chain, and through outer joins nested on the
-- right, whose right input is shared the same way.  Rows at one half: the
-- probabilities are those of the enumeration of the 32 worlds.
CREATE TABLE oj_c(id int, k int, x int);
INSERT INTO oj_c VALUES (1, 0, 5), (2, 0, 6), (3, 1, 7), (4, 1, 8), (5, 2, NULL);
SELECT add_provenance('oj_c');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM oj_c; END $$;
-- 1-3-3-N: 1/4; 1-4-3-N, 1-4-N-N, 1-N-N-N, and each of the four rows of 2:
-- 1/8; 1-3-N-N and 1-N-3-N hold in no world.
CREATE TABLE oj_c_r AS
  SELECT r.id::text || '-' || coalesce(s.id::text, 'N') || '-' ||
         coalesce(t.id::text, 'N') || '-' || coalesce(u.id::text, 'N') AS o,
         probability_evaluate(provenance()) AS p
  FROM oj_c r LEFT JOIN oj_c s ON s.k = r.id LEFT JOIN oj_c t ON t.x = r.x + 2
       LEFT JOIN oj_c u ON u.k = s.id
  WHERE r.k = 0;
SELECT remove_provenance('oj_c_r');
SELECT o, round(sum(p)::numeric, 6) AS p FROM oj_c_r GROUP BY o ORDER BY o;
DROP TABLE oj_c_r;
-- 1-3-N, 1-4-N, 2-5-N, 2-N-N: 1/4; 1-N-N: 1/8.
CREATE TABLE oj_c_r AS
  SELECT r.id::text || '-' || coalesce(s.id::text, 'N') || '-' ||
         coalesce(t.id::text, 'N') AS o,
         probability_evaluate(provenance()) AS p
  FROM oj_c r LEFT JOIN (oj_c s LEFT JOIN oj_c t ON t.k = s.id) ON s.k = r.id
  WHERE r.k = 0;
SELECT remove_provenance('oj_c_r');
SELECT o, round(sum(p)::numeric, 6) AS p FROM oj_c_r GROUP BY o ORDER BY o;
DROP TABLE oj_c_r;
-- COUNT(DISTINCT) over a chain: its rewriting copies the query holding the
-- shared CTE, and the copy is a CTE of its own (difftest's BEAVER dw/082: "could
-- not find pathkey item to sort", and a wrong probability once that was
-- avoided).  Group 0: 1/8 for two distinct x; 3/4, 3/4, 1/2 for no t.
CREATE TABLE oj_c_r AS
  SELECT r.k, probability_evaluate(provenance()) AS p
  FROM oj_c r LEFT JOIN oj_c s ON s.k = r.id LEFT JOIN oj_c t ON t.k = s.id
  GROUP BY r.k HAVING count(DISTINCT s.x) >= 2;
SELECT remove_provenance('oj_c_r');
SELECT k, round(p::numeric, 6) AS p FROM oj_c_r ORDER BY k;
DROP TABLE oj_c_r;
CREATE TABLE oj_c_r AS
  SELECT r.k, probability_evaluate(provenance()) AS p
  FROM oj_c r LEFT JOIN oj_c s ON s.k = r.id LEFT JOIN oj_c t ON t.k = s.id
  GROUP BY r.k HAVING count(DISTINCT t.id) = 0;
SELECT remove_provenance('oj_c_r');
SELECT k, round(p::numeric, 6) AS p FROM oj_c_r ORDER BY k;
DROP TABLE oj_c_r;
SELECT remove_provenance('oj_c');
DROP TABLE oj_c;

-- The preserved side reads no tracked relation -- a VALUES list, a series, a
-- WITH query over neither -- and is the same in every world: provenance one
-- for each of its rows.  Two tracked rows, k = 1 and 2, each at 1/2.  The ids
-- of the list no row matches: 1 and 2 at 1/2, 4 always.  The rows matched up
-- to each point of a series, on average: 1/2, then 1, then 1.
CREATE TABLE oj_c(k int);
INSERT INTO oj_c VALUES (1), (2);
SELECT add_provenance('oj_c');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM oj_c; END $$;
CREATE TABLE oj_r AS
  SELECT t.v, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM (VALUES (1), (2), (4)) t(v) LEFT JOIN oj_c c ON c.k = t.v
  WHERE c.k IS NULL;
SELECT remove_provenance('oj_r');
SELECT * FROM oj_r ORDER BY v;
DROP TABLE oj_r;
CREATE TABLE oj_r AS
  WITH w(v) AS (VALUES (1), (2), (4))
  SELECT w.v, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM w LEFT JOIN oj_c c ON c.k = w.v WHERE c.k IS NULL;
SELECT remove_provenance('oj_r');
SELECT * FROM oj_r ORDER BY v;
DROP TABLE oj_r;
CREATE TABLE oj_r AS
  SELECT g, round(expected(count(c.k))::numeric, 6) AS e
  FROM generate_series(1, 3) g LEFT JOIN oj_c c ON c.k <= g GROUP BY g;
SELECT remove_provenance('oj_r');
SELECT * FROM oj_r ORDER BY g;
DROP TABLE oj_r;
SELECT remove_provenance('oj_c');
DROP TABLE oj_c;

-- A LEFT JOIN LATERAL whose body only selects and joins, correlated by its
-- WHERE, is the outer join on that correlation; one whose body aggregates
-- without grouping, joined ON TRUE, has exactly one row, each column the
-- scalar subquery giving it.  oj_l certain, both rows of oj_m at 1/2: key 1
-- meets 10 and 20 at 1/2 each, no row at 1/4; key 2 never meets one.  The
-- keys matched by no row: 1 at 1/4, 2 always.
CREATE TABLE oj_l(k int);
CREATE TABLE oj_m(k int, v int);
INSERT INTO oj_l VALUES (1), (2);
INSERT INTO oj_m VALUES (1, 10), (1, 20);
SELECT add_provenance('oj_l');
SELECT add_provenance('oj_m');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM oj_m; END $$;
CREATE TABLE oj_r AS
  SELECT l.k, m.v, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM oj_l l LEFT JOIN LATERAL (SELECT * FROM oj_m m WHERE m.k = l.k) m
       ON true;
SELECT remove_provenance('oj_r');
SELECT * FROM oj_r ORDER BY k, v;
DROP TABLE oj_r;
CREATE TABLE oj_r AS
  SELECT l.k, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM oj_l l
  LEFT JOIN LATERAL (SELECT count(*) AS c FROM oj_m m WHERE m.k = l.k) a
       ON true
  WHERE a.c = 0;
SELECT remove_provenance('oj_r');
SELECT * FROM oj_r ORDER BY k;
DROP TABLE oj_r;
-- A body ORDER BY ... LIMIT 1 has at most one row, the first in that order:
-- each column the scalar subquery giving it.  The greatest v of key 1 is 20
-- where the row of 20 is there, 1/2; key 2 has none.
CREATE TABLE oj_r AS
  SELECT l.k, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM oj_l l
  LEFT JOIN LATERAL (SELECT m.v FROM oj_m m WHERE m.k = l.k
                     ORDER BY m.v DESC LIMIT 1) t ON true
  WHERE t.v > 15;
SELECT remove_provenance('oj_r');
SELECT * FROM oj_r WHERE p > 0 ORDER BY k;
DROP TABLE oj_r;
-- Not in a query that groups by the lateral column: read there, the scalar
-- subquery it is would be a grouping key, and the join stays a refusal.
SELECT l.k, a.c, count(*)
FROM oj_l l
LEFT JOIN LATERAL (SELECT count(*) AS c FROM oj_m m WHERE m.k = l.k) a
     ON true
GROUP BY l.k, a.c;
SELECT remove_provenance('oj_l');
SELECT remove_provenance('oj_m');
DROP TABLE oj_l, oj_m;

-- A computation over an aggregate read as plain SQL (under its warning) is
-- evaluated only on the groups of the data as it is.  Every row of oj_la is
-- matched there, so the groups of the unmatched rows are those of other
-- worlds only, where on the data as it is the count is 0: its logarithm is
-- NULL there, where it raised -- SQL, which has no such group, answers.
CREATE TABLE oj_la(id int, g int);
CREATE TABLE oj_lb(id int);
INSERT INTO oj_la VALUES (1, 1), (2, 2);
INSERT INTO oj_lb VALUES (1), (2);
SELECT add_provenance('oj_la');
SELECT add_provenance('oj_lb');
SET client_min_messages = error;
CREATE TABLE oj_r AS
  SELECT a.g, log(10, count(*)) AS l
  FROM oj_la a LEFT JOIN oj_lb b ON b.id = a.id
  WHERE b.id IS NULL GROUP BY a.g;
RESET client_min_messages;
SELECT remove_provenance('oj_r');
SELECT g, l FROM oj_r ORDER BY g;
DROP TABLE oj_r;
-- Not an aggregate without grouping: its one row is there in every world,
-- over no row too, where the count is 0 -- '0 rows', as in SQL.
SET client_min_messages = error;
CREATE TABLE oj_r AS
  SELECT count(*) || ' rows' AS v FROM oj_la WHERE g > 5;
RESET client_min_messages;
SELECT remove_provenance('oj_r');
SELECT v FROM oj_r;
DROP TABLE oj_r;
SELECT remove_provenance('oj_la');
SELECT remove_provenance('oj_lb');
DROP TABLE oj_la, oj_lb;
-- The months padded by generate_series over the minimum and maximum of a
-- grouped subquery: several rows per group, so the unmatched arm compares
-- them. Jan and Apr are there with probability 1/2 and 3/4, Mar always
-- padded with 0.
CREATE TABLE oj_sales(d date, amount int);
INSERT INTO oj_sales VALUES ('2020-01-10', 100), ('2020-02-10', 240),
  ('2020-04-05', 200), ('2020-04-20', 230);
SELECT add_provenance('oj_sales');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM oj_sales; END $$;
SET client_min_messages = error;
CREATE TABLE oj_r AS
  WITH w AS (SELECT month, sum(amount) AS total
             FROM (SELECT date_trunc('month', d) AS month, amount
                   FROM oj_sales) z GROUP BY month)
  SELECT to_char(month, 'fmMon') AS month, coalesce(total, 0) AS total,
         probability_evaluate(provenance()) AS p
  FROM (SELECT generate_series(min(month), max(month), '1 month'::interval)
               AS month FROM w) m
  LEFT JOIN w USING (month);
RESET client_min_messages;
SELECT remove_provenance('oj_r');
SET client_min_messages = error;
SELECT month, total::text, round(p::numeric, 4) AS p
FROM oj_r ORDER BY month, total::text;
RESET client_min_messages;
DROP TABLE oj_r;
-- The latest month only, an aggregate result: the unmatched arm compares no
-- column and counts every group of w against it, so it is there only when
-- there is no sale at all (1/16). The rewritten query is deparsed at verbose
-- level 20, written to the log only.
SET client_min_messages = error;
SET log_min_messages = notice;
SET provsql.verbose_level = 20;
CREATE TABLE oj_r AS
  WITH w AS (SELECT date_trunc('month', d) AS month, sum(amount) AS total
             FROM oj_sales GROUP BY 1)
  SELECT coalesce(total, 0) AS total, probability_evaluate(provenance()) AS p
  FROM (SELECT max(month) AS month FROM w) m LEFT JOIN w USING (month);
RESET provsql.verbose_level;
RESET log_min_messages;
SELECT remove_provenance('oj_r');
SELECT total::text, round(p::numeric, 4) AS p FROM oj_r ORDER BY p, total::text;
RESET client_min_messages;
DROP TABLE oj_r;
SELECT remove_provenance('oj_sales');
DROP TABLE oj_sales;

-- A sublink in the ON condition of an outer join (NOT IN, EXISTS, NOT
-- EXISTS; LEFT, RIGHT, FULL; reading one side or both): the condition is
-- copied into the subqueries the join is lowered to, with its sublinks.  The rows present in the database as
-- it is are those of SQL.
CREATE TABLE oj_q(id int);
CREATE TABLE oj_a(parentid int);
CREATE TABLE oj_h(postid int);
INSERT INTO oj_q VALUES (1), (2);
INSERT INTO oj_a VALUES (1), (2);
INSERT INTO oj_h VALUES (2);
SELECT add_provenance('oj_q');
SELECT add_provenance('oj_a');
SELECT add_provenance('oj_h');
CREATE TABLE oj_r AS
  SELECT 'not in' AS c, q.id, a.parentid, present(provenance()) AS present
  FROM oj_q q LEFT JOIN oj_a a
    ON q.id = a.parentid AND NOT q.id IN (SELECT postid FROM oj_h)
  UNION ALL
  SELECT 'exists', q.id, a.parentid, present(provenance())
  FROM oj_q q LEFT JOIN oj_a a
    ON q.id = a.parentid AND EXISTS (SELECT 1 FROM oj_h h WHERE h.postid = q.id)
  UNION ALL
  SELECT 'right in', q.id, a.parentid, present(provenance())
  FROM oj_q q RIGHT JOIN oj_a a
    ON q.id = a.parentid AND q.id IN (SELECT postid FROM oj_h)
  UNION ALL
  SELECT 'full not in', q.id, a.parentid, present(provenance())
  FROM oj_q q FULL JOIN oj_a a
    ON q.id = a.parentid AND NOT q.id IN (SELECT postid FROM oj_h)
  UNION ALL
  SELECT 'both sides', q.id, a.parentid, present(provenance())
  FROM oj_q q LEFT JOIN oj_a a
    ON q.id = a.parentid
   AND NOT EXISTS (SELECT 1 FROM oj_h h
                   WHERE h.postid = q.id AND h.postid = a.parentid);
SELECT remove_provenance('oj_r');
SELECT c, id, parentid FROM oj_r WHERE present ORDER BY c, id, parentid;
DROP TABLE oj_r;
SELECT remove_provenance('oj_q');
SELECT remove_provenance('oj_a');
SELECT remove_provenance('oj_h');
DROP TABLE oj_q, oj_a, oj_h;
