\set ECHO none

SELECT city FROM personal GROUP BY GROUPING SETS ((), (city));
