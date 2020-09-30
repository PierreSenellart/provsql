\set ECHO none
SET search_path TO public,provsql;

CREATE TABLE agg_result AS
  SELECT count(distinct classification)
    FROM (SELECT name,classification FROM personnel ORDER BY name) b;

SELECT remove_provenance('agg_result');

SELECT * FROM agg_result;

SELECT aggregation_formula(count,'personnel_name') FROM agg_result;

CREATE TABLE agg_result2 AS
  SELECT city, count(distinct classification)
    FROM personnel
    GROUP BY city ORDER BY city, classification;

SELECT remove_provenance('agg_result2');

SELECT * FROM agg_result2 ORDER BY city;

SELECT city, aggregation_formula(count,'personnel_name') FROM agg_result2 ORDER BY city;

DROP TABLE agg_result;
DROP TABLE agg_result2;
