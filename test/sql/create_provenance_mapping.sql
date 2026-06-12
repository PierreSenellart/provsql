\set ECHO none
\pset format unaligned

SELECT create_provenance_mapping('Personnel_Id','personnel','id');
SELECT value FROM Personnel_Id;
DROP TABLE Personnel_Id;

SELECT create_provenance_mapping('Personnel_Id','personnel','id','t');
SELECT value FROM "Personnel_Id";
DROP TABLE "Personnel_Id";

-- View-based mapping
SELECT create_provenance_mapping_view('personnel_id_view','personnel','id');

CREATE TABLE view_result AS SELECT value FROM personnel_id_view;
SELECT remove_provenance('view_result');
SELECT * FROM view_result ORDER BY value;
DROP TABLE view_result;
DROP VIEW personnel_id_view;

SELECT create_provenance_mapping_view('Personnel_Id_View','personnel','id','t');

CREATE TABLE view_result2 AS SELECT value FROM "Personnel_Id_View";
SELECT remove_provenance('view_result2');
SELECT * FROM view_result2 ORDER BY value;
DROP TABLE view_result2;
DROP VIEW "Personnel_Id_View";

-- View-based mapping used with sr_formula
SELECT create_provenance_mapping_view('personnel_name_view','personnel','name');

CREATE TABLE view_formula_result AS
SELECT name, city, sr_formula(provenance(), 'personnel_name_view') AS formula
FROM personnel WHERE city='Paris';

SELECT remove_provenance('view_formula_result');
SELECT * FROM view_formula_result ORDER BY name;

DROP TABLE view_formula_result;
DROP VIEW personnel_name_view;

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
