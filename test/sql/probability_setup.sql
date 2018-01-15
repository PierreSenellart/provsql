\set ECHO none
SET search_path TO public, provsql;

/* Add some probabilities to the personnel table */
ALTER TABLE personnel ADD COLUMN probability DOUBLE PRECISION;
UPDATE personnel SET probability=id*1./10;

SELECT create_provenance_mapping('p', 'personnel', 'probability');

SELECT value FROM p ORDER BY value;
