\set ECHO none

SELECT * FROM personnel EXCEPT SELECT * FROM personnel;

SELECT * FROM personnel EXCEPT ALL SELECT * FROM personnel;
