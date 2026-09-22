\set ECHO none
\pset format unaligned

-- Single DISTINCT aggregate
CREATE TABLE agg_result AS
  SELECT city, count(distinct classification)
    FROM personnel
    GROUP BY city ORDER BY city;

SELECT remove_provenance('agg_result');

SELECT * FROM agg_result ORDER BY city;

SELECT city, string_agg(word, '+' ORDER BY word) AS aggregation_formula
FROM (
  SELECT city, unnest(string_to_array(sr_formula(count,'personnel_name'),'+')) AS word
  FROM agg_result
) AS temp
GROUP BY city
ORDER BY city;

DROP TABLE agg_result;

-- Multiple DISTINCT aggregates
CREATE TABLE agg_result2 AS
  SELECT city,
         count(*) AS count,
         count(distinct name) AS count_name,
         count(distinct classification) AS count_class
    FROM personnel
    GROUP BY city ORDER BY city;

SELECT remove_provenance('agg_result2');

SELECT * FROM agg_result2 ORDER BY city;

SELECT
  city,
  string_agg(word2, '+' ORDER BY word2) AS count_name,
  string_agg(word3, '+' ORDER BY word3) AS count_class
FROM (
  SELECT
    city,
    unnest(string_to_array(sr_formula(count_name,'personnel_name'),'+')) AS word2,
    unnest(string_to_array(sr_formula(count_class,'personnel_name'),'+')) AS word3
  FROM agg_result2
) AS temp
GROUP BY city
ORDER BY city;

DROP TABLE agg_result2;

-- DISTINCT aggregate mixed with provenance() in the same SELECT
CREATE TABLE agg_result3 AS
  SELECT city,
         count(distinct position) AS cnt,
         sr_counting(provenance(), 'personnel_count') AS counting
    FROM personnel
    GROUP BY city ORDER BY city;

SELECT remove_provenance('agg_result3');

SELECT * FROM agg_result3 ORDER BY city;

DROP TABLE agg_result3;

-- A query without GROUP BY returns one row even when its WHERE keeps none:
-- the AGG(DISTINCT) subqueries and the other aggregates are then read from
-- one-row subqueries, not from the empty rows of the query.
CREATE TABLE agg_result4 AS
  SELECT count(DISTINCT city) - count(DISTINCT position) AS d, count(*) AS n,
         max(id) AS m
  FROM personnel WHERE id > 100;
SELECT remove_provenance('agg_result4');
SELECT * FROM agg_result4;
DROP TABLE agg_result4;
CREATE TABLE agg_result4 AS
  SELECT count(DISTINCT city) - count(DISTINCT position) AS d, count(*) AS n,
         max(id) AS m
  FROM personnel WHERE id > 2;
SELECT remove_provenance('agg_result4');
SELECT * FROM agg_result4;
DROP TABLE agg_result4;

-- AGG(DISTINCT) with constant extra arguments, ordered by the key.
CREATE TABLE agg_result5 AS
  SELECT city, string_agg(DISTINCT position, ', ' ORDER BY position) AS s
  FROM personnel GROUP BY city;
SELECT remove_provenance('agg_result5');
SELECT * FROM agg_result5 ORDER BY city;
DROP TABLE agg_result5;
SELECT string_agg(DISTINCT position, name) FROM personnel;

-- AGG(DISTINCT) in a LATERAL subquery reading a column of a subquery whose
-- provsql column, in the middle of its columns, is moved last.
CREATE TABLE agg_result6 AS
  SELECT a.id, b.n
  FROM (SELECT *, string_to_array(name || ' ' || name, ' ') AS arr
        FROM personnel) a
  LEFT JOIN LATERAL (SELECT count(DISTINCT e) AS n FROM unnest(arr) e) b ON true;
SELECT remove_provenance('agg_result6');
SELECT * FROM agg_result6 ORDER BY id;
DROP TABLE agg_result6;

-- AGG(DISTINCT key) FILTER (WHERE f).  The filter goes into the key, as
-- "CASE WHEN f THEN key END", and not into the WHERE of the deduplicating
-- subquery: a filter that rejects every row of a group leaves that group with
-- a count of 0 and its row in the answer, where a WHERE would have dropped the
-- group with the join.  Over a FROM of one relation the filter was already
-- read; over a join it named a relation the deduplicating subquery does not
-- have, and the rewriting raised "no relation entry for relid".
CREATE TABLE afd(g int, v int, w int);
INSERT INTO afd VALUES (1, 1, 9), (1, 2, 9), (1, 2, 8), (2, 5, 9), (2, 6, 9);
SELECT add_provenance('afd');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM afd; END $$;
-- No row of group 1 passes v > 4, so its count is 0 and the group stays; the
-- sum of an empty selection is NULL, as it is in plain SQL.  The filter reads a
-- column that is not the key (w), so it applies row by row before the
-- deduplication: of group 1 only (1, 2, 8) has w = 8.
CREATE TABLE afd_r AS
  SELECT g, count(DISTINCT v) FILTER (WHERE v > 4) AS n,
            count(DISTINCT v) AS d,
            count(DISTINCT v) FILTER (WHERE w = 8) AS nw,
            sum(DISTINCT v) FILTER (WHERE v > 4) AS s
  FROM afd, (VALUES (1), (2)) x(k) WHERE x.k = 1 GROUP BY g;
SELECT remove_provenance('afd_r');
SELECT g, n::text AS n, d::text AS d, nw::text AS nw, s::text AS s
  FROM afd_r ORDER BY g;
DROP TABLE afd_r;
-- Read in a HAVING, the filtered count carries the provenance of the rows the
-- filter keeps: group 2 holds as soon as one of its two rows is there,
-- 1 - 1/4 = 0.75, and for w = 8 group 1 holds exactly when (1, 2, 8) is,
-- one half.  A group no world can satisfy has probability 0.
CREATE TABLE afd_r AS SELECT g, probability(provenance()) AS p FROM afd
  GROUP BY g HAVING count(DISTINCT v) FILTER (WHERE v > 4) >= 1;
SELECT remove_provenance('afd_r');
SELECT g, round(p::numeric, 6) AS p FROM afd_r ORDER BY g;
DROP TABLE afd_r;
CREATE TABLE afd_r AS SELECT g, probability(provenance()) AS p FROM afd
  GROUP BY g HAVING count(DISTINCT v) FILTER (WHERE w = 8) >= 1;
SELECT remove_provenance('afd_r');
SELECT g, round(p::numeric, 6) AS p FROM afd_r ORDER BY g;
DROP TABLE afd_r;
-- An aggregate that reads a NULL input as a value of its own would see the
-- rejected rows as an extra element, so it is refused rather than given one.
SELECT g, array_agg(DISTINCT v) FILTER (WHERE v > 4) FROM afd GROUP BY g;
DROP TABLE afd;
