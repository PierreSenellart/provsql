\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE "order" (x INT, y INT);
INSERT INTO "order" VALUES (1,1),(1,2),(2,2),(3,3);

SELECT add_provenance('order');
SELECT create_provenance_mapping('select', 'order', 'x', 't');
CREATE TABLE result AS
  SELECT formula(provenance(),'select') FROM (SELECT DISTINCT 1 FROM "order");
SELECT remove_provenance('result');
SELECT * FROM result;
DROP TABLE result;
CREATE TABLE result AS
  SELECT sr_formula(provenance(),'select') FROM (SELECT DISTINCT 1 FROM "order");
SELECT remove_provenance('result');
SELECT * FROM result;
DROP TABLE result;

SELECT remove_provenance('order');
SELECT repair_key('order', 'x');
DO $$ BEGIN
  PERFORM set_prob(provenance(), (x+y)*.1) FROM "order";
END $$;
CREATE TABLE result AS
  SELECT x,probability_evaluate(provenance()) FROM (SELECT DISTINCT x FROM "order") ORDER BY x;
SELECT remove_provenance('result');
SELECT * FROM result;
DROP TABLE result;

DROP TABLE "order";
DROP TABLE "select";
