\set ECHO none
-- Continuous-RV moment transparency: the analytic moment surface (expected /
-- variance / moment) is approximate by NATURE when no closed form exists
-- (it falls back to Monte Carlo, controlled by provsql.rv_mc_samples), but it
-- must never be *silently* so.  At verbose_level >= 5 -- the tier Studio uses
-- for evaluation, the same one carrying the probability-side approximation
-- NOTICEs -- a fallback to MC emits a NOTICE so an estimate is never mistaken
-- for a closed-form moment.  See src/Expectation.cpp mc_samples_or_throw.
\pset format unaligned
SET search_path TO provsql_test,provsql;
SET provsql.monte_carlo_seed = 42;

-- expected(rv * rv) over a SHARED base RV has overlapping footprints and no
-- closed form, so it takes the MC fallback.  Closed-form moments (a bare
-- Normal) do NOT, so they stay quiet -- the NOTICE distinguishes the two.
SET provsql.rv_mc_samples = 20000;
SET provsql.verbose_level = 5;

-- closed form: no NOTICE.
SELECT expected(provsql.normal(2.5, 0.5)) AS closed_form_mean;

-- MC fallback: a transparency NOTICE fires, and the estimate is finite and in
-- the right ballpark (E[Z^2] = Var = 1 for Z ~ Normal(0,1)).
SELECT abs(expected(rv * rv) - 1.0) < 0.1 AS mc_estimate_in_range
FROM (SELECT provsql.normal(0, 1) AS rv) t;

SET provsql.verbose_level = 0;

-- At the default verbose level the fallback is quiet (still an estimate, just
-- not announced): no NOTICE here.
SELECT abs(expected(rv * rv) - 1.0) < 0.1 AS mc_estimate_quiet
FROM (SELECT provsql.normal(0, 1) AS rv) t;

RESET provsql.verbose_level;
RESET provsql.rv_mc_samples;
RESET provsql.monte_carlo_seed;
