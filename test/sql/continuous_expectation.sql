\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Pin the MC RNG: any branch that falls back to monteCarloScalarSamples
-- below must be reproducible across runs.
SET provsql.monte_carlo_seed = 42;

-- Enforce analytical-only evaluation for the closed-form / structural-
-- independence / moment / central-moment sections below: if any of
-- them silently took the MC path (because of an incorrect dispatch
-- decision in Expectation.cpp), the call would raise instead of
-- passing within tolerance.  The MC-fallback section enables MC
-- explicitly, and the disable-toggle section turns it back off.
SET provsql.rv_mc_samples = 0;

-- ---------------------------------------------------------------------
-- Closed-form mean / variance for the three base distributions.
-- ---------------------------------------------------------------------

-- Constant: E[c] = c, Var[c] = 0.
SELECT expected(provsql.as_random(7.25))   AS const_mean;
SELECT variance(provsql.as_random(7.25))   AS const_variance;

-- Normal(2.5, 0.5): E = 2.5, Var = 0.25.
SELECT expected(provsql.normal(2.5, 0.5))  AS normal_mean;
SELECT variance(provsql.normal(2.5, 0.5))  AS normal_variance;

-- Uniform(1, 3): E = 2, Var = (3-1)^2 / 12 = 1/3.
SELECT expected(provsql.uniform(1, 3))     AS uniform_mean;
SELECT abs(variance(provsql.uniform(1, 3)) - 1.0/3.0) < 1e-12
       AS uniform_variance_exact;

-- Exponential(λ=2): E = 1/2, Var = 1/4.
SELECT expected(provsql.exponential(2))    AS exp_mean;
SELECT variance(provsql.exponential(2))    AS exp_variance;

-- ---------------------------------------------------------------------
-- Linearity of expectation (always analytical; no independence required).
-- ---------------------------------------------------------------------

-- E[X + Y] = E[X] + E[Y] for independent normals.
WITH r AS (SELECT provsql.normal(2.5, 0.5) + provsql.normal(1.0, 0.7) AS v)
SELECT abs(expected(v) - 3.5) < 1e-12 AS sum_indep_normals_mean
FROM r;

-- E[a*X + b] is via gate_arith TIMES with constant -- one of the two
-- children is a deterministic gate_value, footprints disjoint.
-- E[3 * N(2, 1) + 5] = 3*2 + 5 = 11.
WITH r AS (SELECT 3::random_variable * provsql.normal(2, 1) + 5 AS v)
SELECT abs(expected(v) - 11.0) < 1e-12 AS affine_mean
FROM r;

-- E[-X] = -E[X] (NEG via the unary - operator on random_variable).
WITH r AS (SELECT -provsql.exponential(4) AS v)
SELECT expected(v) = -0.25 AS neg_mean
FROM r;

-- E[X - Y] = E[X] - E[Y] for independent uniforms.
WITH r AS (SELECT provsql.uniform(0, 4) - provsql.uniform(0, 2) AS v)
SELECT abs(expected(v) - 1.0) < 1e-12 AS minus_indep_mean
FROM r;

-- ---------------------------------------------------------------------
-- Variance under structural independence.
-- ---------------------------------------------------------------------

-- Var(X + Y) = Var(X) + Var(Y), independent normals.
WITH r AS (SELECT provsql.normal(0, 3) + provsql.normal(0, 4) AS v)
SELECT abs(variance(v) - 25.0) < 1e-12 AS sum_indep_variance
FROM r;

-- Var(X - Y) = Var(X) + Var(Y), independent.
WITH r AS (SELECT provsql.normal(10, 3) - provsql.normal(-2, 4) AS v)
SELECT abs(variance(v) - 25.0) < 1e-12 AS minus_indep_variance
FROM r;

-- Var(-X) = Var(X).
WITH r AS (SELECT -provsql.uniform(2, 8) AS v)
SELECT abs(variance(v) - 3.0) < 1e-12 AS neg_variance
FROM r;

-- Var(X / c) = Var(X) / c^2 (constant divisor).
WITH r AS (SELECT provsql.normal(0, 6) / 3 AS v)
SELECT abs(variance(v) - 4.0) < 1e-12 AS div_const_variance
FROM r;

-- TIMES with disjoint footprints (independent normals): closed form.
-- E[X * Y]  = E[X] * E[Y] = 2 * 3 = 6
-- Var[X*Y] = (Var[X]+E[X]^2)*(Var[Y]+E[Y]^2) - (E[X]*E[Y])^2
--          = (1+4)*(4+9) - 36 = 65 - 36 = 29
WITH r AS (SELECT provsql.normal(2, 1) * provsql.normal(3, 2) AS v)
SELECT abs(expected(v) - 6.0) < 1e-12  AS times_indep_mean,
       abs(variance(v) - 29.0) < 1e-12 AS times_indep_variance
