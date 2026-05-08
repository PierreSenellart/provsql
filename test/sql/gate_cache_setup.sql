\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Stash a plus-gate UUID into a regular table so a fresh-backend test
-- (gate_cache_consistency) can call get_gate_type / get_children
-- against it with an empty per-process circuit cache. SELECT DISTINCT
-- over a 2-row class collapses to one plus over 2 inputs (Paul + Nancy
-- in personnel : both restricted).
DROP TABLE IF EXISTS _ggt_seed;
CREATE TABLE _ggt_seed AS
  SELECT classification::text AS classification,
         provenance()         AS plus_token
  FROM (SELECT DISTINCT classification FROM personnel WHERE classification = 'restricted') t;
SELECT remove_provenance('_ggt_seed');

-- Sanity-check the seed: exactly one plus over two inputs.
SELECT get_gate_type(plus_token)::text AS gate_type,
       array_length(get_children(plus_token), 1) AS n_children
FROM _ggt_seed;
