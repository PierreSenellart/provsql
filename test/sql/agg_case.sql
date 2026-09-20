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
--   {}      no row of the group: the CASE has no value
--   {t1}    sum(x)=1  -> sum(z)=100
--   {t2}    sum(x)=5  -> sum(y)=20
--   {t1,t2} sum(x)=6  -> sum(y)=30
-- each world 0.25, so the three defined ones weigh 1/3 each:
-- E=50, E[pick^2]=3766.67, Var=1266.67.
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
--   {} has no row, {t1}->sum(z)=100, {t2}->sum(y)+sum(z)=220, {t1,t2}->330,
--   so E = (100+220+330)/3 = 216.67.
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
SELECT abs(expected(p) - 216.67) < 5 AS arith_branch_mc_close FROM picka;
SET provsql.active = on;
DROP TABLE picka; DROP TABLE ca;

RESET provsql.rv_mc_samples;
RESET provsql.monte_carlo_seed;

-- Conditional-on-defined semantics: the moment of a CASE conditions on
-- its value being DEFINED (the MIN/MAX convention), NULL only when it
-- never is.  Rows 10 and 100 each present with probability 1/2:
--   {10,100} sum=110 >= 100 -> sum 110 ; {100} -> sum 100 ;
--   {10} -> min 10 ; {} -> min over nothing: undefined (excluded,
--   the defined mass renormalises).
-- E[pick | defined]   = (110+100+10)/4 / (3/4) = 220/3  = 73.3333...
-- Var[pick | defined] = 22200/3 - (220/3)^2 = 18200/9   = 2022.2222...
CREATE TABLE cd2(g int, x numeric);
INSERT INTO cd2 VALUES (1, 10), (1, 100);
SELECT add_provenance('cd2');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM cd2; END $$;
SET provsql.rv_mc_samples = 0;
CREATE TABLE pickd2 AS
  SELECT g, CASE WHEN sum(x) >= 100 THEN sum(x) ELSE min(x) END AS p
  FROM cd2 GROUP BY g;
SET provsql.active = off;
SELECT round(expected(p)::numeric, 4) AS e_cond_defined,
       round(variance(p)::numeric, 4) AS var_cond_defined
FROM pickd2;
-- Conditioning on both rows absent: the CASE's value is never defined.
SELECT expected(p,
         (SELECT provenance_times(a.nt, b.nt)
            FROM (SELECT provenance_not(provsql) AS nt FROM cd2 WHERE x = 10) a,
                 (SELECT provenance_not(provsql) AS nt FROM cd2 WHERE x = 100) b))
       IS NULL AS never_defined_null
FROM pickd2;
SET provsql.active = on;
DROP TABLE pickd2; DROP TABLE cd2;
RESET provsql.rv_mc_samples;

-- A simple-form CASE (CASE <arg> WHEN ...) over aggregates is not a
-- searched guarded selection, so the agg_case lowering leaves it alone.
-- The branches must then degrade through the agg_token cast back to the
-- CASE's numeric type (their actual-world values, provenance dropped
-- with the usual warning) -- never bare agg_token datums under a numeric
-- CASE type, which would be reinterpreted as a garbage varlena and
-- corrupt (or crash on) the materialised tuple.  The same degradation
-- protects searched CASEs on a schema whose upgrade path predates
-- agg_case.
CREATE TABLE cf(g int, x numeric);
INSERT INTO cf VALUES (1, 10), (1, 100);
SELECT add_provenance('cf');
CREATE TABLE pickf AS
  SELECT g, CASE g WHEN 1 THEN sum(x) ELSE min(x) END AS p FROM cf GROUP BY g;
SET provsql.active = off;
SELECT g, p, pg_typeof(p) AS p_type FROM pickf;
SET provsql.active = on;
DROP TABLE pickf; DROP TABLE cf;

-- A branch whose value is NULL -- an aggregate over the padded rows of an
-- outer join only, a NULL constant -- is NULL in every world; a CASE of a
-- type other than a number (a timestamp) is evaluated as plain SQL.
CREATE TABLE cn_t(tag text, e int, w int);
CREATE TABLE cn_p(id int, score int, d date);
INSERT INTO cn_t VALUES ('a', 1, 2), ('b', 3, NULL), ('c', NULL, 4);
INSERT INTO cn_p VALUES (1, 10, '2020-01-01'), (2, 20, '2021-01-01'),
                        (3, 30, '2019-01-01');
