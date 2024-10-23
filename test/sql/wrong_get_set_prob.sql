\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE wrong_get AS SELECT get_prob(provenance()) FROM (SELECT DISTINCT city FROM personnel) t;
SELECT remove_provenance('wrong_get');
SELECT * FROM wrong_get;
SELECT set_prob(provenance(), 1.) FROM (SELECT DISTINCT city FROM personnel) t;