FROM r;

-- ---------------------------------------------------------------------
-- Raw moments of base distributions.
-- ---------------------------------------------------------------------

-- N(0, 1) raw moments: 1, 0, 1, 0, 3, 0, 15.
WITH x AS (SELECT provsql.normal(0, 1) AS v)
SELECT moment(v, 0) AS m0, moment(v, 1) AS m1,
       moment(v, 2) AS m2, moment(v, 3) AS m3,
       moment(v, 4) AS m4, moment(v, 5) AS m5,
       moment(v, 6) AS m6
FROM x;

-- U(0, 1) raw moments: 1/(k+1).
WITH x AS (SELECT provsql.uniform(0, 1) AS v)
SELECT moment(v, 0) AS m0,
       abs(moment(v, 1) - 0.5)        < 1e-12 AS m1_ok,
       abs(moment(v, 2) - 1.0/3.0)    < 1e-12 AS m2_ok,
       abs(moment(v, 3) - 0.25)       < 1e-12 AS m3_ok,
       abs(moment(v, 4) - 0.2)        < 1e-12 AS m4_ok
FROM x;

-- Exp(1) raw moments: k!.
WITH x AS (SELECT provsql.exponential(1) AS v)
SELECT moment(v, 0) AS m0, moment(v, 1) AS m1,
       moment(v, 2) AS m2, moment(v, 3) AS m3,
       moment(v, 4) AS m4
FROM x;

-- ---------------------------------------------------------------------
-- Central moments.  central_moment(v, 0)=1, (v, 1)=0, (v, 2)=variance.
-- For N(μ, σ): odd-k central moments are 0, even-k are σ^k * (k-1)!!.
-- ---------------------------------------------------------------------

WITH x AS (SELECT provsql.normal(7, 2) AS v)
SELECT central_moment(v, 0) AS c0,
       central_moment(v, 1) AS c1,
       central_moment(v, 2) AS c2,
       abs(central_moment(v, 3) - 0.0)  < 1e-9 AS c3_zero,
       abs(central_moment(v, 4) - 48.0) < 1e-9 AS c4_ok
FROM x;

-- ---------------------------------------------------------------------
-- Compositional raw moments (binomial expansion, independent children).
-- E[(X+Y)^2] = Var(X) + Var(Y) + (E[X]+E[Y])^2 for independent X,Y.
--   X ~ N(2,1), Y ~ N(-1,2): variance 1+4=5, mean^2 (2-1)^2 = 1, total 6.
-- ---------------------------------------------------------------------

WITH r AS (SELECT provsql.normal(2, 1) + provsql.normal(-1, 2) AS v)
SELECT abs(moment(v, 2) - 6.0) < 1e-12 AS plus_independent_m2
FROM r;

-- ---------------------------------------------------------------------
-- Disabling MC fallback: with provsql.rv_mc_samples = 0 (set at the
-- top of this file), sub-circuits that cannot be decomposed
-- analytically raise rather than silently sampling.  Each assertion
-- below covers a distinct non-decomposable shape and verifies that
-- the raised exception's message specifically blames rv_mc_samples
-- (so we catch any unrelated bug masquerading as the right error).
-- This double-checks that the analytical sections above never took
-- the MC path: if any of them had, they would have raised the same
-- exception and broken the test.
-- ---------------------------------------------------------------------

-- Shared base RV inside a TIMES: footprints overlap, no closed form.
DO $$
DECLARE msg text; matched bool;
BEGIN
  PERFORM expected(rv * rv) FROM (SELECT provsql.normal(0, 1) AS rv) t;
  RAISE NOTICE 'expected_times_shared_rv_did_not_raise';
EXCEPTION WHEN OTHERS THEN
  msg := SQLERRM;
  matched := position('rv_mc_samples' in msg) > 0;
  RAISE NOTICE 'expected_times_shared_raises_specific=%', matched;
END
$$;

-- Same shape via variance: must raise too (the variance formula falls
-- through to mc_var when independence fails).
DO $$
DECLARE msg text; matched bool;
BEGIN
  PERFORM provsql.variance(rv * rv) FROM (SELECT provsql.normal(0, 1) AS rv) t;
  RAISE NOTICE 'variance_times_shared_rv_did_not_raise';
