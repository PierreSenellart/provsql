\set ECHO none
\pset format unaligned

-- Set operations nested in set operations, and ORDER BY / LIMIT / OFFSET on a
-- non-ALL set operation.
--
-- The rewriting takes one set operation per query level: a tree of UNION ALL
-- (⊎), or one EXCEPT over two leaves, a non-ALL top node being wrapped in a
-- GROUP BY (⊕).  A subtree that does not fit -- a UNION under a UNION ALL, an
-- EXCEPT under a UNION, anything under an EXCEPT -- is nested as a query of
-- its own, as a user would by moving it to a FROM subquery.
--
--   r = {1, 1, 2, 5, 7}   s = {1, 3, 9, NULL}   w = {2, 9}
-- Probabilities: r 0.5, s 0.4, w 0.2.  The counting semiring gives the number
-- of derivations; every row is checked against what SQL returns.

CREATE TABLE sn_r(a int); INSERT INTO sn_r VALUES (1),(1),(2),(5),(7);
CREATE TABLE sn_s(a int); INSERT INTO sn_s VALUES (1),(3),(9),(NULL);
CREATE TABLE sn_w(a int); INSERT INTO sn_w VALUES (2),(9);
SELECT add_provenance('sn_r'); SELECT add_provenance('sn_s'); SELECT add_provenance('sn_w');
DO $$ BEGIN
  PERFORM set_prob(provenance(), 0.5) FROM sn_r;
  PERFORM set_prob(provenance(), 0.4) FROM sn_s;
  PERFORM set_prob(provenance(), 0.2) FROM sn_w;
END $$;
CREATE TABLE sn_one AS
  SELECT 1 AS value, provsql AS provenance FROM sn_r
  UNION ALL SELECT 1, provsql FROM sn_s UNION ALL SELECT 1, provsql FROM sn_w;
SELECT remove_provenance('sn_one');

CREATE FUNCTION sn_c(u uuid) RETURNS int LANGUAGE sql AS
  $$ SELECT provsql.sr_counting(u, 'sn_one') $$;
CREATE FUNCTION sn_p(u uuid) RETURNS numeric LANGUAGE sql AS
  $$ SELECT round(provsql.probability_evaluate(u)::numeric, 4) $$;

-- ---------------------------------------------------------------------------
-- 1. UNION under UNION ALL: the inner UNION keeps its deduplication
-- ---------------------------------------------------------------------------

-- (r UNION s) UNION ALL w.  SQL: {1,2,3,5,7,9,NULL} then {2,9}: 9 rows.
-- a=1 has 3 derivations (two r rows, one s row), P = 1-0.5*0.5*0.6 = 0.85.
CREATE TABLE sn_t1 AS SELECT a, sn_c(provenance()) AS c, sn_p(provenance()) AS p
  FROM ((SELECT a FROM sn_r UNION SELECT a FROM sn_s) UNION ALL SELECT a FROM sn_w) t;
SELECT remove_provenance('sn_t1');
SELECT 'left-nested' AS q, a, c, p FROM sn_t1 ORDER BY a, c;

-- Right-nested: w UNION ALL (r UNION s).  Same rows.
CREATE TABLE sn_t2 AS SELECT a, sn_c(provenance()) AS c, sn_p(provenance()) AS p
  FROM (SELECT a FROM sn_w UNION ALL (SELECT a FROM sn_r UNION SELECT a FROM sn_s)) t;
SELECT remove_provenance('sn_t2');
SELECT 'right-nested' AS q, a, c, p FROM sn_t2 ORDER BY a, c;

-- Two levels: ((r UNION s) UNION ALL w) UNION ALL w: 11 rows.
CREATE TABLE sn_t3 AS SELECT count(*) AS n FROM
  (((SELECT a FROM sn_r UNION SELECT a FROM sn_s) UNION ALL SELECT a FROM sn_w)
   UNION ALL SELECT a FROM sn_w) t;
SELECT remove_provenance('sn_t3'); SELECT 'two levels' AS q, n FROM sn_t3;

-- As the whole statement, not only as a FROM subquery: 9 rows.
CREATE TABLE sn_t4 AS
  (SELECT a FROM sn_r UNION SELECT a FROM sn_s) UNION ALL SELECT a FROM sn_w;
SELECT remove_provenance('sn_t4');
SELECT 'top level' AS q, count(*) AS n, count(DISTINCT a) AS nd FROM sn_t4;

