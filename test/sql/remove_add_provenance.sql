\set ECHO none
\pset format unaligned

SELECT remove_provenance('personnel');
DROP TABLE personnel_name;

SELECT add_provenance('personnel');
SELECT create_provenance_mapping('personnel_name', 'personnel', 'name');
