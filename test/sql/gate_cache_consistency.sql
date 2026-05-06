\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Regression: get_gate_type used to populate the per-process circuit
-- cache with an empty children list, causing the next get_children to
-- silently return zero children. provenance_evaluate (PL/pgSQL) hits
-- exactly that pattern and used to fold custom-semiring plus / times
-- gates over an empty set.
--
-- The seed table is created by gate_cache_setup in a prior backend; we
-- run in a fresh session here, so the per-process cache starts empty.
SELECT get_gate_type(plus_token)::text AS gate_type FROM _ggt_seed;
SELECT array_length(get_children(plus_token), 1) AS n_children FROM _ggt_seed;

DROP TABLE _ggt_seed;
