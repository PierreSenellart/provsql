\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

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
