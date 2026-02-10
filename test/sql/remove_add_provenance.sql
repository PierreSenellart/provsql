\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SELECT remove_provenance('personnel');
DROP TABLE personnel_name;

SELECT add_provenance('personnel');
SELECT create_provenance_mapping('personnel_name', 'personnel', 'name');
