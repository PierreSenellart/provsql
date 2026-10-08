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

-- An order-dependent aggregate whose order the query does not determine --
-- no ORDER BY, or rows tying on it with different values, in any world --
-- warns: the order of the database as it is is read in every world.  Rows
-- tying with equal values, or a total ORDER BY, do not.
CREATE TABLE aob(d int, k int, name text);
INSERT INTO aob VALUES (1,1,'a'), (1,1,'b'), (1,2,'c'), (2,1,'x'), (2,1,'x');
CREATE TABLE aob_ex(name text);
INSERT INTO aob_ex VALUES ('b');
SELECT add_provenance('aob');
SELECT add_provenance('aob_ex');
CREATE TABLE aob_r AS
  SELECT 'no ORDER BY' AS q, d, string_agg(name, ',') AS v
  FROM aob WHERE d = 1 GROUP BY d;
CREATE TABLE aob_r2 AS
  SELECT 'peers differ' AS q, d, array_agg(name ORDER BY k) AS v
  FROM aob WHERE d = 1 GROUP BY d;
CREATE TABLE aob_r3 AS
  SELECT 'tie in another world only' AS q, d,
         string_agg(name, ',' ORDER BY k) AS v
  FROM aob
  WHERE d = 1 AND k = 1
    AND NOT EXISTS (SELECT 1 FROM aob_ex WHERE aob_ex.name = aob.name)
  GROUP BY d;
-- no warning from here on
CREATE TABLE aob_r4 AS
  SELECT 'total ORDER BY' AS q, d, json_agg(name ORDER BY name, k) AS v
  FROM aob GROUP BY d;
CREATE TABLE aob_r5 AS
  SELECT 'peers equal' AS q, d, string_agg(name, ',' ORDER BY k) AS v
  FROM aob WHERE d = 2 GROUP BY d;
SELECT remove_provenance('aob_r');
SELECT remove_provenance('aob_r2');
SELECT remove_provenance('aob_r3');
SELECT remove_provenance('aob_r4');
SELECT remove_provenance('aob_r5');
SELECT * FROM aob_r;
SELECT * FROM aob_r2;
SELECT * FROM aob_r3;
SELECT * FROM aob_r4 ORDER BY d;
SELECT * FROM aob_r5;
DROP TABLE aob_r, aob_r2, aob_r3, aob_r4, aob_r5;
SELECT remove_provenance('aob');
SELECT remove_provenance('aob_ex');
DROP TABLE aob, aob_ex;
