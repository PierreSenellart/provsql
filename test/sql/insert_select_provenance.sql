\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- INSERT ... SELECT propagates provenance
CREATE TABLE insert_prov_result (name varchar, city varchar);
SELECT add_provenance('insert_prov_result');
INSERT INTO insert_prov_result SELECT name, city FROM personnel WHERE city='Paris';

CREATE TABLE insert_prov_eval AS
  SELECT name, city, sr_formula(provenance(), 'personnel_name') AS formula
  FROM insert_prov_result;
SELECT remove_provenance('insert_prov_eval');
SELECT * FROM insert_prov_eval ORDER BY name;

DROP TABLE insert_prov_eval;
DROP TABLE insert_prov_result;

-- INSERT ... SELECT with join propagates combined provenance
CREATE TABLE insert_prov_join (name1 varchar, name2 varchar);
SELECT add_provenance('insert_prov_join');
INSERT INTO insert_prov_join
  SELECT p1.name, p2.name FROM personnel p1 JOIN personnel p2
  ON p1.city = p2.city AND p1.id < p2.id WHERE p1.city='Paris';

CREATE TABLE insert_prov_join_eval AS
  SELECT name1, name2, sr_formula(provenance(), 'personnel_name') AS formula
  FROM insert_prov_join;
SELECT remove_provenance('insert_prov_join_eval');
SELECT * FROM insert_prov_join_eval ORDER BY name1, name2;

DROP TABLE insert_prov_join_eval;
DROP TABLE insert_prov_join;

-- INSERT ... SELECT into table without provsql column warns
CREATE TABLE insert_no_prov (name varchar, city varchar);
INSERT INTO insert_no_prov SELECT name, city FROM personnel WHERE city='Paris';
DROP TABLE insert_no_prov;
