\set ECHO none
\pset format unaligned

CREATE TABLE union_all_result AS
SELECT *,sr_formula(provenance(),'personnel_name') AS formula FROM (
  SELECT classification FROM personnel WHERE city='Paris'
  UNION ALL
  SELECT classification FROM personnel
) t;

SELECT remove_provenance('union_all_result');
SELECT * FROM union_all_result ORDER BY classification;
DROP TABLE union_all_result;

CREATE TABLE union_all_result AS
  SELECT * FROM personnel
  UNION ALL
  SELECT * FROM personnel;

SELECT remove_provenance('union_all_result');
SELECT * FROM union_all_result ORDER BY id;
DROP TABLE union_all_result;

-- UNION ALL of different aggregation levels
CREATE TABLE union_all_agg_result AS
SELECT level, grp, sr_counting(provenance(),'personnel_count') AS counting FROM (
  SELECT 'by_city' AS level, city AS grp, COUNT(*)
  FROM personnel GROUP BY city
  UNION ALL
  SELECT 'total', 'all', COUNT(*)
  FROM personnel
) t;

SELECT remove_provenance('union_all_agg_result');
SELECT * FROM union_all_agg_result ORDER BY level, grp;
DROP TABLE union_all_agg_result;

-- A column where aggregates meet a plain value reads the aggregates as
-- values; one whose arms are all aggregates stays an agg_token, also in a
-- nested UNION ALL.
CREATE TABLE union_all_agg_result AS
  SELECT 'a' AS item, count(*) AS c FROM personnel
  UNION ALL SELECT 'b', 0;
SELECT remove_provenance('union_all_agg_result');
SELECT item, c FROM union_all_agg_result ORDER BY item;
DROP TABLE union_all_agg_result;
CREATE TABLE union_all_agg_result AS
  SELECT 'a' AS item, count(*) AS c FROM personnel
  UNION ALL SELECT 'b', count(*) FROM personnel WHERE city = 'Paris'
  UNION ALL SELECT 'c', count(*) FROM personnel WHERE city = 'Berlin';
SELECT remove_provenance('union_all_agg_result');
SELECT item, c FROM union_all_agg_result ORDER BY item;
DROP TABLE union_all_agg_result;
