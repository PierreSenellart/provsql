\set ECHO none
\pset format unaligned

-- The joins built by the rewriting for a difference (EXCEPT, the padded arm of
-- an outer join, decorrelated sublinks) and for AGG(DISTINCT) compare columns
-- with NULLs taken as equal.  Written as IS NOT DISTINCT FROM, PostgreSQL can
-- only run them as nested loops, quadratic in their inputs.  They are written
-- "=" when one side cannot be NULL, and ARRAY[l] = ARRAY[r] otherwise, which
-- has the same meaning and can be hashed and merged; IS NOT DISTINCT FROM
-- remains for array-typed columns.
--
-- nsj_plan reports, for a query, whether its plan has a nested loop although
-- nested loops are disabled (that is, no other join method applies), and which
-- of the three forms appear.

CREATE FUNCTION nsj_plan(q text, OUT nested_loop bool, OUT array_eq bool,
                         OUT is_distinct bool) AS $$
DECLARE line text; plan text := '';
BEGIN
  SET LOCAL enable_nestloop = off;
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    plan := plan || line || E'\n';
  END LOOP;
  nested_loop := plan LIKE '%Nested Loop%';
  array_eq    := plan LIKE '%ARRAY[%';
  is_distinct := plan LIKE '%IS DISTINCT FROM%';
END $$ LANGUAGE plpgsql;

CREATE TABLE nsj_l(k int, t text, n numeric, arr int[], nn int NOT NULL);
CREATE TABLE nsj_r(k int, t text, n numeric, arr int[], nn int NOT NULL);
INSERT INTO nsj_l VALUES
  (1, 'a', 1.0, '{1}', 1), (2, 'b', 2.0, '{2}', 2), (NULL, NULL, NULL, NULL, 3),
  (4, 'd', 4.0, '{4,NULL}', 4);
INSERT INTO nsj_r VALUES
  (1, 'a', 1.00, '{1}', 1), (NULL, NULL, NULL, NULL, 3), (4, 'D', 4.5, '{4,NULL}', 5);
SELECT add_provenance('nsj_l');
SELECT add_provenance('nsj_r');
DO $$ BEGIN
  PERFORM set_prob(provenance(), 0.5) FROM nsj_l;
  PERFORM set_prob(provenance(), 0.5) FROM nsj_r;
END $$;
CREATE FUNCTION nsj_p(u uuid) RETURNS numeric LANGUAGE sql AS
  $$ SELECT round(provsql.probability_evaluate(u)::numeric, 4) $$;

-- Nullable int: NULL is removed by NULL, 1 by 1; hash or merge join.
CREATE TABLE nsj_1 AS SELECT k, nsj_p(provenance()) AS p
  FROM (SELECT k FROM nsj_l EXCEPT SELECT k FROM nsj_r) x;
SELECT remove_provenance('nsj_1');
SELECT * FROM nsj_1 ORDER BY k;
SELECT * FROM nsj_plan('SELECT k FROM nsj_l EXCEPT SELECT k FROM nsj_r');

-- text (case-sensitive: d is not removed by D) and numeric (1.0 is removed by
-- 1.00: the type's equality, not the binary image), together.
CREATE TABLE nsj_2 AS SELECT t, n, nsj_p(provenance()) AS p
  FROM (SELECT t, n FROM nsj_l EXCEPT SELECT t, n FROM nsj_r) x;
SELECT remove_provenance('nsj_2');
SELECT * FROM nsj_2 ORDER BY t;
SELECT * FROM nsj_plan('SELECT t, n FROM nsj_l EXCEPT SELECT t, n FROM nsj_r');

-- Array-typed column: IS NOT DISTINCT FROM is kept (ARRAY[] of an array is a
-- two-dimensional array), so the nested loop stays; results as usual, a NULL
-- element inside an array included.
CREATE TABLE nsj_3 AS SELECT arr, nsj_p(provenance()) AS p
  FROM (SELECT arr FROM nsj_l EXCEPT SELECT arr FROM nsj_r) x;
SELECT remove_provenance('nsj_3');
SELECT * FROM nsj_3 ORDER BY arr;
SELECT * FROM nsj_plan('SELECT arr FROM nsj_l EXCEPT SELECT arr FROM nsj_r');

-- Column declared NOT NULL: plain equality.
CREATE TABLE nsj_4 AS SELECT nn, nsj_p(provenance()) AS p
  FROM (SELECT nn FROM nsj_l EXCEPT SELECT nn FROM nsj_r) x;
SELECT remove_provenance('nsj_4');
SELECT * FROM nsj_4 ORDER BY nn;
SELECT * FROM nsj_plan('SELECT nn FROM nsj_l EXCEPT SELECT nn FROM nsj_r');
-- NOT NULL on one side only is enough; an expression is not trusted.
SELECT * FROM nsj_plan('SELECT nn FROM nsj_l EXCEPT SELECT k FROM nsj_r');
SELECT * FROM nsj_plan('SELECT nn + 0 FROM nsj_l EXCEPT SELECT k FROM nsj_r');

-- Outer join: the padded arm compares all the columns of the preserved
-- relation, the all-NULL row included.
CREATE TABLE nsj_5 AS SELECT nsj_l.nn, nsj_r.nn AS rnn, nsj_p(provenance()) AS p
  FROM nsj_l LEFT JOIN nsj_r ON nsj_l.k = nsj_r.k;
SELECT remove_provenance('nsj_5');
SELECT * FROM nsj_5 ORDER BY nn, rnn;
SELECT nested_loop FROM nsj_plan(
  'SELECT nsj_l.nn, nsj_r.nn FROM nsj_l LEFT JOIN nsj_r ON nsj_l.nn = nsj_r.nn');

-- AGG(DISTINCT) with a nullable key.
CREATE TABLE nsj_6 AS SELECT k, count(DISTINCT t) AS c FROM nsj_l GROUP BY k;
SELECT remove_provenance('nsj_6');
SELECT k, c FROM nsj_6 ORDER BY k;
SELECT * FROM nsj_plan('SELECT k, count(DISTINCT t) FROM nsj_l GROUP BY k');
SELECT * FROM nsj_plan('SELECT nn, count(DISTINCT t) FROM nsj_l GROUP BY nn');

DROP TABLE nsj_1, nsj_2, nsj_3, nsj_4, nsj_5, nsj_6, nsj_l, nsj_r;
DROP FUNCTION nsj_plan(text);
DROP FUNCTION nsj_p(uuid);
