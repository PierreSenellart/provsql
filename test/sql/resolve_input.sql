\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Known input: round-trip via the row's provsql column.
CREATE TABLE ri_q AS
  SELECT provsql AS u FROM personnel WHERE name='John';
SELECT remove_provenance('ri_q');
SELECT relation, row_data->>'name' AS name
FROM ri_q, LATERAL provsql.resolve_input(u);
DROP TABLE ri_q;

-- Every personnel row resolves back to itself.  Capture provsql values into a
-- non-tracked table first so the LATERAL call doesn't run under the rewriter
-- (which rejects FROM functions with multiple output attributes).
CREATE TABLE ri_all AS SELECT provsql AS u FROM personnel;
SELECT remove_provenance('ri_all');
SELECT row_data->>'name' AS name
FROM ri_all,
     LATERAL provsql.resolve_input(u)
ORDER BY name;
DROP TABLE ri_all;

-- Unknown UUID: zero rows, no error.
SELECT count(*) AS n
FROM provsql.resolve_input('00000000-0000-0000-0000-000000000000'::UUID);

-- Non-input gate (a plus produced by SELECT DISTINCT): zero rows.
CREATE TABLE ri_q AS
  SELECT provenance() AS u FROM (SELECT DISTINCT 1 FROM personnel) t;
SELECT remove_provenance('ri_q');
SELECT count(*) AS n
FROM ri_q, LATERAL provsql.resolve_input(u);
DROP TABLE ri_q;
