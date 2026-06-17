\set ECHO none
\pset format unaligned

-- The makeDD route is now cost-selected by default ('auto', the empty/NULL
-- method, and 'default' all map to it); the named routes 'tree-decomposition'
-- and 'interpret-as-dd' each force a single construction. Whatever route is
-- chosen, the d-DNNF represents the same Boolean function, so Shapley and
-- Banzhaf values are identical across routes. This locks that equivalence
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
CREATE TABLE sa_auto AS SELECT variable, value FROM target_token, shapley_all_vars(prov, 'auto');
CREATE TABLE sa_td   AS SELECT variable, value FROM target_token, shapley_all_vars(prov, 'tree-decomposition');
CREATE TABLE sa_idd  AS SELECT variable, value FROM target_token, shapley_all_vars(prov, 'interpret-as-dd');
CREATE TABLE sa_def  AS SELECT variable, value FROM target_token, shapley_all_vars(prov);
CREATE TABLE ba_auto AS SELECT variable, value FROM target_token, banzhaf_all_vars(prov, 'auto');
CREATE TABLE ba_td   AS SELECT variable, value FROM target_token, banzhaf_all_vars(prov, 'tree-decomposition');

-- Shapley: auto vs the named routes vs the bare default must agree to rounding.
SELECT 'shapley' AS measure, count(*) AS mismatches
FROM sa_auto a JOIN sa_td t USING (variable) JOIN sa_idd i USING (variable) JOIN sa_def d USING (variable)
WHERE abs(a.value - t.value) > 1e-9 OR abs(a.value - i.value) > 1e-9 OR abs(a.value - d.value) > 1e-9;

-- Banzhaf: auto vs tree-decomposition agree.
SELECT 'banzhaf' AS measure, count(*) AS mismatches
FROM ba_auto a JOIN ba_td t USING (variable)
WHERE abs(a.value - t.value) > 1e-9;

DROP TABLE target_token, sa_auto, sa_td, sa_idd, sa_def, ba_auto, ba_td;
