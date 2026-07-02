\set ECHO none
\pset format unaligned

-- Gamma(k, λ) with general (non-integer) shape k: the first family
-- added under the per-family src/distributions/ layout.  The SQL
-- constructor routes integer shapes through erlang (the gamma with
-- integer shape IS Erlang), so a gamma gate always carries a
-- non-integer shape; its CDF is the regularised lower incomplete
-- gamma P(k, λx).  chi_squared(k) is sugar for gamma(k/2, 1/2).

-- (1) Closed-form moments:
-- E[Gamma(2.5, 0.4)] = k/λ = 6.25, Var = k/λ² = 15.625,
-- E[X²] = k(k+1)/λ² = 2.5·3.5/0.16 = 54.6875 (tolerance-checked:
-- 0.4² is not an exact double).
SELECT provsql.expected(provsql.gamma(2.5, 0.4)) = 6.25 AS gamma_mean_exact;
SELECT abs(provsql.variance(provsql.gamma(2.5, 0.4)) - 15.625) < 1e-12
       AS gamma_var_exact;
SELECT abs(provsql.moment(provsql.gamma(2.5, 0.4), 2) - 54.6875) < 1e-10
       AS gamma_m2_exact;

-- (2) Support: [0, +Infinity).
SELECT lo, hi FROM support(provsql.gamma(2.5, 0.4));

-- (3) CDF, series branch of the incomplete gamma (λc < k+1):
-- Gamma(0.5, 0.5) is χ²₁ = Z² for Z ~ N(0,1), so
-- P(X <= 1) = P(|Z| <= 1) = erf(1/√2) ≈ 0.6826894921370859.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_le(provsql.gamma(0.5, 0.5), 1::random_variable),
             'independent') - 0.6826894921370859) < 1e-12
       AS gamma_cdf_series_exact;

-- (4) CDF, continued-fraction branch (λc >= k+1):
-- P(χ²₁ > 4) = P(|Z| > 2) = 1 - erf(√2) ≈ 0.04550026389635842.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(provsql.gamma(0.5, 0.5), 4::random_variable),
             'independent') - 0.04550026389635842) < 1e-12
       AS gamma_cdf_cf_exact;

-- (5) chi_squared sugar, odd df: chi_squared(1) = gamma(0.5, 0.5).
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_le(provsql.chi_squared(1), 1::random_variable),
             'independent') - 0.6826894921370859) < 1e-12
       AS chisq1_sugar_exact;

-- (6) chi_squared sugar, even df routes through erlang:
-- chi_squared(4) = gamma(2, 0.5) = erlang(2, 0.5), and
-- P(χ²₄ > 2) = e^{-1}(1 + 1) = 2/e via the Erlang finite-sum CDF.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(provsql.chi_squared(4), 2::random_variable),
             'independent') - 2.0 * exp(-1.0)) < 1e-12
       AS chisq4_via_erlang_exact;

-- (7) Integer-shape routing: gamma(3, 2) is stored as erlang:3,2, so
-- its P(X > 1) matches erlang(3, 2)'s finite-sum CDF exactly.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(provsql.gamma(3, 2), 1::random_variable),
         'independent')
     = provsql.probability_evaluate(
         provsql.rv_cmp_gt(provsql.erlang(3, 2), 1::random_variable),
         'independent')
       AS gamma_integer_routes_to_erlang;

-- (8) Same-rate sum closure: Gamma(0.5, 2) + Gamma(0.7, 2) folds to
-- Gamma(1.2, 2) in the simplifier, so the comparison resolves via the
-- closed-form CDF (exact, no MC noise):
-- P(Gamma(1.2, 2) <= 1) = P(1.2, 2) ≈ 0.8176987670910338 (mpmath).
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_le(provsql.gamma(0.5, 2) + provsql.gamma(0.7, 2),
                               1::random_variable),
             'independent') - 0.8176987670910338) < 1e-12
       AS gamma_sum_closure_exact;

-- (9) Positive-scaling fold: 2 · Gamma(0.5, 1) = Gamma(0.5, 0.5) = χ²₁,
-- so P(2X <= 1) = erf(1/√2) again, exactly, via the affine fold + CDF.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_le(2 * provsql.gamma(0.5, 1), 1::random_variable),
             'independent') - 0.6826894921370859) < 1e-12
       AS gamma_scale_fold_exact;

-- (10) Gamma vs Gamma comparison: the pdf declines at the shape<1
-- singular endpoint, so the pairwise quadrature bails and the whole
-- circuit falls to seeded MC sampling std::gamma_distribution.
-- P(X < Y) for X ~ Gamma(0.5, 1), Y ~ Gamma(0.7, 1): X/(X+Y) ~
-- Beta(0.5, 0.7), so P = I_{1/2}(0.5, 0.7) ≈ 0.6003642321330015.
SET provsql.monte_carlo_seed = 42;
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_lt(provsql.gamma(0.5, 1), provsql.gamma(0.7, 1)),
             'monte-carlo', '100000') - 0.6003642321330015) < 0.01
       AS gamma_vs_gamma_mc_within_tolerance;
RESET provsql.monte_carlo_seed;

-- (11) Constructor validation errors.  VERBOSITY terse keeps the
-- plpgsql CONTEXT lines out of the expected output.
\set VERBOSITY terse
SELECT provsql.gamma(0, 1);
SELECT provsql.gamma(1, 0);
SELECT provsql.gamma('NaN', 1);
SELECT provsql.chi_squared(-1);
\set VERBOSITY default

-- (12) End-to-end through the planner hook: WHERE rv > c on a
-- gamma-valued column resolves to the exact CDF answer.
CREATE TABLE gamma_sensors(id text, load provsql.random_variable);
INSERT INTO gamma_sensors VALUES
  ('a', provsql.gamma(0.5, 0.5)),
  ('b', provsql.chi_squared(4));
SELECT add_provenance('gamma_sensors');
CREATE TABLE gamma_result AS
  SELECT id, probability_evaluate(provenance(), 'independent') AS p
    FROM gamma_sensors WHERE load > 2;
SELECT remove_provenance('gamma_result');
-- P(χ²₁ > 2) = P(|Z| > √2) = 1 - erf(1) ≈ 0.1572992070502851;
-- P(χ²₄ > 2) = 2/e (erlang finite sum).
SELECT id,
       abs(p - CASE id WHEN 'a' THEN 1 - 0.8427007929497149
                       WHEN 'b' THEN 2.0 * exp(-1.0) END) < 1e-12
       AS within_tolerance
  FROM gamma_result ORDER BY id;
DROP TABLE gamma_result;
DROP TABLE gamma_sensors;

SELECT 'ok'::text AS continuous_gamma_done;
