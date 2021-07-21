\set ECHO none
SET search_path to provsql_test;

SELECT * FROM personnel EXCEPT SELECT * FROM personnel;

SELECT * FROM personnel EXCEPT ALL SELECT * FROM personnel;
