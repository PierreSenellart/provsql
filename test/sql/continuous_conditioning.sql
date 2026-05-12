\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Pin the MC seed: any test below that falls back to MC rejection
-- must be reproducible across runs.
SET provsql.monte_carlo_seed = 42;

-- Sections 1-3 below enforce closed-form only.  The MC-fallback
-- section explicitly raises rv_mc_samples first.
SET provsql.rv_mc_samples = 0;

-- ---------------------------------------------------------------------
-- 1. Truncated Normal: closed form.
--
--   X ~ N(0, 1) conditioned on X > 3.  The truncated-normal mean is
--   φ(3) / (1 − Φ(3)).  The second raw moment is
--   σ² (1 + α·φ(α)/(1−Φ(α))) with α = 3.  Reference values come from
--   directly evaluating the closed form via PG's erf().
-- ---------------------------------------------------------------------
WITH r AS (SELECT provsql.normal(0, 1) AS rv),
     ref AS (
       SELECT  0.39894228040143268 * exp(-9.0/2)
             / (1 - (0.5 * (1 + erf(3/sqrt(2.0))))) AS mean_ref
     )
SELECT abs(expected(rv, rv_cmp_gt(rv, as_random(3))) - ref.mean_ref) < 1e-12
         AS trunc_normal_mean,
       abs(moment(rv, 2, rv_cmp_gt(rv, as_random(3)))
            - (1 + 3 * ref.mean_ref)) < 1e-12
         AS trunc_normal_m2
  FROM r, ref;

-- Conditional support of the same RV: should be (3, +Infinity).
WITH r AS (SELECT provsql.normal(0, 1) AS rv)
SELECT s.lo = 3 AS trunc_normal_lo_eq_3,
       s.hi = 'Infinity'::float8 AS trunc_normal_hi_inf
  FROM r, provsql.rv_support(rv, rv_cmp_gt(rv, as_random(3))) s;

-- ---------------------------------------------------------------------
-- 2. Truncated Uniform with two cmps (AND-conjunct walker).
--
--   X ~ U(0, 10), event = (X > 3 AND X < 7).  Conditional distribution
--   is U(3, 7); mean = 5, variance = (7-3)^2 / 12 = 4/3.
-- ---------------------------------------------------------------------
DO $$
DECLARE
  rv random_variable;
  ev uuid;
BEGIN
  rv := provsql.uniform(0, 10);
  ev := provenance_times(rv_cmp_gt(rv, as_random(3)),
                         rv_cmp_lt(rv, as_random(7)));
  RAISE NOTICE 'trunc_uniform_mean=%',
    abs(expected(rv, ev) - 5.0) < 1e-12;
  RAISE NOTICE 'trunc_uniform_variance=%',
    abs(provsql.variance(rv, ev) - 4.0/3.0) < 1e-12;
  RAISE NOTICE 'trunc_uniform_support=%',
    (SELECT s.lo = 3 AND s.hi = 7 FROM provsql.rv_support(rv, ev) s);
END
$$;

-- ---------------------------------------------------------------------
-- 3. Truncated Exponential: memorylessness.
--
--   X ~ Exp(2), event = X > 1.  Conditional mean = 1 + 1/2 = 1.5.
-- ---------------------------------------------------------------------
WITH r AS (SELECT provsql.exponential(2) AS rv)
SELECT abs(expected(rv, rv_cmp_gt(rv, as_random(1))) - 1.5) < 1e-12
         AS trunc_exp_memorylessness
  FROM r;

-- Truncated Exp on a finite interval: E[X | a < X < b] computed
-- against the closed-form formula.  For X ~ Exp(2), a = 0, b = 1:
--   (1 − 3 e^{-2}) / (2 (1 − e^{-2}))
WITH r AS (SELECT provsql.exponential(2) AS rv),
     ref AS (SELECT (1 - 3 * exp(-2.0)) / (2 * (1 - exp(-2.0))) AS mean_ref)
SELECT abs(expected(rv, rv_cmp_lt(rv, as_random(1))) - ref.mean_ref) < 1e-12
         AS trunc_exp_finite_interval
  FROM r, ref;

-- ---------------------------------------------------------------------
-- 4. Gate_one event -> unconditional.  Passing gate_one() as prov
--    must produce the same answer as omitting it (verified against
--    the closed-form unconditional mean of N(0,1) = 0).
-- ---------------------------------------------------------------------
WITH r AS (SELECT provsql.normal(0, 1) AS rv)
SELECT expected(rv, gate_one()) = expected(rv) AS gate_one_unconditional,
       expected(rv) = 0 AS unconditional_mean_zero
  FROM r;

-- ---------------------------------------------------------------------
-- 5. WHERE end-to-end.  This is the headline test: builds a sensor
--    table, runs the planner hook that lifts `reading > 3` into the
--    row's provenance, and asks for `expected(reading, provenance())`.
--
--    Tests the full pipeline: joint-circuit loader, AND-walker
--    skipping the independent input-Bernoulli factor, and the
--    closed-form Normal truncation.
-- ---------------------------------------------------------------------
CREATE TABLE cond_sensor(reading random_variable);
INSERT INTO cond_sensor VALUES (provsql.normal(0, 1));
SELECT add_provenance('cond_sensor');

