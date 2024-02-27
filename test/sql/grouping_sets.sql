\set ECHO none
\pset format csv
SET search_path to provsql_test;

SELECT city FROM personnel GROUP BY GROUPING SETS ((), (city));
