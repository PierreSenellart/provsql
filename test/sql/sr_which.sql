\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_which AS SELECT
  p1.city,
  sr_which(provenance(), 'personnel_name') AS which
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_which');
SELECT city, which
FROM result_which;

DROP TABLE result_which;

-- Round-trip: each leaf is already a which-provenance set; times in
-- Which is set union, so the join produces the union of leaf labels.
SELECT create_provenance_mapping('personnel_set',
  'personnel',
  $$'{' || name || '_x,' || name || '_y}'$$);

CREATE TABLE result_which_struct AS SELECT
  p1.city,
  sr_which(provenance(), 'personnel_set') AS which
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_which_struct');
SELECT city, which
FROM result_which_struct;

DROP TABLE result_which_struct;

-- ⊥ leaf input: times with ⊥ is absorbing, so any pair touching id=1
-- (John) collapses the New York group to ⊥. Berlin and Paris are
-- unaffected.
SELECT create_provenance_mapping('personnel_bot',
  'personnel',
  $$CASE WHEN id=1 THEN '⊥' ELSE name END$$);

CREATE TABLE result_which_bot AS SELECT
  p1.city,
  sr_which(provenance(), 'personnel_bot') AS which
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_which_bot');
SELECT city, which
FROM result_which_bot;

DROP TABLE result_which_bot;
