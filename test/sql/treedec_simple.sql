\set ECHO none
SET search_path TO provsql_test,provsql;

CREATE TABLE treedec_simple_result AS
SELECT *, probability_evaluate(provenance(),'tree-decomposition') AS prob FROM personnel
ORDER BY id;

SELECT remove_provenance('treedec_simple_result');

SELECT id, ROUND(prob::numeric,1) AS prob FROM treedec_simple_result;
DROP TABLE treedec_simple_result;
