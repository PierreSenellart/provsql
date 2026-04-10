\set ECHO none
\if `perl -MGraph::Easy -e1 2>/dev/null && echo true || echo false`
\pset format unaligned
SET search_path TO provsql_test,provsql;

SET provsql.where_provenance = on;

CREATE TABLE vc_result AS
SELECT city, view_circuit(provenance(),'d') AS ok
FROM ( 
  SELECT DISTINCT p1.city FROM
    personnel p1, personnel p2
  WHERE
    p1.id=1 AND
    p2.id=1
) t;

SELECT remove_provenance('vc_result');

SELECT city, ok FROM vc_result ORDER BY city;
DROP TABLE vc_result;
\else
\echo 'SKIPPING: graph-easy not available'
\endif