EXCEPTION WHEN OTHERS THEN
  msg := SQLERRM;
  matched := position('rv_mc_samples' in msg) > 0;
  RAISE NOTICE 'variance_times_shared_raises_specific=%', matched;
END
$$;

-- Higher moment over shared TIMES: the binomial-expansion pathway
-- still routes the offending TIMES gate through the MC fallback.
DO $$
DECLARE msg text; matched bool;
BEGIN
  PERFORM moment(rv * rv, 3) FROM (SELECT provsql.normal(0, 1) AS rv) t;
  RAISE NOTICE 'moment_times_shared_rv_did_not_raise';
EXCEPTION WHEN OTHERS THEN
  msg := SQLERRM;
  matched := position('rv_mc_samples' in msg) > 0;
  RAISE NOTICE 'moment_times_shared_raises_specific=%', matched;
END
$$;

-- DIV with a non-constant divisor: closed form requires the divisor
-- to be a deterministic gate_value, otherwise MC fallback.
DO $$
DECLARE msg text; matched bool;
BEGIN
  PERFORM expected(provsql.uniform(1, 5) / provsql.uniform(2, 4));
  RAISE NOTICE 'expected_div_nonconst_did_not_raise';
EXCEPTION WHEN OTHERS THEN
  msg := SQLERRM;
  matched := position('rv_mc_samples' in msg) > 0;
  RAISE NOTICE 'expected_div_nonconst_raises_specific=%', matched;
END
$$;

-- ---------------------------------------------------------------------
-- MC fallback (shared base RV).  X * X has X repeated -> footprints
-- overlap, the analytical decomposition cannot apply, sampler kicks in.
-- For X ~ N(0, 1):
--   E[X^2] = 1 (chi-squared(1) mean), Var[X^2] = 2.
-- Pinning monte_carlo_seed makes the result reproducible; tolerance
-- tracks the standard error of 10000 N(0,1)^2 draws.  Re-enable MC
-- here (set was 0 at the top) so the sampler actually runs.
-- ---------------------------------------------------------------------

RESET provsql.rv_mc_samples;

WITH x AS (SELECT provsql.normal(0, 1) AS rv)
SELECT abs(expected(rv * rv) - 1.0) < 0.05  AS mc_fallback_mean,
       abs(variance(rv * rv) - 2.0) < 0.2   AS mc_fallback_variance
FROM x;

-- ---------------------------------------------------------------------
-- agg_token path: expected/variance/moment/central_moment all share
-- one polymorphic surface.  The agg_token-side raw moment routes
-- through agg_raw_moment (SUM via O(n^k) tuple enumeration; MIN/MAX
-- via rank enumeration).
-- ---------------------------------------------------------------------

CREATE TABLE expectation_agg_t(p text, v int);
INSERT INTO expectation_agg_t VALUES ('a', 10), ('b', 20), ('c', 30);
SELECT add_provenance('expectation_agg_t');

-- expected(SUM) still works via the existing path: each input gate
-- has the default probability 1.0, so E[SUM(v)] = 60.
DO $$
DECLARE
  s agg_token;
BEGIN
  SELECT sum(v) INTO s FROM expectation_agg_t;
  RAISE NOTICE 'agg_expected_sum=%', expected(s);
END
$$;

-- Set distinct, non-trivial inclusion probabilities so the variance /
-- moment formulas have something to do.  Wrap in a DO block to keep
-- the rewriter from leaking a non-deterministic provsql UUID column
-- into the expected output.
DO $$
BEGIN
  PERFORM set_prob(provenance(), 0.5) FROM expectation_agg_t WHERE p = 'a';
  PERFORM set_prob(provenance(), 0.4) FROM expectation_agg_t WHERE p = 'b';
  PERFORM set_prob(provenance(), 0.3) FROM expectation_agg_t WHERE p = 'c';
END
$$;

-- E[SUM] = 0.5*10 + 0.4*20 + 0.3*30 = 22.
-- Var[SUM] = sum of v_i^2 * p_i * (1 - p_i) (rows independent here):
--   100*0.5*0.5 + 400*0.4*0.6 + 900*0.3*0.7
-- = 25 + 96 + 189 = 310.
DO $$
DECLARE s agg_token; e_val float8; var_val float8;
BEGIN
  SELECT sum(v) INTO s FROM expectation_agg_t;
  e_val := expected(s); var_val := provsql.variance(s);
  RAISE NOTICE 'agg_sum_expected=%', e_val;
  RAISE NOTICE 'agg_sum_variance=%', var_val;
  -- E[SUM^2] = Var[SUM] + E[SUM]^2 = 310 + 484 = 794.
  RAISE NOTICE 'agg_sum_moment_2=%', moment(s, 2);
