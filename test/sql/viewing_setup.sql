\set ECHO none
\pset format csv
SET search_path TO provsql_test,provsql;

SELECT create_provenance_mapping('d', 'personnel', 'name');

SELECT value FROM d ORDER BY value;
