\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Aggregates used as window functions over a frame determined by values: the
-- row keeps its token, the value becomes an agg_token over the rows of the
-- frame.  Rows 2 and 3 are peers (same x), rows 2 and 5 have a NULL y.
CREATE TABLE wa_plain(id int, g text, x int, y int, p float8);
INSERT INTO wa_plain VALUES
  (1, 'a', 10, 1, 0.2), (2, 'a', 20, NULL, 0.5), (3, 'a', 20, 3, 0.7),
  (4, 'a', 30, 4, 0.4), (5, 'b', 5, NULL, 0.6), (6, 'b', 7, 6, 0.9);
CREATE TABLE wa AS SELECT id, g, x, y FROM wa_plain;
SELECT add_provenance('wa');
SELECT set_prob(provenance(), p) FROM wa JOIN wa_plain USING (id) \g /dev/null

-- For each row, the probability that pred holds of its value v of expr, by
-- ProvSQL over wa and by enumeration of the 64 worlds of wa_plain, each world
-- running expr over its own rows.
CREATE FUNCTION wa_check(expr text, pred text)
  RETURNS TABLE(id int, tracked numeric, brute numeric) LANGUAGE plpgsql AS $f$
DECLARE
  m int;
BEGIN
  CREATE TEMP TABLE wa_tracked(id int, p float8);
  CREATE TEMP TABLE wa_brute(id int, pw float8);
  EXECUTE format('INSERT INTO wa_tracked SELECT id, probability_evaluate(provenance())
                  FROM (SELECT id, %s AS v FROM wa) u WHERE %s', expr, pred);
  FOR m IN 0 .. 63 LOOP
    EXECUTE format(
      'INSERT INTO wa_brute
       SELECT id, (SELECT exp(sum(ln(CASE WHEN %1$s & (1 << (id - 1)) <> 0
                                      THEN p ELSE 1 - p END))) FROM wa_plain)
       FROM (SELECT id, %2$s AS v
             FROM (SELECT * FROM wa_plain WHERE %1$s & (1 << (id - 1)) <> 0) wa) u
       WHERE %3$s', m, expr, pred);
  END LOOP;
  RETURN QUERY
    SELECT w.id, round(coalesce(t.p, 0)::numeric, 9), round(coalesce(b.p, 0)::numeric, 9)
    FROM wa_plain w
    LEFT JOIN wa_tracked t USING (id)
    LEFT JOIN (SELECT b.id, sum(b.pw) AS p FROM wa_brute b GROUP BY b.id) b USING (id)
    ORDER BY w.id;
  DROP TABLE wa_tracked, wa_brute;
END
$f$;

CREATE FUNCTION wa_report(label text, expr text, pred text)
  RETURNS TABLE(q text, exact boolean, probabilities numeric[]) LANGUAGE sql AS $f$
  SELECT label, bool_and(tracked = brute), array_agg(tracked ORDER BY id)
  FROM wa_check(expr, pred)
$f$;

SELECT * FROM wa_report('whole partition',
  'sum(x) OVER (PARTITION BY g)', 'v > 25');
SELECT * FROM wa_report('whole table',
  'count(*) OVER ()', 'v >= 4');
SELECT * FROM wa_report('whole partition, ROWS',
  'sum(x) OVER (PARTITION BY g ORDER BY x ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING)',
  'v > 40');
SELECT * FROM wa_report('running sum, peers included',
  'sum(x) OVER (PARTITION BY g ORDER BY x)', 'v > 25');
SELECT * FROM wa_report('running count',
  'count(*) OVER (PARTITION BY g ORDER BY x)', 'v <= 2');
SELECT * FROM wa_report('running count, descending',
  'count(*) OVER (PARTITION BY g ORDER BY x DESC)', 'v <= 2');
SELECT * FROM wa_report('count of a nullable column',
  'count(y) OVER (PARTITION BY g)', 'v < 2');
SELECT * FROM wa_report('count(*) with FILTER',
  'count(*) FILTER (WHERE x > 6) OVER ()', 'v >= 3');
SELECT * FROM wa_report('sum with FILTER',
  'sum(x) FILTER (WHERE y IS NOT NULL) OVER (PARTITION BY g)', 'v > 20');
SELECT * FROM wa_report('running max',
  'max(x) OVER (PARTITION BY g ORDER BY id)', 'v >= 20');
SELECT * FROM wa_report('min',
  'min(x) OVER (PARTITION BY g)', 'v < 15');
SELECT * FROM wa_report('avg',
  'avg(x) OVER (PARTITION BY g)', 'v > 18');
-- A ratio of TWO aggregates, which the per-world evaluation used to truncate:
-- the gate says which division SQL means (the rewriting emits the integer one
-- exactly where the expression's type is an integer type), and reading that
-- from the values instead made "count / count" 1 or 0 per world, so this
-- comparison answered the probability that the two counts are equal -- 0.3
-- where the enumeration says 0.452 for row 2.  Rows 2 and 3 are peers, which
-- is where the two counts differ by more than the rows after them.
SELECT * FROM wa_report('ratio of two counts',
  '(count(*) OVER (PARTITION BY g ORDER BY x))::numeric
     / (count(*) OVER (PARTITION BY g))::numeric', 'v > 0.5');
SELECT * FROM wa_report('ratio of two sums',
  '(sum(x) OVER (PARTITION BY g ORDER BY x))::numeric
     / (sum(x) OVER (PARTITION BY g))::numeric', 'v > 0.6');
-- cume_dist() is that ratio, by its definition -- the rows up to the current
-- row's peers over the rows of the partition -- so it is tracked where it used
-- to be read as a plain value, and the enumeration says it is right in each of
-- the 64 worlds.
SELECT * FROM wa_report('cume_dist over a partition',
  'cume_dist() OVER (PARTITION BY g ORDER BY x)', 'v > 0.5');
SELECT * FROM wa_report('cume_dist, whole table',
  'cume_dist() OVER (ORDER BY x)', 'v >= 0.5');
SELECT * FROM wa_report('cume_dist, descending',
  'cume_dist() OVER (PARTITION BY g ORDER BY x DESC)', 'v = 1');
SELECT * FROM wa_report('share of the partition',
  'x * 100 / sum(x) OVER (PARTITION BY g)', 'v > 30');
SELECT * FROM wa_report('rest of the partition',
  'sum(x) OVER (PARTITION BY g) - x', 'v >= 20');
SELECT * FROM wa_report('constant added',
  'count(*) OVER (PARTITION BY g) + 1', 'v < 4');

-- The displayed values are those of the query over the untracked copy.
CREATE TABLE wa_shown AS
  SELECT id,
         sum(x) OVER (PARTITION BY g) AS s,
         sum(x) OVER (PARTITION BY g ORDER BY x) AS rs,
         count(*) OVER (PARTITION BY g ORDER BY x) AS rc,
         count(y) OVER () AS cy,
         avg(x) OVER (PARTITION BY g) AS a,
         array_agg(y) OVER (PARTITION BY g) AS ys
  FROM wa;
SELECT remove_provenance('wa_shown');
SELECT * FROM wa_shown ORDER BY id;
SELECT id,
       sum(x) OVER (PARTITION BY g) AS s,
       sum(x) OVER (PARTITION BY g ORDER BY x) AS rs,
       count(*) OVER (PARTITION BY g ORDER BY x) AS rc,
       count(y) OVER () AS cy,
       avg(x) OVER (PARTITION BY g) AS a,
       array_agg(y) OVER (PARTITION BY g) AS ys
FROM wa_plain ORDER BY id;

-- A whole-partition window is, for each row, the aggregate of the GROUP BY on
-- the partition attributes: the same gate.  A running frame has one gate per
-- row, over the rows up to it and its peers.
CREATE TABLE wa_win AS
  SELECT id, g, sum(x) OVER (PARTITION BY g) AS s,
         count(*) OVER (PARTITION BY g ORDER BY x) AS rc
  FROM wa;
CREATE TABLE wa_grp AS SELECT g, sum(x) AS s FROM wa GROUP BY g;
SELECT remove_provenance('wa_win');
SELECT remove_provenance('wa_grp');
SELECT 'same gate as GROUP BY' AS q,
       bool_and(agg_token_uuid(w.s) = agg_token_uuid(r.s)) AS same
FROM wa_win w JOIN wa_grp r USING (g);
SELECT 'running frame' AS q, id,
       array_length(get_children(agg_token_uuid(rc)), 1) AS children
FROM wa_win ORDER BY id;

-- ORDER BY a window value sorts on the displayed value.
CREATE TABLE wa_sorted AS
  SELECT id, sum(x) OVER (PARTITION BY g) AS s FROM wa ORDER BY s, id;
SELECT remove_provenance('wa_sorted');
SELECT * FROM wa_sorted;

-- Not tracked, with a warning: each row keeps its token, the value is an
-- opaque scalar.  Offset and distribution functions, positional frames, and
-- windows over the groups of an aggregation.
CREATE TABLE wa_untracked AS
  SELECT id,
         lag(x) OVER (PARTITION BY g ORDER BY x, id) AS prev,
         ntile(2) OVER (PARTITION BY g ORDER BY x, id) AS half,
         round(percent_rank() OVER (PARTITION BY g ORDER BY x)::numeric, 4) AS pr,
         sum(x) OVER (PARTITION BY g ORDER BY x, id ROWS 1 PRECEDING) AS last2
  FROM wa;
SELECT remove_provenance('wa_untracked');
SELECT * FROM wa_untracked ORDER BY id;
CREATE TABLE wa_over_groups AS
  SELECT g, sum(x) AS s, sum(sum(x)) OVER () AS total,
         rank() OVER (ORDER BY count(*)) AS rk
  FROM wa GROUP BY g;
SELECT remove_provenance('wa_over_groups');
SELECT * FROM wa_over_groups ORDER BY g;

-- COALESCE over a window value, the rows then joined twice.
CREATE TABLE wa_coalesce AS
  WITH w AS (SELECT id, g, coalesce(sum(y) OVER (ORDER BY id), 0) AS sy FROM wa)
  SELECT w1.id AS id1, w2.id AS id2, w2.sy - w1.sy AS d
  FROM w w1 JOIN w w2 ON w1.g = w2.g AND w1.id + 1 = w2.id;
SELECT remove_provenance('wa_coalesce');
SELECT * FROM wa_coalesce ORDER BY id1;
SELECT w1.id AS id1, w2.id AS id2, w2.sy - w1.sy AS d
FROM (SELECT id, g, coalesce(sum(y) OVER (ORDER BY id), 0) AS sy FROM wa_plain) w1
JOIN (SELECT id, g, coalesce(sum(y) OVER (ORDER BY id), 0) AS sy FROM wa_plain) w2
  ON w1.g = w2.g AND w1.id + 1 = w2.id
ORDER BY id1;
DROP TABLE wa_coalesce;

DROP TABLE wa_shown, wa_win, wa_grp, wa_sorted, wa_untracked, wa_over_groups;

-- Frames with an EXCLUDE clause or in GROUPS mode (PostgreSQL 11+), and the
-- ranks built on them.  A frame that excludes the current row may be empty
-- while the row exists: a count is then 0, a sum NULL.

SELECT * FROM wa_report('rows strictly before',
  'count(*) OVER (PARTITION BY g ORDER BY x RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW EXCLUDE GROUP)',
  'v < 1');
SELECT * FROM wa_report('sum strictly before',
  'sum(x) OVER (PARTITION BY g ORDER BY x RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW EXCLUDE GROUP)',
  'v >= 10');
SELECT * FROM wa_report('the others',
  'sum(x) OVER (PARTITION BY g ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE CURRENT ROW)',
  'v > 40');
SELECT * FROM wa_report('peers excluded',
  'count(*) OVER (PARTITION BY g ORDER BY x RANGE BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE TIES)',
  'v >= 3');
SELECT * FROM wa_report('GROUPS, running',
  'sum(x) OVER (PARTITION BY g ORDER BY x GROUPS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)',
  'v > 25');
SELECT * FROM wa_report('RANGE with an offset',
  'count(*) OVER (PARTITION BY g ORDER BY x RANGE BETWEEN 10 PRECEDING AND CURRENT ROW)',
  'v >= 2');
SELECT * FROM wa_report('RANGE before the row',
  'sum(x) OVER (PARTITION BY g ORDER BY x RANGE BETWEEN 15 PRECEDING AND 5 PRECEDING)',
  'v >= 5');

-- Ranks: rank() is 1 + the number of rows strictly before, dense_rank() 1 +
-- the number of distinct ordering values strictly before, and row_number()
-- is its rank, equal when the ORDER BY leaves no ties.
SELECT * FROM wa_report('rank',
  'rank() OVER (PARTITION BY g ORDER BY x)', 'v <= 2');
SELECT * FROM wa_report('rank, first',
  'rank() OVER (ORDER BY x DESC)', 'v = 1');
SELECT * FROM wa_report('rank, beyond',
  'rank() OVER (PARTITION BY g ORDER BY x)', 'v > 2');
SELECT * FROM wa_report('rank, subtracted',
  '3 - rank() OVER (ORDER BY x)', 'v >= 1');
SELECT * FROM wa_report('dense_rank',
  'dense_rank() OVER (PARTITION BY g ORDER BY x)', 'v <= 2');
SELECT * FROM wa_report('dense_rank, equal',
  'dense_rank() OVER (PARTITION BY g ORDER BY x)', 'v = 3');
SELECT * FROM wa_report('dense_rank, two keys',
  'dense_rank() OVER (ORDER BY g, x)', 'v >= 4');
SELECT * FROM wa_report('dense_rank, no ORDER BY',
  'dense_rank() OVER (PARTITION BY g)', 'v = 1');
SELECT * FROM wa_report('row_number, total order',
  'row_number() OVER (PARTITION BY g ORDER BY x, id)', 'v <= 2');

CREATE TABLE wa_ranks AS
  SELECT id,
         rank() OVER (PARTITION BY g ORDER BY x) AS rk,
         dense_rank() OVER (ORDER BY x DESC) AS drk,
         row_number() OVER (PARTITION BY g ORDER BY x, id) AS rn
  FROM wa;
SELECT remove_provenance('wa_ranks');
SELECT * FROM wa_ranks ORDER BY id;
SELECT id,
       rank() OVER (PARTITION BY g ORDER BY x) AS rk,
       dense_rank() OVER (ORDER BY x DESC) AS drk,
       row_number() OVER (PARTITION BY g ORDER BY x, id) AS rn
FROM wa_plain ORDER BY id;

-- row_number() over ties: the rank is shown and tracked, with one warning.
CREATE TABLE wa_row_number_ties AS
  SELECT id, row_number() OVER (PARTITION BY g ORDER BY x) AS rn FROM wa;
SELECT remove_provenance('wa_row_number_ties');
SELECT * FROM wa_row_number_ties ORDER BY id;

-- The top-k idiom over a larger table: the comparison of a rank with a
-- constant is a COUNT comparison over independent rows, evaluated by the
-- Poisson-binomial pre-pass.
CREATE TABLE wa_many AS
  SELECT i AS id, (i * 7919) % 1000 AS score FROM generate_series(1, 40) i;
SELECT add_provenance('wa_many');
SELECT set_prob(provenance(), 0.3 + (id % 5) / 10.0) FROM wa_many \g /dev/null
SET provsql.verbose_level = 5;
CREATE TABLE wa_top AS
  SELECT id, probability_evaluate(provenance()) AS p
  FROM (SELECT id, rank() OVER (ORDER BY score DESC) AS rk FROM wa_many) u
  WHERE rk <= 3 AND id = 7;
RESET provsql.verbose_level;
SELECT remove_provenance('wa_top');
SELECT id, round(p::numeric, 12) AS p FROM wa_top;
DROP TABLE wa_ranks, wa_row_number_ties, wa_many, wa_top;

-- A self-join of ranked rows whose condition has a unary operator.
CREATE TABLE wa_neg AS
  WITH r AS (SELECT id, x, row_number() OVER (PARTITION BY g ORDER BY x, id) AS rn FROM wa)
  SELECT r1.id AS id1, r2.id AS id2
  FROM r r1 JOIN r r2 ON r1.x = -(-r2.x) AND r1.rn = r2.rn AND r1.id < r2.id;
SELECT remove_provenance('wa_neg');
SELECT count(*) AS pairs FROM wa_neg;
DROP TABLE wa_neg;

-- Not tracked: a GROUPS offset counts the peer groups that are present.
CREATE TABLE wa_groups_offset AS
  SELECT id, sum(x) OVER (PARTITION BY g ORDER BY x
                          GROUPS BETWEEN 1 PRECEDING AND CURRENT ROW) AS s
  FROM wa;
SELECT remove_provenance('wa_groups_offset');
SELECT * FROM wa_groups_offset ORDER BY id;
DROP TABLE wa_groups_offset;


-- plain() over a window aggregate says the value is meant as plain SQL: the
-- entry is left to PostgreSQL, so the value is the one of the data as it is
-- and the row keeps its own provenance.  Without this the window value was
-- tracked in spite of the marker and then read back through the cast, which
-- warns that provenance is lost -- once per row.
CREATE TABLE wa_plainmark AS
  SELECT id, plain(sum(x) OVER (PARTITION BY g)) AS s FROM wa;
SELECT remove_provenance('wa_plainmark');
SELECT id, s FROM wa_plainmark ORDER BY id;
DROP TABLE wa_plainmark;

DROP FUNCTION wa_report(text, text, text);
DROP FUNCTION wa_check(text, text);
DROP TABLE wa, wa_plain;
