\set ECHO none
SET search_path TO public, provsql;

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
