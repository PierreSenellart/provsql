\set ECHO none
\pset format unaligned

SELECT create_provenance_mapping('Personnel_Id','personnel','id');
SELECT value FROM Personnel_Id;
DROP TABLE Personnel_Id;

SELECT create_provenance_mapping('Personnel_Id','personnel','id','t');
SELECT value FROM "Personnel_Id";
DROP TABLE "Personnel_Id";

-- Mapping used with sr_formula
SELECT create_provenance_mapping('personnel_name_map','personnel','name');

CREATE TABLE formula_result AS
SELECT name, city, sr_formula(provenance(), 'personnel_name_map') AS formula
FROM personnel WHERE city='Paris';

SELECT remove_provenance('formula_result');
SELECT * FROM formula_result ORDER BY name;

DROP TABLE formula_result;
DROP TABLE personnel_name_map;

-- Maintained mapping: a row inserted after the mapping is created is appended
-- automatically (keyed to its fresh input token), so it resolves like the
-- snapshot rows.
CREATE TABLE maint_src(id int, lbl text);
INSERT INTO maint_src VALUES (1, 'one');
SELECT add_provenance('maint_src');
SELECT create_provenance_mapping('maint_map', 'maint_src', 'lbl', maintained => true);
INSERT INTO maint_src(id, lbl) VALUES (2, 'two');
SELECT value FROM maint_map ORDER BY value;
DROP TABLE maint_map;
DROP TABLE maint_src;

-- NULL mapping values: a mapping row whose value is NULL contributes
-- no entry (the unmapped leaf falls back to the semiring's one());
-- it used to crash the backend (pfree of SPI_getvalue's NULL).
CREATE TABLE null_val(id int, lbl text);
INSERT INTO null_val VALUES (1, 'a'), (2, NULL);
SELECT add_provenance('null_val');
SELECT create_provenance_mapping('null_val_mapping', 'null_val', 'lbl');

CREATE TABLE null_val_result AS
SELECT id, sr_formula(provenance(), 'null_val_mapping') AS formula
FROM null_val;
SELECT remove_provenance('null_val_result');
SELECT * FROM null_val_result ORDER BY id;

DROP TABLE null_val_result;
DROP TABLE null_val_mapping;
DROP TABLE null_val;