SELECT add_provenance('cn_t');
SELECT add_provenance('cn_p');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM cn_p; END $$;
CREATE TABLE cn_r AS
  SELECT t.tag,
         CASE WHEN max(e.score) > max(w.score) THEN max(e.score)
              ELSE max(w.score) END AS s,
         round(expected(CASE WHEN max(e.score) > max(w.score) THEN max(e.score)
                             ELSE max(w.score) END)::numeric, 4) AS e_s,
         CASE WHEN max(e.d) > max(w.d) THEN max(e.d) ELSE max(w.d) END AS d
  FROM cn_t t LEFT JOIN cn_p e ON e.id = t.e LEFT JOIN cn_p w ON w.id = t.w
  GROUP BY t.tag;
SELECT remove_provenance('cn_r');
SELECT tag, s::text AS s, e_s, d FROM cn_r ORDER BY tag;
DROP TABLE cn_r;
CREATE TABLE cn_r AS
  SELECT t.tag, CASE WHEN max(e.score) > 15 THEN max(e.score) END AS s
  FROM cn_t t JOIN cn_p e ON e.id = t.e GROUP BY t.tag;
SELECT remove_provenance('cn_r');
SELECT tag, s::text AS s FROM cn_r ORDER BY tag;
DROP TABLE cn_r, cn_t, cn_p;


-- COALESCE(aggregate, constant) is the CASE it means, CASE WHEN agg IS NOT
-- NULL THEN agg ELSE constant END, so it lowers to the same gate: its guard is
-- the NullTest lowering, delta(+Kn), a row the aggregate reads a value from is
-- present.  Over the two rows of the first group, each present with
-- probability one half, sum(v) is NULL in the world holding only the
-- NULL-valued row, so coalesce(sum(v), 0) is 0 there and 5 in the two worlds
-- holding the other row: E = (0 + 5 + 5)/3 = 3.333333 and the variance is
-- 50/3 - 100/9 = 5.555556, both conditional on the group existing as every
-- moment is.  Both rows of the second group are NULL-valued, so the default
-- answers in every world the group exists in; the third group is certain of
-- its single row.  A default needs not be constant: an expression of the row
-- that holds no aggregate -- the grouping key here -- is the same in every
-- world, so it is lifted into a value gate like a constant.  A third argument
-- is left as the query wrote it and read as a plain value.
--
-- GREATEST and LEAST of an aggregate and such an expression are the CASE they
-- mean as well, and SQL's reading of a NULL argument as "no value" is what
-- their two NULL guards carry: GREATEST(NULL, 10) is 10, where a plain
-- "CASE WHEN a > b" would fall to its ELSE and answer NULL.  Over the first
-- group, whose two rows are one null-valued and one of 5, the sum is NULL in
-- the world holding only the first (one world in three where the group exists)
-- and 5 in the other two, so GREATEST(sum(v), 2) takes 2 and 5, E = 4, and
-- LEAST(sum(v), 2) is 2 in every one of them.  The third group is certain of
-- its 7.
CREATE TABLE cc(g int, v int);
INSERT INTO cc VALUES (1,NULL),(1,5),(2,NULL),(2,NULL),(3,7);
SELECT add_provenance('cc');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM cc; END $$;
CREATE TABLE cc_r AS
  SELECT g, coalesce(sum(v), 0) AS s0, coalesce(max(v), -1) AS mx,
         coalesce(count(*), 0) AS cnt, coalesce(sum(v), g) AS dflt_var,
         coalesce(sum(v), NULL, 0) AS three
  FROM cc GROUP BY g;
SET provsql.active = off;
SELECT g, s0::text AS s0, round(expected(s0, provsql)::numeric, 6) AS e_s0,
       round(variance(s0, provsql)::numeric, 6) AS var_s0,
       mx::text AS mx, round(expected(mx, provsql)::numeric, 6) AS e_mx,
       cnt::text AS cnt, dflt_var::text AS dflt_var, three::text AS three,
       round(probability(provsql)::numeric, 6) AS p
FROM cc_r ORDER BY g;
SET provsql.active = on;
DROP TABLE cc_r;
CREATE TABLE cc_r AS
  SELECT g, GREATEST(sum(v), 2) AS gt, LEAST(sum(v), 2) AS ls FROM cc GROUP BY g;
SET provsql.active = off;
SELECT g, gt::text AS gt, round(expected(gt, provsql)::numeric, 6) AS e_gt,
       ls::text AS ls, round(expected(ls, provsql)::numeric, 6) AS e_ls
FROM cc_r ORDER BY g;
SET provsql.active = on;
DROP TABLE cc_r;
-- The NULL argument: the second group has no value in any world, so both are
-- the other argument, as SQL says.
CREATE TABLE cc_r AS
  SELECT GREATEST(sum(v), 2) AS gt, LEAST(sum(v), 2) AS ls FROM cc WHERE g = 2;
SET provsql.active = off;
SELECT gt::text AS gt, ls::text AS ls FROM cc_r;
SET provsql.active = on;
DROP TABLE cc_r, cc;

SELECT 'ok'::text AS agg_case_done;
