\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- The apx-safe corner of the HAVING trichotomy (Re & Suciu 2009, VLDB J.).
-- An aggregate whose value-support exceeds BOTH exact weighted-sum evaluators --
-- the dense SumCmpEvaluator (range > kMaxSumRange) and the sparse marginal-vector
-- engine (distinct values > kMaxSumSupport) -- cannot be resolved exactly, and
-- provsql_having's threshold-lineage expansion does not terminate for it.  An
-- (eps,delta) request instead samples the comparator directly (the gate_agg arm
-- of the Monte-Carlo sampler: the DKLR stopping rule for 'relative', fixed-sample
-- MC for 'additive' / 'monte-carlo') -- a sound FPRAS, independent of magnitude.
-- Sound for every aggregate the sampler evaluates faithfully: SUM / AVG / MIN /
-- MAX (all but COUNT, whose arm ignores the 0/1 contributor value).
SET provsql.monte_carlo_seed = 6353;

-- (1) SUM, with a known exact probability.  25 tuples with weights 2^0..2^24,
-- each present independently with probability 1/2.  The lower 24 weights sum to
-- 2^24 - 1 < 2^24, so sum(x) >= 2^24 holds iff the 2^24 tuple is present --
-- probability exactly 1/2 -- yet the support is 2^25 (both caps blown), so the
-- comparator genuinely survives to the sampler.
CREATE TABLE fpw(x bigint);
INSERT INTO fpw SELECT (2::bigint)^i FROM generate_series(0,24) i;
SELECT add_provenance('fpw');
DO $$ BEGIN PERFORM provsql.set_prob(provsql.provenance(),0.5) FROM provsql_test.fpw; END $$;

SELECT provenance() AS tok FROM fpw HAVING sum(x) >= 16777216
\gset
SELECT abs(probability_evaluate(:'tok','relative','epsilon=0.05,delta=0.01') - 0.5)
       <= 0.05 * 0.5 AS sum_relative_in_tol;
SELECT abs(probability_evaluate(:'tok','additive','epsilon=0.03,delta=0.01') - 0.5)
       <= 0.03 AS sum_additive_in_tol;
SELECT abs(probability_evaluate(:'tok','monte-carlo','200000') - 0.5) <= 0.03
       AS sum_monte_carlo_in_tol;

SELECT remove_provenance('fpw');
DROP TABLE fpw;

-- (2) AVG / MIN / MAX route to the sampler on the same principle.  Force the
-- comparators to survive the exact pre-passes (cmp_probability_evaluation off)
-- so the sample-routing itself is exercised on a small instance whose exact
-- value the Boolean expansion still computes, and check the sampled estimate
-- matches it -- including NULL rows, which AVG / MIN / MAX exclude (the property
-- that makes them faithful, unlike COUNT).
SET provsql.cmp_probability_evaluation = off;
CREATE TABLE agg(x int);
INSERT INTO agg VALUES (1),(2),(3),(5),(6),(7),(NULL),(NULL);
SELECT add_provenance('agg');
DO $$ BEGIN PERFORM provsql.set_prob(provsql.provenance(),0.5) FROM provsql_test.agg; END $$;

SELECT provenance() AS atok FROM agg HAVING avg(x) >= 4
\gset
SELECT probability_evaluate(:'atok') AS avg_exact
\gset
SELECT abs(probability_evaluate(:'atok','monte-carlo','300000') - :avg_exact)
       <= 0.02 AS avg_mc_faithful;

SELECT provenance() AS ntok FROM agg HAVING min(x) <= 2
\gset
SELECT probability_evaluate(:'ntok') AS min_exact
\gset
SELECT abs(probability_evaluate(:'ntok','monte-carlo','300000') - :min_exact)
       <= 0.02 AS min_mc_faithful;

SELECT provenance() AS xtok FROM agg HAVING max(x) >= 6
\gset
SELECT probability_evaluate(:'xtok') AS max_exact
\gset
SELECT abs(probability_evaluate(:'xtok','monte-carlo','300000') - :max_exact)
       <= 0.02 AS max_mc_faithful;

-- COUNT is sample-faithful too: the contributor value gate is the 0/1 indicator
-- (0 for a NULL row), so the sampler counts count(*) and count(x) correctly --
-- count(x) over this NULL-bearing column excludes the two NULL rows.
SELECT provenance() AS ctok FROM agg HAVING count(*) >= 4
\gset
SELECT probability_evaluate(:'ctok') AS cstar_exact
\gset
SELECT abs(probability_evaluate(:'ctok','monte-carlo','300000') - :cstar_exact)
       <= 0.02 AS count_star_mc_faithful;

SELECT provenance() AS ktok FROM agg HAVING count(x) >= 4
\gset
SELECT probability_evaluate(:'ktok') AS ccol_exact
\gset
SELECT abs(probability_evaluate(:'ktok','monte-carlo','300000') - :ccol_exact)
       <= 0.02 AS count_col_mc_faithful;
RESET provsql.cmp_probability_evaluation;

SELECT remove_provenance('agg');
DROP TABLE agg;
