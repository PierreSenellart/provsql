\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- A 3-way times circuit (AND of three personnel inputs with probabilities
-- 0.5, 0.6, 0.7). Stable CNF shape under the Tseytin transformation.
CREATE TABLE tseytin_cnf_q AS
SELECT
  tseytin_cnf(provenance(), true)  AS cnf_weighted,
  tseytin_cnf(provenance(), false) AS cnf_unweighted
FROM personnel p1, personnel p2, personnel p3
WHERE p1.id=5 AND p2.id=6 AND p3.id=7;

SELECT remove_provenance('tseytin_cnf_q');

SELECT cnf_weighted FROM tseytin_cnf_q;
SELECT cnf_unweighted FROM tseytin_cnf_q;

DROP TABLE tseytin_cnf_q;
