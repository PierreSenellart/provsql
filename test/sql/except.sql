\set ECHO none
SET search_path TO public, provsql;

CREATE TABLE result_formula AS SELECT
  *, formula(provenance(),'personal_name') FROM (
    (SELECT DISTINCT city FROM personal)
  EXCEPT
    (SELECT p1.city                               
     FROM personal p1, personal p2
     WHERE p1.city = p2.city AND p1.id < p2.id
     GROUP BY p1.city
     ORDER BY p1.city)
  ) t;

SELECT remove_provenance('result_formula');
SELECT * FROM result_formula;

DROP TABLE result_formula;
