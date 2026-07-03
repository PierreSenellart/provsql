\set ECHO none
\pset format unaligned

-- A searched CASE whose guards are aggregate comparisons and whose branches are
-- aggregates lowers to an agg_case gate_case (the aggregate-carrier analogue of
-- the RV CASE).  Its moments are evaluated EXACTLY by possible-worlds
-- decomposition -- E[pick^k] = Σ_i P(region_i)·E[value_i^k | region_i] over the
-- first-match regions -- so every assertion below runs under rv_mc_samples = 0
-- (no Monte Carlo).  Correlation between a guard and its branch (shared input
-- tuples) is carried by the conditioning, exactly as HAVING carries it.

SET provsql.rv_mc_samples = 0;

-- Two independent tuples, each present with probability 0.5.
CREATE TABLE cs(g int, x numeric, y numeric, z numeric);
INSERT INTO cs VALUES (1,1,10,100),(1,5,20,200);
SELECT add_provenance('cs');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM cs; END $$;

-- CASE WHEN sum(x) > 3 THEN sum(y) ELSE sum(z).  Possible worlds:
--   {}      sum(x)=0  -> sum(z)=0
--   {t1}    sum(x)=1  -> sum(z)=100
--   {t2}    sum(x)=5  -> sum(y)=20
--   {t1,t2} sum(x)=6  -> sum(y)=30
-- each world 0.25  =>  E=37.5, E[pick^2]=2825, Var=1418.75.
CREATE TABLE pick AS
  SELECT g, CASE WHEN sum(x) > 3 THEN sum(y) ELSE sum(z) END AS p FROM cs GROUP BY g;

SET provsql.active = off;
-- The branch lowered to a gate_case carried by an agg_token.
SELECT get_gate_type(p::uuid) AS root_gate FROM pick;
-- The token's cell carries the actual-world CASE value -- both tuples are
-- present in the actual data, so sum(x)=6 > 3 selects sum(y)=30 -- and
-- agg_token_value_text resolves the same display from the bare UUID.
SELECT p AS display FROM pick;
SELECT agg_token_value_text(p::uuid) AS display_from_uuid FROM pick;
SELECT round(expected(p)::numeric,4)  AS e_pick,
       round(variance(p)::numeric,4)  AS var_pick,
       round(moment(p,2)::numeric,4)  AS m2_pick
FROM pick;
SET provsql.active = on;
DROP TABLE pick; DROP TABLE cs;

-- Certain tuples (probability 1): sum(x)=6 > 3 selects sum(y)=30 deterministically.
CREATE TABLE cd(g int, x numeric, y numeric, z numeric);
INSERT INTO cd VALUES (1,1,10,100),(1,5,20,200);
SELECT add_provenance('cd');
DO $$ BEGIN PERFORM set_prob(provenance(), 1.0) FROM cd; END $$;
CREATE TABLE pickd AS
  SELECT g, CASE WHEN sum(x) > 3 THEN sum(y) ELSE sum(z) END AS p FROM cd GROUP BY g;
SET provsql.active = off;
SELECT round(expected(p)::numeric,4) AS e_certain,
       round(variance(p)::numeric,4) AS var_certain
FROM pickd;
SET provsql.active = on;
DROP TABLE pickd; DROP TABLE cd;

-- MIN / MAX branches (a two-way max/min switch on the aggregate).  Three
-- tuples, one certain so the selected branch is never an empty group.
CREATE TABLE cm(g int, x numeric, y numeric);
INSERT INTO cm VALUES (1,1,10),(1,5,20),(1,3,7);
SELECT add_provenance('cm');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.6) FROM cm WHERE x IN (1,5); END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 1.0) FROM cm WHERE x = 3; END $$;
CREATE TABLE pickm AS
  SELECT g, CASE WHEN sum(x) > 6 THEN max(y) ELSE min(y) END AS p FROM cm GROUP BY g;
SET provsql.active = off;
-- Actual world: sum(x)=9 > 6 selects max(y)=20.
SELECT p AS display_minmax FROM pickm;
SELECT round(expected(p)::numeric,4) AS e_minmax,
       round(variance(p)::numeric,4) AS var_minmax
FROM pickm;
SET provsql.active = on;
DROP TABLE pickm; DROP TABLE cm;

-- Constant branch (`ELSE 0`): lifted into a value gate, so the branch is a
-- Dirac and its conditional moment is exact.  Worlds (t1,t2 each 0.5):
--   {}->0, {t1}->0, {t2}->sum(y)=20, {t1,t2}->sum(y)=30  => E=12.5, Var=168.75.
CREATE TABLE cc(g int, x numeric, y numeric);
INSERT INTO cc VALUES (1,1,10),(1,5,20);
SELECT add_provenance('cc');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM cc; END $$;
CREATE TABLE pickc AS
  SELECT g, CASE WHEN sum(x) > 3 THEN sum(y) ELSE 0 END AS p FROM cc GROUP BY g;
-- Actual-world guard false: the default (a value gate) is displayed.
CREATE TABLE pickc2 AS
  SELECT g, CASE WHEN sum(x) > 100 THEN sum(y) ELSE 0 END AS p FROM cc GROUP BY g;
SET provsql.active = off;
SELECT round(expected(p)::numeric,4) AS e_const,
       round(variance(p)::numeric,4) AS var_const
FROM pickc;
SELECT p AS display_default FROM pickc2;
SET provsql.active = on;
DROP TABLE pickc; DROP TABLE pickc2; DROP TABLE cc;

-- Arithmetic branch (`sum(y)+sum(z)`): no exact possible-worlds moment for the
-- arithmetic combination, so that branch's conditional moment is estimated by
-- the Monte-Carlo scalar path (the region probabilities stay exact).  Worlds:
--   {}->0, {t1}->sum(z)=100, {t2}->sum(y)+sum(z)=220, {t1,t2}->330  => E=162.5.
CREATE TABLE ca(g int, x numeric, y numeric, z numeric);
INSERT INTO ca VALUES (1,1,10,100),(1,5,20,200);
SELECT add_provenance('ca');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ca; END $$;
CREATE TABLE picka AS
  SELECT g, CASE WHEN sum(x) > 3 THEN sum(y)+sum(z) ELSE sum(z) END AS p FROM ca GROUP BY g;
SET provsql.rv_mc_samples = 500000;
SET provsql.monte_carlo_seed = 1;
SET provsql.active = off;
-- Actual world: sum(x)=6 > 3 selects sum(y)+sum(z) = 330 (an arith gate,
-- whose actual-world value agg_arith_make recorded in extra).
SELECT p AS display_arith FROM picka;
SELECT abs(expected(p) - 162.5) < 5 AS arith_branch_mc_close FROM picka;
SET provsql.active = on;
DROP TABLE picka; DROP TABLE ca;

RESET provsql.rv_mc_samples;
RESET provsql.monte_carlo_seed;

SELECT 'ok'::text AS agg_case_done;
