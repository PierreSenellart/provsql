\set ECHO none
\pset format unaligned

-- quantile(input, p [, prov]): the inverse-CDF readout (§B.1), the
-- polymorphic sibling of expected / variance / moment / support.
-- Closed forms per family (Normal via Beasley-Springer-Moro polished
-- by Newton steps, Uniform / Exponential by algebraic inversion),
-- generic monotone-CDF bisection where no elementary inverse exists
-- (Erlang, Gamma), exact generalised inverse for categoricals, and
-- the empirical MC quantile for compound circuits.

-- (1) Normal: median exactly 0; the 97.5% quantile of N(0,1) to full
-- double precision (Newton-polished, not the raw ~1e-7 BSM).
SELECT provsql.quantile(provsql.normal(0, 1), 0.5) = 0 AS normal_median_exact;
SELECT abs(provsql.quantile(provsql.normal(0, 1), 0.975)
           - 1.959963984540054) < 1e-12 AS normal_q975_exact;
SELECT abs(provsql.quantile(provsql.normal(10, 2), 0.975)
           - (10 + 2 * 1.959963984540054)) < 1e-11 AS normal_affine_exact;

-- (2) Uniform: algebraic inversion, exact.
SELECT provsql.quantile(provsql.uniform(2, 6), 0.25) = 3 AS uniform_q25_exact;
SELECT provsql.quantile(provsql.uniform(2, 6), 0.5)  = 4 AS uniform_median_exact;

-- (3) Exponential: -log1p(-p)/λ; median of Exp(2) = ln(2)/2.
SELECT abs(provsql.quantile(provsql.exponential(2), 0.5) - ln(2.0) / 2)
       < 1e-15 AS exponential_median_exact;

-- (4) Erlang: no elementary inverse; the generic CDF bisection inverts
-- the finite-sum CDF.  F(2) = 1 - 3e⁻² for Erlang(2,1), so the
-- quantile at that mass must return 2.
SELECT abs(provsql.quantile(provsql.erlang(2, 1),
                            1 - 3 * exp(-2.0)) - 2) < 1e-9
       AS erlang_bisection_exact;

-- (5) Gamma: bisection on the regularised lower incomplete gamma.
-- chi_squared(1) = Z², so its CDF at 1 is erf(1/√2): the quantile at
-- that mass must return 1.
SELECT abs(provsql.quantile(provsql.chi_squared(1),
                            0.6826894921370859) - 1) < 1e-9
       AS gamma_bisection_exact;

-- (6) Quantile limits: p = 0 / 1 are the support edges.
SELECT provsql.quantile(provsql.normal(0, 1), 0) = '-Infinity' AS normal_q0_lo;
SELECT provsql.quantile(provsql.normal(0, 1), 1) = 'Infinity' AS normal_q1_hi;
SELECT provsql.quantile(provsql.uniform(1, 3), 0) = 1 AS uniform_q0_lo;
SELECT provsql.quantile(provsql.uniform(1, 3), 1) = 3 AS uniform_q1_hi;
SELECT provsql.quantile(provsql.exponential(1), 0) = 0 AS exponential_q0_lo;

-- (7) Deterministic scalars are Diracs: every quantile is the value,
-- through the numeric branch and the gate_value branch alike.
SELECT provsql.quantile(4.25, 0.99) = 4.25 AS numeric_dirac;
SELECT provsql.quantile(3::integer, 0.01) = 3 AS integer_dirac;
SELECT provsql.quantile(provsql.as_random(7.5), 0.5) = 7.5 AS as_random_dirac;

-- (8) Categorical: exact generalised inverse F⁻¹(p) = min{v : F(v) ≥ p}.
-- F(1) = 0.2, F(2) = 0.7, F(3) = 1.
WITH c AS (SELECT provsql.categorical(ARRAY[0.2, 0.5, 0.3],
                                      ARRAY[1.0, 2.0, 3.0]) AS x)
