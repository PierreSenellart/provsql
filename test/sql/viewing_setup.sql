\set ECHO none
SET search_path TO public, provsql;

/* Add descriptions to the personnel table */
ALTER TABLE personnel ADD COLUMN desc_str VARCHAR(255);
UPDATE personnel SET desc_str='personnel.'||name;

SELECT create_provenance_mapping('d', 'personnel', 'desc_str');

SELECT value FROM d ORDER BY value;
