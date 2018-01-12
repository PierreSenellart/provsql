\set ECHO none
SET search_path TO public,provsql;

SELECT provenance_eq('19257535-6aaf-5275-b02b-899c48576553',1,2);
SELECT * from provenance_circuit_extra;

