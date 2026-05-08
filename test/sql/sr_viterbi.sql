\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SELECT create_provenance_mapping('personnel_prob', 'personnel', '(0.5^id)::float');
CREATE TABLE result_viterbi AS SELECT
  p1.city,
  round(sr_viterbi(provenance(),'personnel_prob')::numeric, 6) AS prob
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_viterbi');
SELECT * FROM result_viterbi;

DROP TABLE result_viterbi;
