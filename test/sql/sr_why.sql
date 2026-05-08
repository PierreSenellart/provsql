\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_why AS SELECT
  p1.city,
  sr_why(provenance(), 'personnel_name') AS why
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_why');
SELECT city, why
FROM result_why;

DROP TABLE result_why;

-- Round-trip: leaves are already structured why-provenance values
-- (each personnel.name maps to a 2-witness set-of-sets). Times in
-- Why is the cartesian product of witness sets, so the join produces
-- 4 combined witnesses per cross-pair.
SELECT create_provenance_mapping('personnel_witnesses',
  'personnel',
  $$'{{' || name || '_a},{' || name || '_b}}'$$);

CREATE TABLE result_why_struct AS SELECT
  p1.city,
  sr_why(provenance(), 'personnel_witnesses') AS why
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_why_struct');
SELECT city, why
FROM result_why_struct;

DROP TABLE result_why_struct;
