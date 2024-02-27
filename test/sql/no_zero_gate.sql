\set ECHO none
\pset format csv
SET search_path to provsql_test;

SELECT * FROM personnel EXCEPT SELECT * FROM personnel;

SELECT * FROM personnel EXCEPT ALL SELECT * FROM personnel;
