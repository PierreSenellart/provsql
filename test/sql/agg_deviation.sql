\set ECHO none
\pset format unaligned

-- ----------------------------------------------------------------------
-- stddev and variance read as the arithmetic that defines them:
--   var_samp(x) = (sum(x*x) - sum(x)^2 / count(x)) / (count(x) - 1)
--   var_pop(x)  = the same over count(x)
--   stddev(x)   = var(x) ^ 0.5
-- over sum, sum of squares and count, which the provenance machinery carries.
-- PostgreSQL computes these with an accumulator of its own, whose result the
-- machinery had no reading of: the value was displayed and could not be read
-- in any other world (moment() refused it, "Cannot compute moment for
-- aggregation function stddev").
--
-- Only an exact argument type, where the arithmetic gives the same number
-- PostgreSQL's accumulator does, digit for digit; a floating-point one is
-- left as it was.
-- ----------------------------------------------------------------------

CREATE TABLE dev(g int, v int, f float8);
INSERT INTO dev VALUES (1,1,1), (1,2,2), (1,4,4), (2,5,5);
SELECT add_provenance('dev');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM dev; END $$;

-- The value on the data as it is, beside what plain SQL answers.
CREATE TABLE dev_v AS
  SELECT stddev(v) AS sd, variance(v) AS var,
         stddev_pop(v) AS sdp, var_pop(v) AS vp
  FROM dev WHERE g = 1;
SELECT remove_provenance('dev_v');
SELECT sd::text AS sd, var::text AS var, sdp::text AS sdp, vp::text AS vp
FROM dev_v;
DROP TABLE dev_v;
SET provsql.active = off;
SELECT stddev(v)::text AS sd, variance(v)::text AS var,
       stddev_pop(v)::text AS sdp, var_pop(v)::text AS vp
FROM dev WHERE g = 1;
SET provsql.active = on;

-- And in every world.  The three rows of the first group are each present with
-- probability one half, so the sample forms are defined in the four worlds
-- holding two rows or more (probability 1/2) and the population forms in the
-- seven holding one or more.  Enumerated by hand over {1,2,4}:
--   E[stddev | defined]     = (0.7071068 + 2.1213203 + 1.4142136 + 1.5275252)/4
--                           = 1.442541
--   E[variance | defined]   = (0.5 + 4.5 + 2 + 2.3333333)/4 = 2.333333
--   E[stddev_pop | defined] = 0.606746, E[var_pop | defined] = 0.722222
CREATE TABLE dev_e AS
  SELECT stddev(v) AS sd, variance(v) AS var,
         stddev_pop(v) AS sdp, var_pop(v) AS vp
  FROM dev WHERE g = 1;
SET provsql.active = off;
SELECT round(expected(sd, provsql)::numeric, 6) AS e_sd,
       round(expected(var, provsql)::numeric, 6) AS e_var,
       round(expected(sdp, provsql)::numeric, 6) AS e_sdp,
       round(expected(vp, provsql)::numeric, 6) AS e_vp
FROM dev_e;
SET provsql.active = on;
DROP TABLE dev_e;

-- difftest found the population forms counting only the worlds with two rows
-- or more, as if they followed the sample rule: over {2,4} at one half each,
-- E[var_pop | defined] is (0 + 0 + 1)/3 = 1/3 and not 1.  The cause was the
-- guard that keeps an exact zero from printing the trailing digits of the
-- division's scale: a comparison of the NUMERATOR with zero is not read in
-- every world, so the worlds it picks dropped out of the moments.  The guard
-- is a COUNT compared with a constant now, which is read in every world.  One
-- case is left over: a variance that is exactly zero over SEVERAL equal rows
-- takes the quotient and prints 0.00000000000000000000 where PostgreSQL prints
-- 0.  Catching that needs the comparison of an arithmetic expression over
-- aggregates to be readable per world, which is a gap of its own.
CREATE TABLE dev_pop(v int);
INSERT INTO dev_pop VALUES (2), (4);
SELECT add_provenance('dev_pop');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM dev_pop; END $$;
CREATE TABLE dev_p AS
  SELECT var_pop(v) AS vp, stddev_pop(v) AS sdp, variance(v) AS var FROM dev_pop;
SET provsql.active = off;
SELECT vp::text AS vp,
       round(expected(vp, provsql)::numeric, 6) AS e_vp,
       round(expected(sdp, provsql)::numeric, 6) AS e_sdp,
       round(expected(var, provsql)::numeric, 6) AS e_var
FROM dev_p;
SET provsql.active = on;
DROP TABLE dev_p;
SELECT remove_provenance('dev_pop');
DROP TABLE dev_pop;

-- A group of one row: the sample forms divide by count-1, which is zero there,
-- and SQL answers NULL rather than raising.  The guard says so, per world.
CREATE TABLE dev_g AS
  SELECT g, stddev(v) AS sd, var_pop(v) AS vp FROM dev GROUP BY g;
SELECT remove_provenance('dev_g');
-- Printed as text on both sides: the sample form has no value for the group of
-- one row, and a value that is not there prints as nothing either way.  Its
-- population form is 0 there, exactly, and not the trailing zeros a division
-- of its own scale would give.
SELECT g, sd::text AS sd, vp::text AS vp FROM dev_g ORDER BY g;
DROP TABLE dev_g;
SET provsql.active = off;
SELECT g, stddev(v)::text AS sd, var_pop(v)::text AS vp
FROM dev GROUP BY g ORDER BY g;
SET provsql.active = on;

-- A floating-point argument is left as it was: its value is the one of the
-- data as it is, and no other world is read.
-- Rounded: a float8 prints a different number of digits from one PostgreSQL
-- version to the next.
SELECT round(stddev(f)::numeric, 6)::text AS sd_float FROM dev WHERE g = 1;

SELECT remove_provenance('dev');
DROP TABLE dev;

-- The moment of a deviation over a group that is not always there.  A var_pop
-- is a CASE: the count against 0, the count against 1, and a formula over the
-- sums.  In the world where the group has NO row, both guards are comparisons
-- over an aggregate that has no value there, so neither fires and the FORMULA
-- arm is selected -- and `agg_defined_event` called an arith gate defined in
-- every world, so that world was counted although the formula's operands have
-- no value in it.  Arithmetic is strict, so an arith is defined where its
-- operands are, which is what it now answers.
-- The reading with the row's provenance and the reading without it MUST agree
-- here: the only world without a value is the one where the group is absent, so
-- there is nothing for the condition to remove -- exactly as for min and sum,
-- which agreed all along and are kept below as the contrast.
-- One row in the first group and two in the second, each at one half:
--   g1: the value is 0 wherever it exists, so 0 either way.
--   g2: var_pop is 0 over {5}, 0 over {6} and 0.25 over {5,6}, so 0.25/3 =
--       0.083333; its square root is 0.5 over {5,6}, so 0.5/3 = 0.166667.
-- Before the fix these read NaN and 0.125, and NaN and 0.25.
CREATE TABLE dev_m(g int, x int);
INSERT INTO dev_m VALUES (1,4), (2,5), (2,6);
SELECT add_provenance('dev_m');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM dev_m; END $$;
CREATE TABLE dev_mr AS SELECT g,
  round(expected(var_pop(x))::numeric, 6) AS vp,
  round(expected(var_pop(x), provenance())::numeric, 6) AS vp_cond,
  round(expected(stddev_pop(x))::numeric, 6) AS sp,
  round(expected(stddev_pop(x), provenance())::numeric, 6) AS sp_cond,
  round(expected(min(x))::numeric, 6) AS mn,
  round(expected(sum(x))::numeric, 6) AS sm
  FROM dev_m GROUP BY g;
SELECT remove_provenance('dev_mr');
SELECT * FROM dev_mr ORDER BY g;
DROP TABLE dev_mr;
SELECT remove_provenance('dev_m');
DROP TABLE dev_m;
