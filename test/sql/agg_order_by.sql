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
-- Window functions, untracked, warn where the order they read is not
-- determined: lag and lead with rows tying on the ORDER BY (equal values or
-- not), first_value / last_value / nth_value over a frame of whole peer
-- groups with tying rows of different values.  The count only shows the
-- query ran.
CREATE TABLE aob_w AS
  SELECT 'lag, ties' AS q, count(*) AS n
  FROM (SELECT lag(name) OVER (PARTITION BY d ORDER BY k) FROM aob) s;
CREATE TABLE aob_w2 AS
  SELECT 'lag, ties of equal values' AS q, count(*) AS n
  FROM (SELECT lag(name) OVER (ORDER BY k) FROM aob WHERE d = 2) s;
CREATE TABLE aob_w3 AS
  SELECT 'first_value, ties of different values' AS q, count(*) AS n
  FROM (SELECT first_value(name) OVER (PARTITION BY d ORDER BY k) FROM aob) s;
-- no tie warning from here on
CREATE TABLE aob_w4 AS
  SELECT 'first_value, ties of equal values' AS q, count(*) AS n
  FROM (SELECT first_value(name) OVER (ORDER BY k) FROM aob WHERE d = 2) s;
CREATE TABLE aob_w5 AS
  SELECT 'lag, no tie' AS q, count(*) AS n
  FROM (SELECT lag(name) OVER (PARTITION BY d ORDER BY k, name) FROM aob
        WHERE d = 1) s;
SELECT remove_provenance('aob_w');
SELECT remove_provenance('aob_w2');
SELECT remove_provenance('aob_w3');
SELECT remove_provenance('aob_w4');
SELECT remove_provenance('aob_w5');
DROP TABLE aob_w, aob_w2, aob_w3, aob_w4, aob_w5;

-- LIMIT over rows that tie only in other worlds: two null-padded rows of a
-- LEFT JOIN, absent from the database as it is, are present together where
-- the matched rows are not; the tie warns.  A total order does not.
CREATE TABLE aob_dept(dept text, name text);
CREATE TABLE aob_emp(id text, dept text, salary int);
INSERT INTO aob_dept VALUES ('A','Accounting'), ('B','Bakery'), ('C','Catering');
INSERT INTO aob_emp VALUES ('a1','A',100), ('a2','A',90), ('b1','B',120);
SELECT add_provenance('aob_dept');
SELECT add_provenance('aob_emp');
CREATE TABLE aob_l AS
  SELECT d.name, e.id FROM aob_dept d LEFT JOIN aob_emp e ON e.dept = d.dept
  ORDER BY e.salary DESC NULLS LAST, e.id LIMIT 3;
-- no warning
CREATE TABLE aob_l2 AS
  SELECT d.name, e.id FROM aob_dept d LEFT JOIN aob_emp e ON e.dept = d.dept
  ORDER BY e.salary DESC NULLS LAST, e.id, d.dept LIMIT 3;
SELECT remove_provenance('aob_l');
SELECT remove_provenance('aob_l2');
DROP TABLE aob_l, aob_l2;
SELECT remove_provenance('aob_dept');
SELECT remove_provenance('aob_emp');
DROP TABLE aob_dept, aob_emp;

SELECT remove_provenance('aob');
SELECT remove_provenance('aob_ex');
DROP TABLE aob, aob_ex;
