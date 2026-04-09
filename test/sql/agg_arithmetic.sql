\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Arithmetic on aggregate results (issue #63)
-- These queries require casts from agg_token to the original aggregate type

-- Multiplication by constant
CREATE TABLE agg_arith1 AS
  SELECT position, count(*) * 10 AS scaled_count FROM personnel GROUP BY position;
SELECT remove_provenance('agg_arith1');
SELECT * FROM agg_arith1 ORDER BY position;
DROP TABLE agg_arith1;

-- Division by constant
CREATE TABLE agg_arith2 AS
  SELECT position, count(*) / 2.0 AS half_count FROM personnel GROUP BY position;
SELECT remove_provenance('agg_arith2');
SELECT * FROM agg_arith2 ORDER BY position;
DROP TABLE agg_arith2;

-- Addition of constant
CREATE TABLE agg_arith3 AS
  SELECT position, count(*) + 100 AS offset_count FROM personnel GROUP BY position;
SELECT remove_provenance('agg_arith3');
SELECT * FROM agg_arith3 ORDER BY position;
DROP TABLE agg_arith3;

-- AVG with multiplication
CREATE TABLE agg_arith4 AS
  SELECT AVG(id) * 2 AS doubled_avg FROM personnel;
SELECT remove_provenance('agg_arith4');
SELECT * FROM agg_arith4;
DROP TABLE agg_arith4;

-- SUM with arithmetic
CREATE TABLE agg_arith5 AS
  SELECT position, SUM(id) + 1 AS sum_plus_one FROM personnel GROUP BY position;
SELECT remove_provenance('agg_arith5');
SELECT * FROM agg_arith5 ORDER BY position;
DROP TABLE agg_arith5;

-- Explicit cast of aggregate result to numeric
CREATE TABLE agg_arith_cast AS
  SELECT city, count(*)::numeric AS cnt_numeric FROM personnel GROUP BY city;
SELECT remove_provenance('agg_arith_cast');
SELECT * FROM agg_arith_cast ORDER BY city;
DROP TABLE agg_arith_cast;

-- Arithmetic on aggregate from subquery
CREATE TABLE agg_arith_sub AS
  SELECT city, cnt + 1 AS plus_one
  FROM (SELECT city, COUNT(*) AS cnt FROM personnel GROUP BY city) t;
SELECT remove_provenance('agg_arith_sub');
SELECT * FROM agg_arith_sub ORDER BY city;
DROP TABLE agg_arith_sub;

-- Window function over aggregate from subquery
CREATE TABLE agg_arith_win AS
  SELECT city, cnt, SUM(cnt) OVER () AS total
  FROM (SELECT city, COUNT(*) AS cnt FROM personnel GROUP BY city) t;
SELECT remove_provenance('agg_arith_win');
SELECT * FROM agg_arith_win ORDER BY city;
DROP TABLE agg_arith_win;

-- COALESCE on aggregate from subquery
CREATE TABLE agg_arith_coalesce AS
  SELECT city, COALESCE(cnt, 0) AS cnt
  FROM (SELECT city, COUNT(*) AS cnt FROM personnel GROUP BY city) t;
SELECT remove_provenance('agg_arith_coalesce');
SELECT * FROM agg_arith_coalesce ORDER BY city;
DROP TABLE agg_arith_coalesce;

-- GREATEST on aggregate from subquery
CREATE TABLE agg_arith_greatest AS
  SELECT city, GREATEST(cnt, 3) AS at_least_3
  FROM (SELECT city, COUNT(*) AS cnt FROM personnel GROUP BY city) t;
SELECT remove_provenance('agg_arith_greatest');
SELECT * FROM agg_arith_greatest ORDER BY city;
DROP TABLE agg_arith_greatest;

-- Arithmetic on AVG (numeric type) from subquery
CREATE TABLE agg_arith_avg AS
  SELECT city, avg_id * 2 AS doubled
  FROM (SELECT city, AVG(id) AS avg_id FROM personnel GROUP BY city) t;
SELECT remove_provenance('agg_arith_avg');
SELECT * FROM agg_arith_avg ORDER BY city;
DROP TABLE agg_arith_avg;

-- String aggregate with concatenation
CREATE TABLE agg_arith6 AS
  SELECT city, string_agg(name, ', ' ORDER BY name) || ' (team)' AS team
    FROM personnel GROUP BY city;
SELECT remove_provenance('agg_arith6');
SELECT * FROM agg_arith6 ORDER BY city;
DROP TABLE agg_arith6;
