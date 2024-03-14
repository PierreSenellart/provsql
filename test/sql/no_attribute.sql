\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE no_attribute AS SELECT FROM personnel;
SELECT remove_provenance('no_attribute');
SELECT * FROM no_attribute;
DROP TABLE no_attribute;
