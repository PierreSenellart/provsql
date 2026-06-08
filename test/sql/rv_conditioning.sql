\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;
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
