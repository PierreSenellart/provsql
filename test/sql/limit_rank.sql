\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- ORDER BY ... LIMIT k over tracked relations keeps, in each possible world,
-- the present rows that fewer than k present rows precede: the filter of
-- row_number() <= k, or of rank() <= k with FETCH ... WITH TIES.  Every row
-- that may be kept is output, annotated with that condition.  LIMIT
-- plain(k) truncates the actual result instead.  Rows 2 and 3 tie on x; y
-- has NULLs.

CREATE TABLE lr_plain(id int, g text, x int, y int, p float8);
INSERT INTO lr_plain VALUES
  (1, 'a', 10, 1, 0.2), (2, 'a', 20, NULL, 0.5), (3, 'a', 20, 3, 0.7),
  (4, 'a', 30, 4, 0.4), (5, 'b', 5, NULL, 0.6), (6, 'b', 7, 6, 0.9);
CREATE TABLE lr AS SELECT id, g, x, y FROM lr_plain;
SELECT add_provenance('lr');
SELECT set_prob(provenance(), p) FROM lr JOIN lr_plain USING (id) \g /dev/null
CREATE SCHEMA lr_world;

-- For each output key k of the query, its probability by ProvSQL over lr, and
-- by running the query over each of the 64 worlds of lr_plain (a view lr of
-- lr_world, first on the search_path, holds the rows of the world).
CREATE FUNCTION lr_check(label text, sql text)
  RETURNS TABLE(q text, exact boolean, probabilities text) LANGUAGE plpgsql AS $f$
DECLARE
  m int;
