\set ECHO none
\pset format unaligned

-- ==========================================================================
-- Order statistics over random_variable: the same-row provsql.greatest /
-- provsql.least constructors and the max / min aggregates, both lowering to a
-- gate_arith MAX / MIN opcode.  MC-backed here (seed pinned); the closed forms
-- for i.i.d. families are asserted in continuous_analytic.  Support propagation
-- (RangeCheck) is exact and independent of MC.
-- ==========================================================================

SET provsql.active = on;
SET provsql.monte_carlo_seed = 42;
SET provsql.rv_mc_samples = 200000;
SET search_path TO public, provsql;

CREATE TABLE d AS SELECT uniform(0,1) AS x, uniform(0,1) AS y, uniform(0,1) AS z;

-- Same-row greatest / least of three i.i.d. U(0,1).  E[max] = n/(n+1) = 3/4,
-- E[min] = 1/(n+1) = 1/4, Var[max] = Var(Beta(3,1)) = 3/80 = 0.0375.
SELECT abs(expected(provsql.greatest(x,y,z)) - 0.75)    < 0.02  AS e_max_ok,
       abs(expected(provsql.least(x,y,z))    - 0.25)    < 0.02  AS e_min_ok,
       abs(variance(provsql.greatest(x,y,z)) - 3.0/80)  < 0.02  AS v_max_ok
  FROM d;

-- Support of a same-row order statistic is exact (interval propagation, no MC):
-- max / min of U(0,1) columns stays in [0, 1].
SELECT s.lo = 0 AS max_lo_0, s.hi = 1 AS max_hi_1
  FROM d, support(provsql.greatest(x,y,z)) s;

-- Two-argument greatest: E[max of two U(0,1)] = 2/3.
SELECT abs(expected(provsql.greatest(uniform(0,1), uniform(0,1))) - 2.0/3) < 0.02
       AS e_max2_ok;

-- Single argument (and NULL handling): greatest(x) is x; an all-NULL / empty
-- call is NULL.  Compare the underlying uuids -- an RV `=` would itself be
-- lifted into a comparison-event token, not a boolean.
WITH r AS (SELECT uniform(0,1) AS u)
SELECT provsql.greatest(u)::uuid = u::uuid AS greatest_singleton,
       provsql.greatest(NULL::random_variable) IS NULL AS greatest_all_null
  FROM r;

-- max / min aggregates over a column of i.i.d. U(0,1): same references.
WITH s(r) AS (VALUES (uniform(0,1)),(uniform(0,1)),(uniform(0,1)))
SELECT abs(expected(max(r)) - 0.75) < 0.02 AS agg_max_ok,
       abs(expected(min(r)) - 0.25) < 0.02 AS agg_min_ok
  FROM s;

-- Empty-group identity: max over zero rows is -inf, min over zero rows is +inf
-- (the extremum identities, the counterpart to sum's 0 / product's 1).
WITH s(r) AS (SELECT uniform(0,1) WHERE false)
SELECT expected(max(r)) = '-Infinity'::float8 AS empty_max_neg_inf,
       expected(min(r)) = 'Infinity'::float8  AS empty_min_pos_inf
  FROM s;

-- End-to-end through the provenance-tracked aggregate path: each row's argument
-- is wrapped in mixture(prov, X, as_random(0)) and the FFUNC patches the else
-- branch to the -inf identity, so a prob-1 group recovers E[max] = 3/4.
CREATE TABLE os_sensors(id text, reading random_variable);
INSERT INTO os_sensors VALUES
  ('a', uniform(0,1)), ('b', uniform(0,1)), ('c', uniform(0,1));
SELECT add_provenance('os_sensors');
CREATE TABLE os_peak AS SELECT max(reading) AS m FROM os_sensors;
SELECT remove_provenance('os_peak');
SELECT abs(expected(m) - 0.75) < 0.02 AS tracked_max_ok FROM os_peak;
DROP TABLE os_peak;
DROP TABLE os_sensors;

-- Closed-form order statistics (MC disabled): i.i.d. Uniform / Exponential
-- resolve exactly, no sampling.
SET provsql.rv_mc_samples = 0;

-- i.i.d. Uniform(0,1): E[max_n] = n/(n+1), E[min_n] = 1/(n+1), exact.
SELECT expected(provsql.greatest(x,y,z)) = 0.75 AS iid_unif_max_exact,
       expected(provsql.least(x,y,z))    = 0.25 AS iid_unif_min_exact
  FROM d;

-- i.i.d. Exponential(λ): E[min] = 1/(nλ), E[max] = H_n/λ.
SELECT expected(provsql.least(exponential(1), exponential(1))) = 0.5 AS iid_exp_min_exact,
       expected(provsql.greatest(exponential(1), exponential(1))) = 1.5 AS iid_exp_max_exact;

-- General Uniform(a,b): E[max of 2 U(2,6)] = 2 + 4*2/3 = 14/3.
SELECT abs(expected(provsql.greatest(uniform(2,6), uniform(2,6))) - 14.0/3) < 1e-12
       AS unif_ab_max_exact;

-- The builtin GREATEST / LEAST grammar is lifted over random_variable args
-- into the same order statistic, so bare greatest(x,y,z) works and matches
-- the schema-qualified form exactly.
SELECT expected(greatest(x,y,z)) = 0.75 AS bare_greatest_exact,
       expected(least(x,y,z))    = 0.25 AS bare_least_exact
  FROM d;

-- Idempotence: greatest(x,x,y) de-duplicates to greatest(x,y) -- the same
-- gate -- and greatest(x) collapses to x.
SELECT (greatest(x,x,y))::uuid = (greatest(x,y))::uuid AS dedup_same_gate,
       (greatest(x))::uuid     = (x)::uuid             AS singleton_collapses,
       expected(greatest(x,x,y)) = expected(greatest(x,y)) AS dedup_same_value
  FROM d;

RESET provsql.rv_mc_samples;

-- Ordering / comparing a random_variable directly is meaningless (a
-- distribution, not a scalar): ORDER BY over >1 row raises a clear error.
DO $$
BEGIN
  PERFORM r FROM (VALUES (uniform(0,1)),(uniform(0,1))) v(r) ORDER BY r;
  RAISE NOTICE 'rv_order_by_rejected=%', false;
EXCEPTION WHEN OTHERS THEN
  RAISE NOTICE 'rv_order_by_rejected=%', true;
END
$$;

DROP TABLE d;

SELECT 'ok'::text AS continuous_order_statistics_done;
