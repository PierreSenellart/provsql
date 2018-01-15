\set ECHO none
SET search_path TO public, provsql;

SELECT create_provenance_mapping('d', 'personnel', 'name');

SELECT value FROM d ORDER BY value;
