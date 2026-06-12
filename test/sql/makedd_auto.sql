\set ECHO none
\pset format unaligned

-- The makeDD route is now cost-selected by default ('auto', the empty/NULL
-- method, and 'default' all map to it); the old fixed interpret-as-dd ->
-- tree-decomposition -> compiler chain stays reachable as 'ladder'. Whatever
-- route is chosen, the d-DNNF represents the same Boolean function, so Shapley
-- and Banzhaf values are identical across routes. This locks that equivalence
-- (a route returning wrong values would surface as mismatches).

DO $$ BEGIN PERFORM set_prob(provenance(), 0.4) FROM personnel; END $$;

-- A single target token: the provenance of "is there any personnel row", a
-- plus-gate over all input tuples (so several input variables, one token).
CREATE TABLE target_token AS
SELECT provenance() AS prov
FROM (SELECT DISTINCT 1 AS one FROM personnel) x;
SELECT remove_provenance('target_token');

-- Materialise each route's per-variable values (the CS2 §15 pattern). The
-- shapley_all_vars output is not provenance-tracked, so these tables carry no
-- provsql column.
CREATE TABLE sa_auto   AS SELECT variable, value FROM target_token, shapley_all_vars(prov, 'auto');
CREATE TABLE sa_ladder AS SELECT variable, value FROM target_token, shapley_all_vars(prov, 'ladder');
CREATE TABLE sa_def    AS SELECT variable, value FROM target_token, shapley_all_vars(prov);
CREATE TABLE ba_auto   AS SELECT variable, value FROM target_token, banzhaf_all_vars(prov, 'auto');
CREATE TABLE ba_ladder AS SELECT variable, value FROM target_token, banzhaf_all_vars(prov, 'ladder');

-- Shapley: auto vs ladder vs the bare default must agree to within rounding.
SELECT 'shapley' AS measure, count(*) AS mismatches
FROM sa_auto a JOIN sa_ladder l USING (variable) JOIN sa_def d USING (variable)
WHERE abs(a.value - l.value) > 1e-9 OR abs(a.value - d.value) > 1e-9;

-- Banzhaf: auto vs ladder agree.
SELECT 'banzhaf' AS measure, count(*) AS mismatches
FROM ba_auto a JOIN ba_ladder l USING (variable)
WHERE abs(a.value - l.value) > 1e-9;

DROP TABLE target_token, sa_auto, sa_ladder, sa_def, ba_auto, ba_ladder;
