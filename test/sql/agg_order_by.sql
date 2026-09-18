\set ECHO none
\pset format unaligned

CREATE TABLE agg_order_result AS
  SELECT city, count(*)
    FROM personnel
    GROUP BY city ORDER BY city;

SELECT remove_provenance('agg_order_result');

SELECT * FROM agg_order_result ORDER BY city;

DROP TABLE agg_order_result;

-- ORDER BY on an aggregate result sorts on its plain value, with a warning
-- at the top level; the rows keep their provenance.  A table created from a
-- sorted query keeps the rows in that order.
CREATE TABLE agg_order_result AS
  SELECT city, count(*)
    FROM personnel
    GROUP BY city ORDER BY count(*) DESC, city;
SELECT remove_provenance('agg_order_result');
SELECT * FROM agg_order_result;
DROP TABLE agg_order_result;

-- Also on the aggregate result of a subquery, and in a subquery (no warning)
CREATE TABLE agg_order_result AS
  SELECT city, cnt
    FROM (SELECT city, count(*) AS cnt FROM personnel GROUP BY city) t
    ORDER BY cnt DESC, city;
SELECT remove_provenance('agg_order_result');
SELECT * FROM agg_order_result;
DROP TABLE agg_order_result;
CREATE TABLE agg_order_result AS
  SELECT *
    FROM (SELECT city, max(name) AS m FROM personnel GROUP BY city
          ORDER BY max(name)) t;
SELECT remove_provenance('agg_order_result');
SELECT * FROM agg_order_result ORDER BY city;
DROP TABLE agg_order_result;