-- Under a non-ALL top node no nesting is needed: one ⊕ over all leaves.
-- (r UNION ALL s) UNION r: a=1 has 2+1+2 = 5 derivations.
CREATE TABLE sn_t5 AS SELECT a, sn_c(provenance()) AS c
  FROM ((SELECT a FROM sn_r UNION ALL SELECT a FROM sn_s) UNION SELECT a FROM sn_r) t;
SELECT remove_provenance('sn_t5');
SELECT 'ALL under non-ALL' AS q, a, c FROM sn_t5 ORDER BY a;
-- (r UNION s) UNION w: a=2 has 2 derivations, a=9 has 2.
CREATE TABLE sn_t6 AS SELECT a, sn_c(provenance()) AS c
  FROM ((SELECT a FROM sn_r UNION SELECT a FROM sn_s) UNION SELECT a FROM sn_w) t;
SELECT remove_provenance('sn_t6');
SELECT 'non-ALL under non-ALL' AS q, a, c FROM sn_t6 ORDER BY a;

-- ---------------------------------------------------------------------------
-- 2. EXCEPT in and over other set operations
-- ---------------------------------------------------------------------------

-- (r EXCEPT s) UNION ALL w.  a=1: (r1 or r1') and not s1 = 0.75*0.6 = 0.45;
-- a=2: 0.5 from r, and 0.2 from w as a separate row.
CREATE TABLE sn_t7 AS SELECT a, sn_p(provenance()) AS p
  FROM ((SELECT a FROM sn_r EXCEPT SELECT a FROM sn_s) UNION ALL SELECT a FROM sn_w) t;
SELECT remove_provenance('sn_t7');
SELECT 'EXCEPT under UNION ALL' AS q, a, p FROM sn_t7 ORDER BY a, p;

-- Chained: r EXCEPT s EXCEPT w = (r EXCEPT s) EXCEPT w.
-- a=1: 0.45   a=2: 0.5*0.8 = 0.4   a=5, 7: 0.5
CREATE TABLE sn_t8 AS SELECT a, sn_p(provenance()) AS p
  FROM (SELECT a FROM sn_r EXCEPT SELECT a FROM sn_s EXCEPT SELECT a FROM sn_w) t;
SELECT remove_provenance('sn_t8');
SELECT 'chained EXCEPT' AS q, a, p FROM sn_t8 ORDER BY a;

-- r EXCEPT (s UNION w): the same annotations.
CREATE TABLE sn_t9 AS SELECT a, sn_p(provenance()) AS p
  FROM (SELECT a FROM sn_r EXCEPT (SELECT a FROM sn_s UNION SELECT a FROM sn_w)) t;
SELECT remove_provenance('sn_t9');
SELECT 'EXCEPT over UNION' AS q, a, p FROM sn_t9 ORDER BY a;

-- r EXCEPT (s UNION ALL w): the same annotations again.
CREATE TABLE sn_t10 AS SELECT a, sn_p(provenance()) AS p
  FROM (SELECT a FROM sn_r EXCEPT (SELECT a FROM sn_s UNION ALL SELECT a FROM sn_w)) t;
SELECT remove_provenance('sn_t10');
SELECT 'EXCEPT over UNION ALL' AS q, a, p FROM sn_t10 ORDER BY a;

-- EXCEPT ALL is refused wherever it is nested.
SELECT a FROM sn_r UNION ALL (SELECT a FROM sn_s EXCEPT ALL SELECT a FROM sn_w);

-- The statement once listed as unsupported: personnel-style t EXCEPT t EXCEPT t.
CREATE TABLE sn_t11 AS SELECT a, sn_p(provenance()) AS p FROM
  (SELECT a FROM sn_w EXCEPT SELECT a FROM sn_w EXCEPT SELECT a FROM sn_w) t;
SELECT remove_provenance('sn_t11');
SELECT 'w EXCEPT w EXCEPT w' AS q, a, p FROM sn_t11 ORDER BY a;

-- INTERSECT stays unsupported, nested or not.
SELECT a FROM sn_r UNION ALL (SELECT a FROM sn_s INTERSECT SELECT a FROM sn_w);

-- ---------------------------------------------------------------------------
-- 3. ORDER BY / LIMIT / OFFSET on a non-ALL set operation apply after the
--    deduplication.  Rows are stored in query order and read back by ctid.
--    Over a set operation, a LIMIT truncates the actual result, which
--    actual() says (otherwise a warning is raised, see limit_warning).
-- ---------------------------------------------------------------------------

-- r UNION s = {1,2,3,5,7,9,NULL}
CREATE TABLE sn_o1 AS SELECT a FROM sn_r UNION SELECT a FROM sn_s ORDER BY a DESC;
SELECT remove_provenance('sn_o1');
SELECT 'ORDER BY a DESC' AS q, string_agg(coalesce(a::text,'NULL'), ' ' ORDER BY ctid) AS rows FROM sn_o1;

CREATE TABLE sn_o2 AS SELECT a FROM sn_r UNION SELECT a FROM sn_s ORDER BY 1 NULLS FIRST;
SELECT remove_provenance('sn_o2');
SELECT 'ORDER BY 1 NULLS FIRST' AS q, string_agg(coalesce(a::text,'NULL'), ' ' ORDER BY ctid) AS rows FROM sn_o2;

-- LIMIT 2: 1, 2 (not the two a=1 rows of r merged into one).
CREATE TABLE sn_o3 AS SELECT a, sn_c(provenance()) AS c
  FROM (SELECT a FROM sn_r UNION SELECT a FROM sn_s ORDER BY a LIMIT 2) t;
SELECT remove_provenance('sn_o3');
SELECT 'LIMIT 2' AS q, a, c FROM sn_o3 ORDER BY a;

-- OFFSET 1 LIMIT 2: 2, 3
CREATE TABLE sn_o4 AS SELECT a FROM sn_r UNION SELECT a FROM sn_s ORDER BY a OFFSET actual(1) LIMIT actual(2);
SELECT remove_provenance('sn_o4');
SELECT 'OFFSET 1 LIMIT 2' AS q, string_agg(a::text, ' ' ORDER BY ctid) AS rows FROM sn_o4;

-- EXCEPT: r EXCEPT w = {1,5,7} (2 is kept with a zero-able token), DESC LIMIT 2: 7, 5
CREATE TABLE sn_o5 AS SELECT a FROM sn_r EXCEPT SELECT a FROM sn_w ORDER BY a DESC LIMIT actual(2);
SELECT remove_provenance('sn_o5');
SELECT 'EXCEPT DESC LIMIT 2' AS q, string_agg(a::text, ' ' ORDER BY ctid) AS rows FROM sn_o5;

-- An arm's own ORDER BY / LIMIT stays on the arm: (max of r) UNION w = {7,2,9}
-- (a truncation of the actual arm, with actual(): the filter of a rank is
-- tested in limit_rank)
CREATE TABLE sn_o6 AS (SELECT a FROM sn_r ORDER BY a DESC LIMIT actual(1))
  UNION SELECT a FROM sn_w ORDER BY a LIMIT actual(2);
SELECT remove_provenance('sn_o6');
SELECT 'arm LIMIT' AS q, string_agg(a::text, ' ' ORDER BY ctid) AS rows FROM sn_o6;

-- The ALL forms were never affected.
CREATE TABLE sn_o7 AS SELECT a FROM sn_r UNION ALL SELECT a FROM sn_s ORDER BY a LIMIT actual(3);
SELECT remove_provenance('sn_o7');
SELECT 'UNION ALL LIMIT 3' AS q, string_agg(a::text, ' ' ORDER BY ctid) AS rows FROM sn_o7;

-- Two columns, ordered by the second one.
CREATE TABLE sn_o8 AS SELECT a, -a AS b FROM sn_r UNION SELECT a, -a FROM sn_w ORDER BY b LIMIT actual(3);
SELECT remove_provenance('sn_o8');
SELECT 'ORDER BY second column' AS q, string_agg(a::text, ' ' ORDER BY ctid) AS rows FROM sn_o8;

DROP FUNCTION sn_c(uuid); DROP FUNCTION sn_p(uuid);
DROP TABLE sn_r, sn_s, sn_w, sn_one, sn_t1, sn_t2, sn_t3, sn_t4, sn_t5, sn_t6,
  sn_t7, sn_t8, sn_t9, sn_t10, sn_t11, sn_o1, sn_o2, sn_o3, sn_o4, sn_o5, sn_o6,
  sn_o7, sn_o8;