END
$$;

-- moment(s, 0) = 1; moment(s, 1) matches expected.
DO $$
DECLARE s agg_token;
BEGIN
  SELECT sum(v) INTO s FROM expectation_agg_t;
  RAISE NOTICE 'agg_sum_moment_0=%', moment(s, 0);
  RAISE NOTICE 'agg_sum_moment_1_eq_expected=%',
    abs(moment(s, 1) - expected(s)) < 1e-9;
END
$$;

-- variance via central_moment(2) must match variance() exactly.
DO $$
DECLARE s agg_token; v1 float8; v2 float8;
BEGIN
  SELECT sum(v) INTO s FROM expectation_agg_t;
  v1 := provsql.variance(s);
  v2 := central_moment(s, 2);
  RAISE NOTICE 'agg_sum_central_2_eq_variance=%', abs(v1 - v2) < 1e-9;
END
$$;

-- central_moment(_, 0) = 1; central_moment(_, 1) = 0.
DO $$
DECLARE s agg_token;
BEGIN
  SELECT sum(v) INTO s FROM expectation_agg_t;
  RAISE NOTICE 'agg_central_0=%', central_moment(s, 0);
  RAISE NOTICE 'agg_central_1=%', central_moment(s, 1);
END
$$;

-- MIN/MAX path: rank enumeration with v^k.  Use prov=provenance() so
-- the conditional normalisation kicks in (otherwise empty-aggregate
-- gives ±Infinity, mirroring expected()).
-- For our 3 rows with p=0.5, 0.4, 0.3 (independent):
--   P(any included) = 1 - 0.5*0.6*0.7 = 0.79
--   E[MIN | something included] / E[MIN^2 | something included]
-- These match what expected() / agg_raw_moment compute through the
-- same machinery; we only assert via cross-checks here.
DO $$
DECLARE
  mn agg_token; mx agg_token;
  pr uuid;
  e_mn float8; v_mn float8; m2_mn float8;
  e_mx float8; v_mx float8; m2_mx float8;
BEGIN
  SELECT min(v), max(v), provenance() INTO mn, mx, pr
    FROM expectation_agg_t;
  e_mn  := expected(mn, pr);
  v_mn  := provsql.variance(mn, pr);
  m2_mn := moment(mn, 2, pr);
  e_mx  := expected(mx, pr);
  v_mx  := provsql.variance(mx, pr);
  m2_mx := moment(mx, 2, pr);

  RAISE NOTICE 'agg_min_expected_finite=%', NOT (e_mn = 'Infinity'::float8);
  RAISE NOTICE 'agg_max_expected_finite=%', NOT (e_mx = '-Infinity'::float8);
  -- Var = E[X^2] - E[X]^2 must hold for MIN and MAX exactly.
  RAISE NOTICE 'agg_min_variance_consistent=%',
    abs(v_mn - (m2_mn - e_mn * e_mn)) < 1e-9;
  RAISE NOTICE 'agg_max_variance_consistent=%',
    abs(v_mx - (m2_mx - e_mx * e_mx)) < 1e-9;
END
$$;

-- Moments stay non-negative for even k; agg_max k=3 stays consistent
-- with the (-1)^k sign-flip baked into agg_raw_moment.
DO $$
DECLARE
  mx agg_token;
  pr uuid;
  m1 float8; m3 float8; m4 float8;
BEGIN
  SELECT max(v), provenance() INTO mx, pr FROM expectation_agg_t;
  m1 := moment(mx, 1, pr);
  m3 := moment(mx, 3, pr);
  m4 := moment(mx, 4, pr);
  -- For non-negative MAX values, every raw moment is non-negative.
  RAISE NOTICE 'agg_max_m3_nonneg=%', m3 >= 0;
  RAISE NOTICE 'agg_max_m4_nonneg=%', m4 >= 0;
  -- E[MAX^3]^(1/3) <= E[MAX^4]^(1/4) (Lyapunov inequality).
  RAISE NOTICE 'agg_max_lyapunov=%',
    power(m3, 1.0/3.0) <= power(m4, 1.0/4.0) + 1e-9;
END
$$;

-- Unsupported aggregation function still raises clearly.
DO $$
DECLARE raised bool := false;
BEGIN
  PERFORM moment(avg(v), 2) FROM expectation_agg_t;
EXCEPTION WHEN OTHERS THEN
  raised := true;
  RAISE NOTICE 'agg_avg_moment_raises=%', raised;
END
$$;

DROP TABLE expectation_agg_t;

RESET provsql.monte_carlo_seed;
