\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE identify_result AS
SELECT identify_token(provenance())::text FROM personnel;

SELECT remove_provenance('identify_result');
SELECT * FROM identify_result;
DROP TABLE identify_result;
