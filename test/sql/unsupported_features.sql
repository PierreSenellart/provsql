\set ECHO none
\pset format unaligned

SELECT
    provenance ();

SELECT DISTINCT ON (city)
    city, count(*)
FROM
    personnel
GROUP BY city;

SELECT DISTINCT
    1
FROM
    personnel
GROUP BY
    city;

SELECT
    *
FROM
    personnel
INTERSECT ALL
SELECT
    *
FROM
    personnel;

-- DISTINCT on the value of an aggregate whose values are not read off its
-- contributions one by one (see explode_agg_value for the ones that are)
SELECT DISTINCT SUM(id) FROM personnel GROUP BY city;

-- EXCEPT on aggregate results: the rows it removes are matched on values that
-- are one per possible world
SELECT city, COUNT(*) FROM personnel GROUP BY city
EXCEPT
SELECT city, COUNT(*) FROM personnel WHERE city='Paris' GROUP BY city;

-- UNION (non-ALL) whose other arm aggregates nothing at that column
SELECT COUNT(*) FROM personnel GROUP BY city UNION SELECT 1;

-- GROUP BY on the value of an aggregate whose values are not read off its
-- contributions one by one (a count(), a min(), a max() and a choose() are
-- exploded into one row per value instead, see explode_agg_value)
SELECT total, COUNT(*) FROM (SELECT city, SUM(id) AS total FROM personnel GROUP BY city) t GROUP BY total;

-- Hand-made provsql column (collides with the auto-added provenance column)
SELECT name, provenance() AS provsql FROM personnel;

-- Hand-made provsql column from a plain expression
SELECT name AS provsql FROM personnel;

-- Hand-made provsql column from another uuid column
SELECT u AS provsql FROM (SELECT public.uuid_generate_v4() AS u, name FROM personnel) s;

-- Hand-made provsql column inside a set-operation arm
SELECT name, provenance() AS provsql FROM personnel
UNION ALL
SELECT name, provenance() AS provsql FROM personnel;

-- EXCEPT ALL over tracked relations, at the top level and nested
SELECT name FROM personnel EXCEPT ALL SELECT name FROM personnel;
SELECT name FROM (SELECT name FROM personnel EXCEPT ALL SELECT name FROM personnel) t;
WITH c AS (SELECT name FROM personnel EXCEPT ALL SELECT name FROM personnel) SELECT * FROM c;

-- A subquery over a tracked relation in a query level the rewriting does not
-- engage on (FROM-less, a LATERAL body computing an array) is evaluated on the
-- data as it is: a warning says its data is treated as certain.  Fetching a
-- token (SELECT provsql FROM ...) is not reading data.
SELECT 'yes' AS found WHERE EXISTS (SELECT * FROM personnel WHERE id = 1);
CREATE TABLE lateral_array AS
  SELECT p.id, cardinality(b.ids) AS n
  FROM (SELECT DISTINCT id FROM personnel WHERE id < 3) p
       LEFT JOIN LATERAL (SELECT ARRAY(SELECT q.id FROM personnel q
                                       WHERE q.id = p.id) AS ids) b ON true;
SELECT remove_provenance('lateral_array');
SELECT id, n FROM lateral_array ORDER BY id;
DROP TABLE lateral_array;
SELECT (SELECT provsql FROM personnel WHERE id = 1) IS NOT NULL AS token;

-- A condition on an aggregate result other than a comparison or IS [NOT]
-- NULL (x = ANY(array_agg(...))) is refused, in WHERE as in HAVING.
SELECT city FROM (SELECT city, array_agg(id) AS ids FROM personnel
                  GROUP BY city) t WHERE 3 = ANY(ids);
SELECT city FROM personnel GROUP BY city HAVING 3 = ANY(array_agg(id));
