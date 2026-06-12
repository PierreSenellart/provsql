\set ECHO none
\pset format unaligned

SELECT create_provenance_mapping('d', 'personnel', 'name');

SELECT value FROM d ORDER BY value;
