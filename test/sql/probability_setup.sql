\set ECHO none
SET search_path TO public, provsql;

/* Add some probabilities to the personal table */
ALTER TABLE personal ADD COLUMN probability DOUBLE PRECISION;
UPDATE personal SET probability=id*1./10;

SELECT create_provenance_mapping('p', 'personal', 'probability');

SELECT value FROM p ORDER BY value;
