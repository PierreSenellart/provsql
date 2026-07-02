\set ECHO none
\pset format unaligned

-- Nonlinear transforms over random_variable (§B.3): the ^ operator and
-- pow / power / ln / exp / sqrt, as three appended gate_arith opcodes
-- (POW / LN / EXP; sqrt is pure sugar for ^ 0.5).  Moments have no
-- linearity to push through a nonlinear map, so evaluation is MC;
-- constant subtrees still fold exactly, RangeCheck propagates sound
-- support intervals through the transforms, and out-of-domain draws
-- raise actionable errors instead of being silently dropped.

-- (1) Constant folding is exact and needs no MC budget at all.
SET provsql.rv_mc_samples = 0;
SELECT provsql.expected(provsql.exp(provsql.as_random(0))) = 1
       AS exp_zero_folds;
SELECT abs(provsql.expected(provsql.exp(provsql.as_random(1))) - exp(1.0))
       < 1e-15 AS exp_one_folds;
SELECT provsql.expected(provsql.ln(provsql.as_random(1))) = 0
       AS ln_one_folds;
SELECT provsql.expected(provsql.pow(provsql.as_random(2), 3)) = 8
       AS pow_two_cubed_folds;
SELECT provsql.expected(provsql.as_random(9) ^ 0.5) = 3
       AS sqrt_via_operator_folds;
SELECT provsql.expected(provsql.sqrt(provsql.as_random(2.25))) = 1.5
       AS sqrt_sugar_folds;
-- A domain-violating constant does NOT fold (the NaN-as-sentinel
-- convention keeps the gate intact); with the MC fallback disabled the
-- evaluator reports the missing decomposition rather than a NaN.
\set VERBOSITY terse
SELECT provsql.expected(provsql.ln(provsql.as_random(-1)));
\set VERBOSITY default
RESET provsql.rv_mc_samples;

-- (2) RangeCheck: sound support propagation through the transforms.
SELECT lo, hi FROM support(provsql.exp(provsql.normal(0, 1)));   -- [0, inf)
SELECT lo, hi FROM support(provsql.ln(provsql.uniform(1, 10)));  -- [0, ln 10]
SELECT lo, hi FROM support(provsql.sqrt(provsql.uniform(0, 4))); -- [0, 2]
SELECT lo, hi
  FROM support(provsql.uniform(1, 2) ^ provsql.normal(0, 1));    -- [0, inf)

-- (3) The support bound decides comparisons without sampling:
-- exp(X) >= 0 always, so P(exp(X) > -1) = 1 exactly.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(provsql.exp(provsql.normal(0, 1)),
                           (-1)::random_variable),
         'independent') = 1.0 AS exp_positive_exact;

-- (4) Monte Carlo evaluation of the transforms (seeded).
SET provsql.monte_carlo_seed = 42;
-- E[exp(Z)] = e^0.5 (lognormal mean).
SELECT abs(provsql.expected(provsql.exp(provsql.normal(0, 1)))
           - 1.6487212707001282) < 0.1 AS lognormal_mean_mc;
-- E[ln U] = -1 for U(0,1).
SELECT abs(provsql.expected(provsql.ln(provsql.uniform(0, 1))) + 1)
       < 0.05 AS ln_uniform_mean_mc;
-- E[sqrt(U)] = 2/3.
SELECT abs(provsql.expected(provsql.sqrt(provsql.uniform(0, 1))) - 2.0 / 3)
       < 0.02 AS sqrt_uniform_mean_mc;
-- The motivating inverse-CDF construction (lessons §4): Y = 2·U^(1/4),
-- E[Y] = 2/(1 + 1/4) = 1.6.
SELECT abs(provsql.expected(2 * provsql.uniform(0, 1) ^ 0.25) - 1.6)
       < 0.02 AS generative_pow_mc;
