\set ECHO none
\pset format unaligned

CREATE TABLE no_attribute AS SELECT FROM personnel;
SELECT remove_provenance('no_attribute');
SELECT * FROM no_attribute;
DROP TABLE no_attribute;
