\set ECHO none
SET search_path TO provsql_test, provsql;

CREATE TABLE agg_result AS
  SELECT city, count(distinct classification)
    FROM personnel
    GROUP BY city ORDER BY city;

SELECT remove_provenance('agg_result');

SELECT * FROM agg_result ORDER BY city;

SELECT city, aggregation_formula(count,'personnel_name') FROM agg_result ORDER BY city;

DROP TABLE agg_result;