BEGIN
  CREATE TEMP TABLE lr_tracked(k text, p float8);
  CREATE TEMP TABLE lr_brute(k text, pw float8);
  SET LOCAL search_path TO provsql_test, provsql;
  EXECUTE format('INSERT INTO lr_tracked SELECT k::text, probability_evaluate(provenance())
                  FROM (%s) u', sql);
  FOR m IN 0 .. 63 LOOP
    EXECUTE format('CREATE OR REPLACE VIEW lr_world.lr AS
                    SELECT id, g, x, y FROM provsql_test.lr_plain
                    WHERE %s & (1 << (id - 1)) <> 0', m);
    SET LOCAL search_path TO lr_world, provsql_test, provsql;
    EXECUTE format(
      'INSERT INTO lr_brute
       SELECT k::text, (SELECT exp(sum(ln(CASE WHEN %1$s & (1 << (id - 1)) <> 0
                                         THEN p ELSE 1 - p END)))
                        FROM provsql_test.lr_plain)
       FROM (%2$s) u', m, sql);
    SET LOCAL search_path TO provsql_test, provsql;
  END LOOP;
  RETURN QUERY
    SELECT label,
           bool_and(round(coalesce(t.p, 0)::numeric, 9) =
                    round(coalesce(b.p, 0)::numeric, 9)),
           string_agg(coalesce(t.k, b.k) || '=' ||
                      round(coalesce(t.p, 0)::numeric, 4),
                      ' ' ORDER BY coalesce(t.k, b.k))
    FROM (SELECT t.k, sum(t.p) AS p FROM lr_tracked t GROUP BY t.k) t
    FULL JOIN (SELECT b.k, sum(b.pw) AS p FROM lr_brute b GROUP BY b.k) b
      USING (k);
  DROP TABLE lr_tracked, lr_brute;
END
$f$;

SELECT * FROM lr_check('top 2',
  'SELECT id AS k FROM lr ORDER BY x DESC, id LIMIT 2');
SELECT * FROM lr_check('first',
  'SELECT id AS k FROM lr ORDER BY x, id LIMIT 1');
SELECT * FROM lr_check('OFFSET and LIMIT',
  'SELECT id AS k FROM lr ORDER BY x, id OFFSET 1 LIMIT 2');
SELECT * FROM lr_check('OFFSET alone',
  'SELECT id AS k FROM lr ORDER BY x, id OFFSET 2');
SELECT * FROM lr_check('after a selection, NULLs first',
  'SELECT id AS k FROM lr WHERE g = ''a'' ORDER BY y NULLS FIRST, id LIMIT 2');
SELECT * FROM lr_check('several keys',
  'SELECT id AS k FROM lr ORDER BY g, x DESC, id LIMIT 3');
-- DISTINCT ON (g) keeps the first row of each group: the filter of
-- row_number() OVER (PARTITION BY g ORDER BY ...) <= 1, checked world by
-- world; with a LIMIT on top, and the ORDER BY key of the rows kept.
SELECT * FROM lr_check('DISTINCT ON',
  'SELECT DISTINCT ON (g) id AS k FROM lr ORDER BY g, x DESC, id');
SELECT * FROM lr_check('DISTINCT ON, then LIMIT',
  'SELECT DISTINCT ON (g) id AS k FROM lr ORDER BY g, x, id LIMIT 1');
SELECT * FROM lr_check('DISTINCT ON a key other than the first',
  'SELECT DISTINCT ON (g) g AS k FROM lr WHERE y IS NOT NULL ORDER BY g, id');
SELECT * FROM lr_check('top 1 per group, LATERAL',
  'SELECT gs.g || '':'' || u.id AS k
   FROM (SELECT DISTINCT g FROM provsql_test.lr_plain) gs,
        LATERAL (SELECT id FROM lr w WHERE w.g = gs.g ORDER BY x DESC, id LIMIT 1) u');
SELECT * FROM lr_check('in a CTE, then filtered',
  'WITH top AS (SELECT id, x FROM lr ORDER BY x DESC, id LIMIT 3)
   SELECT id AS k FROM top WHERE x < 30');
SELECT * FROM lr_check('arms of UNION ALL',
  'SELECT id AS k FROM (SELECT id FROM lr ORDER BY x, id LIMIT 1) a
   UNION ALL SELECT id FROM (SELECT id FROM lr ORDER BY x DESC, id LIMIT 1) b');
SELECT * FROM lr_check('arm of UNION',
  '(SELECT id AS k FROM lr ORDER BY x DESC, id LIMIT 1)
   UNION SELECT id FROM lr WHERE g = ''b''');
SELECT * FROM lr_check('two conditions on one count',
  'SELECT g AS k FROM (SELECT g, count(*) AS c FROM lr GROUP BY g) u
   WHERE c > 1 AND c <= 3');
SELECT * FROM lr_check('after a filter on a rank',
  'SELECT id AS k FROM (SELECT id, g, x,
                        row_number() OVER (PARTITION BY g ORDER BY x DESC, id) AS rn
                        FROM lr) t
   WHERE rn = 1 ORDER BY id DESC LIMIT 1');
SELECT * FROM lr_check('a window over rows filtered on a rank',
  'SELECT id AS k FROM (SELECT id, count(*) OVER (ORDER BY id DESC) AS c
                        FROM (SELECT id FROM (SELECT id, g, x,
                              row_number() OVER (PARTITION BY g ORDER BY x DESC, id) AS rn
                              FROM lr) t WHERE rn = 1) f) z
   WHERE c <= 1');
SELECT * FROM lr_check('joined back',
  'SELECT t.id || ''-'' || w.id AS k
   FROM (SELECT id, g FROM lr ORDER BY x DESC, id LIMIT 2) t
   JOIN lr w ON w.g = t.g AND w.id < t.id');

-- The output: every candidate row, its provenance and probability including
-- the condition; SELECT * keeps the columns of the table.
CREATE TABLE lr_top AS
  SELECT *, probability_evaluate(provenance()) AS p
  FROM lr ORDER BY x DESC, id LIMIT 2;
SELECT remove_provenance('lr_top');
SELECT id, g, x, y, round(p::numeric, 4) AS p FROM lr_top ORDER BY id;

-- LIMIT plain(k): the first k rows of the actual result, with the
-- provenance of the full result (the probability of the row itself).
CREATE TABLE lr_actual AS
  SELECT id, probability_evaluate(provenance()) AS p
  FROM lr ORDER BY x DESC, id LIMIT plain(2);
SELECT remove_provenance('lr_actual');
SELECT id, round(p::numeric, 4) AS p FROM lr_actual ORDER BY id;
CREATE TABLE lr_actual2 AS
  SELECT id FROM lr ORDER BY x, id OFFSET plain(1) FETCH FIRST plain(2) ROWS ONLY;
SELECT remove_provenance('lr_actual2');
SELECT id FROM lr_actual2 ORDER BY id;

-- LIMIT over ties: row_number(), tracked as rank(), with a warning.
CREATE TABLE lr_ties AS SELECT id FROM lr ORDER BY x DESC LIMIT 2;
SELECT remove_provenance('lr_ties');
SELECT count(*) AS candidates FROM lr_ties;

-- A subquery with a provenance column, filtered on a column after it.
CREATE TABLE lr_star AS
  SELECT id, rn FROM (SELECT *, row_number() OVER (ORDER BY x DESC, id) AS rn
                      FROM lr) u
  WHERE rn <= 2;
SELECT remove_provenance('lr_star');
SELECT id, rn FROM lr_star ORDER BY id;

-- A top 3 over 40 rows: a COUNT comparison over independent rows,
-- evaluated by the Poisson-binomial pre-pass.
CREATE TABLE lr_many AS
  SELECT i AS id, (i * 7919) % 1000 AS score FROM generate_series(1, 40) i;
SELECT add_provenance('lr_many');
SELECT set_prob(provenance(), 0.3 + (id % 5) / 10.0) FROM lr_many \g /dev/null
SET provsql.verbose_level = 5;
CREATE TABLE lr_many_top AS
  SELECT id, probability_evaluate(provenance()) AS p
  FROM (SELECT id FROM lr_many ORDER BY score DESC LIMIT 3) u
  WHERE id = 7;
-- OFFSET m LIMIT k compares the rank twice: the two comparisons on the one
-- count make a range, evaluated by the same pre-pass.
CREATE TABLE lr_many_range AS
  SELECT id, probability_evaluate(provenance()) AS p
  FROM (SELECT id FROM lr_many ORDER BY score DESC OFFSET 10 LIMIT 5) u
  WHERE id = 7;
RESET provsql.verbose_level;
SELECT remove_provenance('lr_many_top');
SELECT id, round(p::numeric, 12) AS p FROM lr_many_top;
SELECT remove_provenance('lr_many_range');
SELECT id, round(p::numeric, 12) AS p FROM lr_many_range;

DROP TABLE lr_top, lr_actual, lr_actual2, lr_ties, lr_star, lr_many,
  lr_many_top, lr_many_range;

-- FETCH ... WITH TIES (PostgreSQL 13+): rank() <= k.
SELECT current_setting('server_version_num')::int >= 130000 AS pg_has_with_ties
\gset
\if :pg_has_with_ties
SELECT * FROM lr_check('WITH TIES',
  'SELECT id AS k FROM lr ORDER BY x FETCH FIRST 2 ROWS WITH TIES');
SELECT * FROM lr_check('WITH TIES, first',
  'SELECT id AS k FROM lr ORDER BY x DESC FETCH FIRST 1 ROW WITH TIES');
\else
\echo limit_rank: WITH TIES skipped on PostgreSQL < 13
\endif

-- An aggregate over a rank-filtered LIMIT displays the value of the rows
-- the LIMIT keeps, as plain SQL does, not of every candidate row.
CREATE TABLE lr_t AS
  SELECT count(*) AS c, sum(x) AS s
  FROM (SELECT x FROM lr ORDER BY x, id LIMIT 2) t;
SELECT remove_provenance('lr_t');
SELECT 'DISPLAY limit' AS q, c, s FROM lr_t;
DROP TABLE lr_t;
SELECT 'PLAIN limit' AS q, count(*) AS c, sum(x) AS s
FROM (SELECT x FROM lr_plain ORDER BY x, id LIMIT 2) t;

DROP FUNCTION lr_check(text, text);
-- Without ProvSQL's schema in the search_path: the operators the rewriting
-- builds on its own types (the rank as a count plus one, arithmetic on an
-- aggregate) are looked up in that schema.
SET search_path TO provsql_test;
CREATE TABLE lr_nopath AS SELECT id FROM lr ORDER BY x DESC, id LIMIT 2;
CREATE TABLE lr_nopath_agg AS SELECT g, count(*) + 1 AS c FROM lr GROUP BY g;
SELECT provsql.remove_provenance('lr_nopath');
SELECT provsql.remove_provenance('lr_nopath_agg');
SELECT count(*) AS candidates FROM lr_nopath;
SELECT g, c::text AS c, pg_typeof(c) AS type FROM lr_nopath_agg ORDER BY g;
DROP TABLE lr_nopath, lr_nopath_agg;
SET search_path TO provsql_test, provsql;

DROP VIEW lr_world.lr;
DROP SCHEMA lr_world;
DROP TABLE lr, lr_plain;


-- The top-k of an aggregation whose sort key is NOT a bare aggregate of the
-- block: an aggregate column of a derived table, and a scalar subquery that
-- counts -- the way the query is usually written (103 such queries in the
-- SEDE corpus, prevalence-ac).  Both are the filter of a rank like the bare
-- form, so the answer holds every row that is in the top k in SOME world, each
-- with the provenance of being there.
--
-- What blocked them: the lowering wraps the query it ranks in a pass-through
-- subquery, and the rank-over-an-aggregate rewrite looked for the aggregation
-- directly below the window, so the wrapper hid it.
--
-- Over four rows at one half, two of them of the first group: the first group
-- is in the top two of every world where it has a row (3/4), the others in
-- every world where theirs is there (1/2).
CREATE TABLE lr_v(id int, userid int);
INSERT INTO lr_v VALUES (1,1),(2,1),(3,2),(4,3);
SELECT add_provenance('lr_v');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM lr_v; END $$;
CREATE TABLE lr_top AS
  SELECT userid, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM (SELECT userid, count(*) AS n FROM lr_v GROUP BY userid) t
  ORDER BY n DESC LIMIT 2;
SELECT remove_provenance('lr_top');
SELECT userid, p FROM lr_top ORDER BY userid;
DROP TABLE lr_top;
CREATE TABLE lr_top AS
  SELECT userid,
         round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM (SELECT DISTINCT userid FROM lr_v) u
  ORDER BY (SELECT count(*) FROM lr_v WHERE lr_v.userid = u.userid) DESC
  LIMIT 2;
SELECT remove_provenance('lr_top');
SELECT userid, p FROM lr_top ORDER BY userid;
DROP TABLE lr_top;
-- An ORDER BY of SEVERAL keys, the aggregate one and a tie-breaker beside it,
-- which is how a top-k is written where two groups can have the same count:
-- the rank is the lexicographic one, so a group that ties on the count is
-- before or after by the second key rather than tying.  Two rows in the first
-- group and one in the second, each present with probability one half: the
-- first group is in the top one of every world where it has a row (3/4), and
-- the second only in the world where its row is there and neither of the
-- others is (1/8) -- where the count alone would leave it the rank of a tie in
-- the two worlds where the counts are equal (3/8).  And because the keys hold
-- the grouping column, no two groups can tie on all of them: row_number() is
-- the rank it is tracked as, so the warning that they may differ is not
-- raised.
CREATE TABLE lr_tie(g int, d int);
INSERT INTO lr_tie VALUES (1,10),(1,10),(2,20);
SELECT add_provenance('lr_tie');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM lr_tie; END $$;
CREATE TABLE lr_top AS
  SELECT g, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM (SELECT g, count(*) AS n FROM lr_tie GROUP BY g) t
  ORDER BY n DESC, g ASC LIMIT 1;
SELECT remove_provenance('lr_top');
SELECT g, p FROM lr_top ORDER BY g;
DROP TABLE lr_top;
-- The same without the tie-breaker, where the tie stands.
CREATE TABLE lr_top AS
  SELECT g, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM (SELECT g, count(*) AS n FROM lr_tie GROUP BY g) t
  ORDER BY n DESC LIMIT 1;
SELECT remove_provenance('lr_top');
SELECT g, p FROM lr_top ORDER BY g;
DROP TABLE lr_top;
SELECT remove_provenance('lr_tie');
DROP TABLE lr_tie;
SELECT remove_provenance('lr_v');
DROP TABLE lr_v;
