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