WITH ref AS (
  SELECT  0.39894228040143268 * exp(-9.0/2)
        / (1 - (0.5 * (1 + erf(3/sqrt(2.0))))) AS mean_ref
)
SELECT abs(expected(reading, provenance()) - ref.mean_ref) < 1e-12
         AS where_e2e_mean,
       abs(moment(reading, 2, provenance())
            - (1 + 3 * ref.mean_ref)) < 1e-12
         AS where_e2e_m2
  INTO cond_sensor_result
  FROM cond_sensor, ref WHERE reading > 3;
SELECT remove_provenance('cond_sensor_result');
SELECT * FROM cond_sensor_result;
DROP TABLE cond_sensor_result;
DROP TABLE cond_sensor;

-- ---------------------------------------------------------------------
-- 6. Shared-atom coupling.  The same RV UUID appears in two cmps; the
--    MC rejection sampler must use one draw of the RV per iteration
--    so both indicators see consistent state.  Closed-form path on
--    the first cmp is OK; the AND-walker collects both.  Mean of
--    Exp(1) | (X > 1 AND X < 5) by closed-form vs. the same event
--    expressed as two cmps using the SAME rv must agree.
-- ---------------------------------------------------------------------
DO $$
DECLARE
  rv random_variable;
  ev_one uuid;
  ev_two uuid;
BEGIN
  rv := provsql.exponential(1);
  ev_one := provenance_times(rv_cmp_gt(rv, as_random(1)),
                             rv_cmp_lt(rv, as_random(5)));
  -- Re-conjoin: trailing gate_one factor is independent and should
  -- be transparent for the conditional mean.
  ev_two := provenance_times(rv_cmp_gt(rv, as_random(1)),
                             rv_cmp_lt(rv, as_random(5)),
                             gate_one());
  RAISE NOTICE 'shared_atom_coupling=%',
    abs(expected(rv, ev_one) - expected(rv, ev_two)) < 1e-12;
END
$$;

-- ---------------------------------------------------------------------
-- 7. Sample + histogram honour the conditional event.  These need MC
--    so we enable rv_mc_samples before this section.
-- ---------------------------------------------------------------------
SET provsql.rv_mc_samples = 10000;

-- All accepted samples land inside the truncation interval (3, 7).
WITH r AS (SELECT provsql.uniform(0, 10) AS rv),
     s AS (
       SELECT rv,
              provenance_times(rv_cmp_gt(rv, as_random(3)),
                               rv_cmp_lt(rv, as_random(7))) AS ev
         FROM r
     )
SELECT bool_and(v > 3 AND v < 7) AS samples_in_truncation
  FROM s, provsql.rv_sample(rv, 100, ev) AS v;

-- Histogram bin edges line up with the truncated support.
WITH r AS (SELECT provsql.uniform(0, 10) AS rv),
     s AS (
       SELECT rv,
              provenance_times(rv_cmp_gt(rv, as_random(3)),
                               rv_cmp_lt(rv, as_random(7))) AS ev
         FROM r
     )
SELECT (h.first_bin_lo)::numeric = 3 AS hist_lo_eq_3,
       (h.last_bin_hi)::numeric = 7 AS hist_hi_eq_7
  FROM s,
       LATERAL (
         WITH bins AS (
           SELECT jsonb_array_elements(provsql.rv_histogram(rv, 10, ev)) AS b
         )
         SELECT (SELECT (b->>'bin_lo')::float8 FROM bins LIMIT 1) AS first_bin_lo,
                (SELECT (b->>'bin_hi')::float8 FROM bins
                  ORDER BY (b->>'bin_hi')::float8 DESC LIMIT 1) AS last_bin_hi
       ) h;

-- ---------------------------------------------------------------------
-- 8. Zero-probability event raises an actionable error.
--    P(N(0,1) > 100) is effectively 0; conditional MC accepts nothing.
-- ---------------------------------------------------------------------
DO $$
DECLARE
  rv random_variable;
  ev uuid;
  msg text;
  matched bool;
BEGIN
  rv := provsql.normal(0, 1);
  -- N(0,1) | X*X > 10000: the squared shape defeats the closed-form
  -- walker (cmp side is gate_arith, not bare gate_rv), so MC kicks in.
  -- With finite samples, the acceptance rate is ~0.
  ev := provsql.provenance_cmp(
          random_variable_uuid(rv * rv),
          random_variable_cmp_oid('>'),
          random_variable_uuid(as_random(10000)));
  BEGIN
    PERFORM expected(rv, ev);
    RAISE NOTICE 'zero_prob_event_did_not_raise';
  EXCEPTION WHEN OTHERS THEN
    msg := SQLERRM;
    -- Either MC's acceptance check or RangeCheck's infeasibility
    -- collapse fires; both are acceptable signals to the user.
    matched := position('rv_mc_samples' IN msg) > 0
            OR position('satisfiable' IN msg) > 0
            OR position('accepted' IN msg) > 0;
    RAISE NOTICE 'zero_prob_event_raises_specific=%', matched;
  END;
END
$$;

RESET provsql.monte_carlo_seed;
RESET provsql.rv_mc_samples;
