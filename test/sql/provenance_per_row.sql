\set ECHO none
\pset format unaligned

-- provenance() in a query that aggregates is read where SQL evaluates it: in
-- WHERE, a GROUP BY key and the argument of an aggregate (its FILTER
-- included), once per input row, as the row's own provenance; elsewhere, once
-- per group.  The group's provenance used to be substituted everywhere: an
-- aggregate in a GROUP BY key ("Aggref found in non-Agg plan node"), and,
-- without GROUP BY, the certain token of the one row in place of each row's.
-- Two rows, each an input at probability 1/2.

CREATE TABLE ppr(id int, v int);
INSERT INTO ppr VALUES (1, 10), (2, 20);
SELECT add_provenance('ppr');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ppr; END $$;

-- A GROUP BY key reading the token: both rows are inputs, one group of 2.
CREATE TABLE ppr_r AS
  SELECT get_gate_type(provenance()) AS kind, count(*) AS n FROM ppr GROUP BY 1;
SELECT remove_provenance('ppr_r');
SELECT kind, n FROM ppr_r;
DROP TABLE ppr_r;

-- The same without an aggregate, and as a DISTINCT: the group is there when
-- either row is, 0.75 (it was one row's token, 0.5).
CREATE TABLE ppr_r AS
  SELECT get_gate_type(provenance()) AS kind,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ppr GROUP BY 1;
SELECT remove_provenance('ppr_r');
SELECT kind, p FROM ppr_r;
DROP TABLE ppr_r;
CREATE TABLE ppr_r AS
  SELECT DISTINCT get_gate_type(provenance()) AS kind FROM ppr;
SET provsql.active = off;
SELECT kind, round(probability_evaluate(provsql)::numeric, 4) AS p FROM ppr_r;
SET provsql.active = on;
DROP TABLE ppr_r;

-- GROUP BY provenance() is a key like any other: each row with a token of
-- its own is its own group, with its own token; and with an aggregate, each
-- group there with its row's probability, 0.5.
CREATE TABLE ppr_r AS
  SELECT provenance() AS tok FROM ppr GROUP BY 1;
SET provsql.active = off;
SELECT count(*) AS groups, bool_and(tok = provsql) AS own_token FROM ppr_r;
SET provsql.active = on;
DROP TABLE ppr_r;
CREATE TABLE ppr_r AS
  SELECT provenance() AS tok, count(*) AS n FROM ppr GROUP BY 1;
SET provsql.active = off;
SELECT count(*) AS groups,
       bool_and(round(probability_evaluate(provsql)::numeric, 4) = 0.5) AS p_half
FROM ppr_r;
SET provsql.active = on;
DROP TABLE ppr_r;

-- Two rows sharing a token (a tracked row joined with two untracked ones)
-- form one group, merged as by any other key: the same token as GROUP BY id.
CREATE TABLE ppr_u(id int);
INSERT INTO ppr_u VALUES (1), (1);
CREATE TABLE ppr_r AS
  SELECT 'prov' AS k, provenance() AS tok
  FROM (SELECT ppr.id FROM ppr JOIN ppr_u ON ppr.id = ppr_u.id) s
  GROUP BY provenance()
  UNION ALL
  SELECT 'id', NULL::uuid
  FROM (SELECT ppr.id FROM ppr JOIN ppr_u ON ppr.id = ppr_u.id) s
  GROUP BY id;
SET provsql.active = off;
SELECT count(*) AS rows, count(DISTINCT provsql) AS tokens FROM ppr_r;
SET provsql.active = on;
DROP TABLE ppr_r;
DROP TABLE ppr_u;

-- provenance() in the WHERE of a grouped query.
CREATE TABLE ppr_r AS
  SELECT id, count(*) AS n FROM ppr WHERE provenance() IS NOT NULL GROUP BY id;
SELECT remove_provenance('ppr_r');
SELECT id, n FROM ppr_r ORDER BY id;
DROP TABLE ppr_r;

-- In the argument of an aggregate, without GROUP BY: the rows' own tokens
-- (it was the certain token twice), and a FILTER on them counts both rows (it
-- counted none).
CREATE TABLE ppr_r AS
  SELECT array_agg(provenance() ORDER BY id) AS toks,
         count(*) FILTER (WHERE get_gate_type(provenance()) = 'input') AS inputs
  FROM ppr;
SELECT remove_provenance('ppr_r');
SET provsql.active = off;
SELECT toks::text::uuid[] = ARRAY(SELECT provsql FROM ppr ORDER BY id) AS rows_tokens,
       inputs::text AS inputs
FROM ppr_r;
SET provsql.active = on;
DROP TABLE ppr_r;

SELECT remove_provenance('ppr');
DROP TABLE ppr;
