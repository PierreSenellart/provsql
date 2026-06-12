\set ECHO none
-- Managed mode: ProvSQL's supervisor worker launches/owns the KCMCP server
-- (provsql.kcmcp_server) and publishes its endpoint in shared memory; a tool
-- with endpoint 'managed' resolves to it.  Exercised only when
-- test/kcmcp/with-tdkc.sh has staged a postgres-reachable tdkc binary; a bare
-- `make installcheck` finds none and the test skips (kcmcp_client_managed_1.out).
--
-- This transiently sets and RESETs provsql.kcmcp_server on the running
-- cluster, so do not run `make test` against a cluster relying on managed mode
-- in production.
\if `test -x /tmp/tdkc-regress && echo true || echo false`
\pset format unaligned

-- Configure the managed server and let the supervisor worker launch it.
ALTER SYSTEM SET provsql.kcmcp_server = '/tmp/tdkc-regress --kcmcp {endpoint}';
SELECT pg_reload_conf();
SELECT pg_sleep(1.5);

SELECT register_tool(name=>'tdkc-managed', kind=>'kcmcp',
  operations=>ARRAY['compile'], input_formats=>ARRAY['dimacs-cnf'],
  output_format=>'ddnnf-nnf', parser=>'nnf', endpoint=>'managed');

CREATE TABLE kc_managed AS
SELECT city,
       probability_evaluate(provenance(),'compilation','tdkc-managed') AS via_kcmcp,
       probability_evaluate(provenance(),'possible-worlds')            AS via_ref
FROM (
  SELECT DISTINCT city FROM personnel
EXCEPT
  SELECT p1.city FROM personnel p1,personnel p2
  WHERE p1.id<p2.id AND p1.city=p2.city
  GROUP BY p1.city
) t;
SELECT remove_provenance('kc_managed');
SELECT city, (round(via_kcmcp::numeric,6) = round(via_ref::numeric,6)) AS matches
FROM kc_managed ORDER BY city;
DROP TABLE kc_managed;

SELECT unregister_tool('tdkc-managed');
ALTER SYSTEM RESET provsql.kcmcp_server;
SELECT pg_reload_conf();
\else
\echo 'SKIPPING: managed KCMCP server not available'
\endif