SELECT provsql.quantile(x, 0.1) = 1 AS cat_q10,
       provsql.quantile(x, 0.2) = 1 AS cat_q20_boundary,
       provsql.quantile(x, 0.5) = 2 AS cat_median,
       provsql.quantile(x, 0.9) = 3 AS cat_q90
  FROM c;

-- (9) Conditioning: exact truncated quantiles via the interval event.
-- Median of N(0,1) | X > 0 (half-normal) = Φ⁻¹(0.75) ≈ 0.6744897501960817.
WITH r AS (SELECT provsql.normal(0, 1) AS x)
SELECT abs(provsql.quantile(x | (x > 0), 0.5) - 0.6744897501960817) < 1e-12
       AS halfnormal_median_exact
  FROM r;
-- Truncated uniform: U(0,10) | (3 < X < 7) is U(3,7); its 25% quantile is 4.
WITH r AS (SELECT provsql.uniform(0, 10) AS x)
SELECT abs(provsql.quantile(x | (x > 3 AND x < 7), 0.25) - 4) < 1e-12
       AS trunc_uniform_q25_exact
  FROM r;
-- Memoryless exponential: Exp(1) | X > 5 is 5 + Exp(1); median 5 + ln 2.
WITH r AS (SELECT provsql.exponential(1) AS x)
SELECT abs(provsql.quantile(x | (x > 5), 0.5) - (5 + ln(2.0))) < 1e-9
       AS trunc_exponential_median_exact
  FROM r;

-- (10) All the analytic paths above hold with the MC fallback disabled:
-- they are exact, not sampled.
SET provsql.rv_mc_samples = 0;
SELECT provsql.quantile(provsql.normal(0, 1), 0.5) = 0 AS analytic_without_mc;
SELECT abs(provsql.quantile(provsql.erlang(2, 1), 1 - 3 * exp(-2.0)) - 2)
       < 1e-9 AS erlang_without_mc;
RESET provsql.rv_mc_samples;

-- (11) Compound circuits fall back to the seeded empirical MC quantile
-- (percentile_cont interpolation).  X + Y for i.i.d. N(0,1) is
-- N(0, √2): median 0, q975 = 1.96·√2 ≈ 2.771808...  The sum folds to a
-- bare normal under simplify_on_load, so force a genuinely compound
-- shape with a product instead: median of N(0,1)·N(0,1) is 0 by
-- symmetry.
SET provsql.monte_carlo_seed = 42;
WITH r AS (SELECT provsql.normal(0, 1) AS x, provsql.normal(0, 1) AS y)
SELECT abs(provsql.quantile(x * y, 0.5)) < 0.02 AS product_median_mc
  FROM r;
RESET provsql.monte_carlo_seed;

-- (12) Validation: p outside [0, 1] (or NaN) raises; NULLs propagate.
\set VERBOSITY terse
SELECT provsql.quantile(provsql.normal(0, 1), 1.5);
SELECT provsql.quantile(provsql.normal(0, 1), -0.1);
SELECT provsql.quantile(provsql.normal(0, 1), 'NaN');
\set VERBOSITY default
SELECT provsql.quantile(provsql.normal(0, 1), NULL) IS NULL AS null_p;
SELECT provsql.quantile(NULL::provsql.random_variable, 0.5) IS NULL AS null_rv;

-- (13) End-to-end through the planner hook: the WHERE predicate folds
-- into the row's provenance, and quantile(reading, 0.5, provenance())
-- reads the median of the truncated distribution.
CREATE TABLE q_sensors(id text, reading provsql.random_variable);
INSERT INTO q_sensors VALUES ('a', provsql.normal(0, 1));
SELECT add_provenance('q_sensors');
CREATE TABLE q_result AS
  SELECT id, quantile(reading, 0.5, provenance()) AS med
    FROM q_sensors WHERE reading > 0;
SELECT remove_provenance('q_result');
SELECT id, abs(med - 0.6744897501960817) < 1e-12 AS median_within_tolerance
  FROM q_result ORDER BY id;
DROP TABLE q_result;
DROP TABLE q_sensors;

SELECT 'ok'::text AS continuous_quantile_done;
