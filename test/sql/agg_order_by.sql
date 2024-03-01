\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

CREATE TABLE agg_order_result AS
  SELECT city, count(*)
    FROM personnel
    GROUP BY city ORDER BY city;

SELECT remove_provenance('agg_order_result');

SELECT * FROM agg_order_result ORDER BY city;

DROP TABLE agg_order_result;

SELECT city, count(*)
FROM personnel
GROUP BY city ORDER BY count(*);
