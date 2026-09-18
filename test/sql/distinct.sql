\set ECHO none
\pset format unaligned

CREATE TABLE distinct_result AS
  SELECT *, sr_formula(provenance(),'personnel_name') AS formula
  FROM (
    SELECT DISTINCT classification FROM personnel
  ) t;

SELECT remove_provenance('distinct_result');
SELECT classification,replace(formula,'Paul ⊕ Nancy','Nancy ⊕ Paul') AS formula FROM distinct_result ORDER BY classification;
DROP TABLE distinct_result;

CREATE TABLE distinct_result AS
  SELECT COUNT(DISTINCT name)
  FROM personnel
  GROUP BY city;
SELECT remove_provenance('distinct_result');
SELECT * FROM distinct_result ORDER BY count::numeric;
DROP TABLE distinct_result;

-- DISTINCT over a window value: the window is computed in a subquery, the
-- DISTINCT over it (a window value cannot be a grouping key).
CREATE TABLE distinct_result AS
  SELECT DISTINCT city, first_value(name) OVER (PARTITION BY city ORDER BY id
    ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS first
  FROM personnel;
SELECT remove_provenance('distinct_result');
SELECT city, first FROM distinct_result ORDER BY city;
DROP TABLE distinct_result;
