\set ECHO none
-- Endpoint mode: the in-extension KCMCP client compiles over a socket to a
-- warm server.  Exercised only when test/kcmcp/with-tdkc.sh has started one on
-- the fixed regress socket (`make test`, or the CI installcheck step); a bare
-- `make installcheck` finds no socket and the test skips (kcmcp_client_endpoint_1.out).
\if `test -S /tmp/.provsql-kcmcp-regress.sock && echo true || echo false`
\pset format unaligned
SET search_path TO provsql_test,provsql;

SELECT register_tool(name=>'tdkc-regress', kind=>'kcmcp',
  operations=>ARRAY['compile'], input_formats=>ARRAY['dimacs-cnf'],
  output_format=>'ddnnf-nnf', parser=>'nnf',
  endpoint=>'unix:/tmp/.provsql-kcmcp-regress.sock');

-- Compile each city's provenance over the socket and compare to an in-process
-- exact oracle (possible-worlds).  Equality validates the client's round-trip
-- and NNF parse-back; the probability value itself is checked by the d4 test.
CREATE TABLE kc_endpoint AS
SELECT city,
       probability_evaluate(provenance(),'compilation','tdkc-regress') AS via_kcmcp,
       probability_evaluate(provenance(),'possible-worlds')            AS via_ref
FROM (
  SELECT DISTINCT city FROM personnel
EXCEPT
  SELECT p1.city FROM personnel p1,personnel p2
  WHERE p1.id<p2.id AND p1.city=p2.city
  GROUP BY p1.city
) t;
SELECT remove_provenance('kc_endpoint');
SELECT city, (round(via_kcmcp::numeric,6) = round(via_ref::numeric,6)) AS matches
FROM kc_endpoint ORDER BY city;
DROP TABLE kc_endpoint;

SELECT unregister_tool('tdkc-regress');
\else
\echo 'SKIPPING: tdkc KCMCP server (endpoint mode) not available'
\endif
