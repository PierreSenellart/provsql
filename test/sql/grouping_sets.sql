\set ECHO none

SELECT city FROM personnel GROUP BY GROUPING SETS ((), (city));
