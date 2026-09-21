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
SELECT stddev(f)::text AS sd_float FROM dev WHERE g = 1;

SELECT remove_provenance('dev');
DROP TABLE dev;
