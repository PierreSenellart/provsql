\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- formula() does not support semimod gates created by HAVING;
-- this should produce a clear error message
SELECT city, COUNT(*), formula(provenance(), 'personnel_name')
FROM personnel
GROUP BY city
HAVING COUNT(*) > 2;