-- Median of exp(Z) is 1 (lognormal median = e^μ).
WITH r AS (SELECT provsql.normal(0, 1) AS x)
SELECT abs(provsql.quantile(provsql.exp(x), 0.5) - 1) < 0.05
       AS lognormal_median_mc
  FROM r;
-- Monotone-threshold agreement: P(exp(Z) > e) = 1 - Φ(1).
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(provsql.exp(provsql.normal(0, 1)),
                               exp(1.0)::random_variable),
             'monte-carlo', '100000') - 0.15865525393145707) < 0.01
       AS lognormal_tail_mc;

-- (5) Out-of-domain draws raise actionable errors (never a silently
-- dropped NaN, which would bias the estimate).
\set VERBOSITY terse
SELECT provsql.expected(provsql.ln(provsql.normal(0, 1)));
SELECT provsql.expected(provsql.sqrt(provsql.normal(0, 1)));
\set VERBOSITY default
-- The hinted rewrite works: E[sqrt(greatest(Z, 0))] ≈ 0.41108947933.
SELECT abs(provsql.expected(provsql.sqrt(provsql.greatest(
             provsql.normal(0, 1), provsql.as_random(0))))
           - 0.4110894793312293) < 0.02 AS sqrt_clamped_mc;
RESET provsql.monte_carlo_seed;

-- (6) Integer exponents are total: negative bases are fine.
SET provsql.monte_carlo_seed = 42;
-- E[Z^2] = 1 via pow (matches moment(x, 2)).
SELECT abs(provsql.expected(provsql.normal(0, 1) ^ 2) - 1) < 0.05
       AS integer_pow_total_mc;
RESET provsql.monte_carlo_seed;

-- (7) Unqualified spellings: exp / ln / sqrt / pow / power shadow
-- pg_catalog names, so resolution must pick the rv overload for an rv
-- argument (no rv -> float8 cast exists, so only the provsql candidate
-- matches) while plain numeric arguments keep resolving to the
-- pg_catalog functions (float8 is the preferred numeric type, so the
-- implicit numeric -> rv cast never hijacks them).
WITH r AS (SELECT provsql.uniform(0, 1) AS u)
SELECT pg_typeof(exp(u))       = 'provsql.random_variable'::regtype AS exp_unqualified,
       pg_typeof(ln(u))        = 'provsql.random_variable'::regtype AS ln_unqualified,
       pg_typeof(sqrt(u))      = 'provsql.random_variable'::regtype AS sqrt_unqualified,
       pg_typeof(pow(u, 0.5))  = 'provsql.random_variable'::regtype AS pow_unqualified,
       pg_typeof(power(u, 2))  = 'provsql.random_variable'::regtype AS power_unqualified,
       pg_typeof(u ^ 0.5)      = 'provsql.random_variable'::regtype AS caret_unqualified
  FROM r;
SELECT pg_typeof(exp(1))    = 'double precision'::regtype AS exp_numeric_unaffected,
       pg_typeof(sqrt(2))   = 'double precision'::regtype AS sqrt_numeric_unaffected,
       pg_typeof(ln(2.0))   = 'numeric'::regtype          AS ln_numeric_unaffected,
       pg_typeof(pow(2, 3)) = 'double precision'::regtype AS pow_numeric_unaffected,
       pg_typeof(2 ^ 3)     = 'double precision'::regtype AS caret_numeric_unaffected;
-- ...and the unqualified rv spellings evaluate correctly (exact
-- constant folds, no MC budget needed).
SET provsql.rv_mc_samples = 0;
SELECT provsql.expected(sqrt(2.25::random_variable)) = 1.5
       AS sqrt_unqualified_folds,
       provsql.expected(exp(0::random_variable)) = 1
       AS exp_unqualified_folds,
       provsql.expected(pow(2::random_variable, 3)) = 8
       AS pow_unqualified_folds;
RESET provsql.rv_mc_samples;

SELECT 'ok'::text AS continuous_transforms_done;
