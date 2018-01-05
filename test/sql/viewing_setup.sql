\set ECHO none
SET search_path TO public, provsql;

/* Add some probabilities to the personal table */
ALTER TABLE personal ADD COLUMN desc_str VARCHAR(255);
UPDATE personal SET desc_str='personal.'||name;

SELECT create_provenance_mapping('d', 'personal', 'desc_str');

SELECT value FROM d ORDER BY value;
