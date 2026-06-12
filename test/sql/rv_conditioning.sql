\set ECHO none
\pset format unaligned
SET provsql.rv_mc_samples = 0;  -- closed-form only

-- X | C conditions a random_variable on an event, returning a conditioned
-- distribution that flows onward: expected / variance / moment / support
-- report the conditional distribution, the value can be stored in a column,
-- and nested conditioning folds.  It reuses the same conditional evaluator
-- as the two-argument expected(rv, condition) form.

-- Truncated normal N(0,1) | (X>3): the operator form agrees with the
-- two-argument conditional form exactly (same evaluator).
WITH r AS (SELECT normal(0,1) AS rv)
SELECT (expected(r.rv | rv_cmp_gt(r.rv, as_random(3)))
          = expected(r.rv, rv_cmp_gt(r.rv, as_random(3)))) AS expected_matches,
       (variance(r.rv | rv_cmp_gt(r.rv, as_random(3)))
          = variance(r.rv, rv_cmp_gt(r.rv, as_random(3)))) AS variance_matches
FROM r;

-- Conditioned support is the truncation interval (3, +Infinity).
WITH r AS (SELECT normal(0,1) AS rv)
SELECT s.lo AS support_lo, s.hi AS support_hi
FROM r, support(r.rv | rv_cmp_gt(r.rv, as_random(3))) s;

-- Folding (X|A)|B = X|(A∧B): U(0,10) truncated to (3,7) has mean 5 and
-- variance (7-3)^2/12 = 16/12.
WITH r AS (SELECT uniform(0,10) AS rv)
SELECT round(expected((r.rv | rv_cmp_gt(r.rv,as_random(3)))
                       | rv_cmp_lt(r.rv,as_random(7)))::numeric,6) AS mean_folded,
       round(variance((r.rv | rv_cmp_gt(r.rv,as_random(3)))
                       | rv_cmp_lt(r.rv,as_random(7)))::numeric,6) AS var_folded
FROM r;

-- The conditioned distribution flows onward: stored in a random_variable
-- column and queried later.  U(0,10) | (X>5) is U truncated to (5,10),
-- mean 7.5, support (5,10).
CREATE TABLE dist AS
  WITH r AS (SELECT uniform(0,10) AS rv)
  SELECT (r.rv | rv_cmp_gt(r.rv, as_random(5))) AS x FROM r;
SELECT round(expected(x)::numeric,4) AS e_stored,
       (support(x)).lo AS support_lo, (support(x)).hi AS support_hi
FROM dist;
DROP TABLE dist;

-- The gate is the composable two-child [target, condition] conditioned shape
-- (distinct from the uuid carrier's three-child terminal gate).
WITH r AS (SELECT normal(0,1) AS rv)
SELECT get_gate_type((r.rv | rv_cmp_gt(r.rv, as_random(3)))::uuid) AS gate,
       array_length(
         get_children((r.rv | rv_cmp_gt(r.rv, as_random(3)))::uuid), 1) AS nchildren
FROM r;

-- A conditioned distribution is not a Boolean event: probability_evaluate
-- reports a clear error rather than a probability.
WITH r AS (SELECT normal(0,1) AS rv)
SELECT probability_evaluate((r.rv | rv_cmp_gt(r.rv, as_random(3)))::uuid) FROM r;

-- Arithmetic over conditioned distributions: f(X|A, Y|B) = f(X,Y) | (A ∧ B).
-- The conditioning is lifted to the whole expression on the conjunction of the
-- evidence; the conditional moment is then evaluated by MC rejection (fixed
-- seed for reproducibility).
SET provsql.rv_mc_samples = 200000;
SET provsql.monte_carlo_seed = 42;

-- Independent factor: (X|X>0) * Y, Y ~ U(0,1) independent.
--   E = E[X|X>0] * E[Y] = 0.7979 * 0.5 = 0.3989.
WITH r AS (SELECT normal(0,1) AS x, uniform(0,1) AS y)
SELECT abs(expected((r.x | (r.x > 0)) * r.y) - 0.3989) < 0.01 AS indep_product_ok
FROM r;

-- SHARED evidence (the soundness case): (Z|Z>0) * (Z|Z>0) = (Z*Z) | (Z>0).
--   E[Z^2 | Z>0] = 1.0 (half-normal second moment), NOT the independent
--   shortcut E[Z|Z>0]^2 = 0.7979^2 = 0.6366.  Pinning both bounds proves the
--   shared evidence is not mistaken for independence.
WITH r AS (SELECT normal(0,1) AS z)
SELECT abs(expected((r.z | (r.z > 0)) * (r.z | (r.z > 0))) - 1.0) < 0.03
         AS shared_evidence_correct,
       abs(expected((r.z | (r.z > 0)) * (r.z | (r.z > 0))) - 0.6366) > 0.2
         AS not_the_independent_shortcut
FROM r;

-- Different evidence: (X|X>0) + (Y|Y>0.5) = (X+Y) | (X>0 AND Y>0.5).
--   E = E[X|X>0] + E[Y|Y>0.5] = 0.7979 + 0.75 = 1.5479.
WITH r AS (SELECT normal(0,1) AS x, uniform(0,1) AS y)
SELECT abs(expected((r.x | (r.x > 0)) + (r.y | (r.y > 0.5))) - 1.5479) < 0.02
         AS distinct_evidence_ok
FROM r;
RESET provsql.rv_mc_samples;
RESET provsql.monte_carlo_seed;
