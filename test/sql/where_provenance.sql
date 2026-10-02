\set ECHO none
\pset format unaligned

SET provsql.provenance = 'where';

/* Example of where-provenance */
CREATE TABLE result_where AS
  SELECT p1.city AS c1, p2.city AS c2,
    regexp_replace(where_provenance(provenance()),':[0-9a-f-]*:','::','g')
  FROM personnel p1, personnel p2
  WHERE p1.city = p2.city AND p1.id < p2.id
  GROUP BY p1.city, p2.city
  ORDER BY p1.city;

SELECT remove_provenance('result_where');
SELECT * FROM result_where;
DROP TABLE result_where;

CREATE TABLE result_where AS
  SELECT city,
    regexp_replace(where_provenance(provenance()),':[0-9a-f-]*:','::','g')
  FROM (
    SELECT DISTINCT p1.city 
    FROM personnel p1, personnel p2 
    WHERE p2.city='Paris'
  ) t
  ORDER BY city;

SELECT remove_provenance('result_where');
SELECT * FROM result_where;
DROP TABLE result_where;

CREATE TABLE result_where AS
  SELECT 1,
  city,
  regexp_replace(where_provenance(provenance()),':[0-9a-f-]*:','::','g')
  FROM (
    SELECT city
    FROM personnel
  ) t
  ORDER BY city;

SELECT remove_provenance('result_where');
SELECT * FROM result_where;
DROP TABLE result_where;

-- A tracked ORDER BY ... LIMIT is a cut decided by each row's rank: a
-- comparison beside the row, which decides whether the row is there and
-- not where its values come from.  Each surviving value keeps the location
-- it was copied from.  (The cut keeps every row that is among the first two
-- in some world, hence more rows than in plain SQL.)
CREATE TABLE result_where AS
  SELECT name,
  regexp_replace(where_provenance(provenance()),':[0-9a-f-]*:','::','g')
  FROM personnel
  ORDER BY id
  LIMIT 2;
SELECT remove_provenance('result_where');
SELECT * FROM result_where ORDER BY name;
DROP TABLE result_where;

-- A group's key is copied from every row of the group; its aggregate is
-- computed, so it has no location.
CREATE TABLE result_where AS
  SELECT city, count(*) AS c,
  regexp_replace(where_provenance(provenance()),':[0-9a-f-]*:','::','g')
  FROM personnel
  GROUP BY city;
SELECT remove_provenance('result_where');
SELECT city, regexp_replace FROM result_where ORDER BY city;
DROP TABLE result_where;

-- A HAVING comparison stands for its group rather than filtering a row
-- beside it: not supported.
SELECT city, where_provenance(provenance())
  FROM personnel GROUP BY city HAVING count(*) > 2;
