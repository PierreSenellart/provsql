\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SELECT create_provenance_mapping('Personnel_Id','personnel','id');
SELECT value FROM Personnel_Id;
DROP TABLE Personnel_Id;

SELECT create_provenance_mapping('Personnel_Id','personnel','id','t');
SELECT value FROM "Personnel_Id";
DROP TABLE "Personnel_Id";
