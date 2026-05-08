\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_boolexpr AS SELECT
  p1.city,
  sr_boolexpr(provenance()) AS boolexpr,
  sr_boolexpr(provenance(), 'personnel_name') AS boolexpr_named
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city;

SELECT remove_provenance('result_boolexpr');
SELECT
  city,
  regexp_replace(boolexpr, 'x[0-9]', 'x', 'g'),
  boolexpr_named
FROM result_boolexpr
ORDER BY city;

-- NULL token short-circuits, regardless of whether a mapping is given.
SELECT sr_boolexpr(NULL::uuid) IS NULL AS null_no_mapping,
       sr_boolexpr(NULL::uuid, 'personnel_name') IS NULL AS null_with_mapping;

DROP TABLE result_boolexpr;
