\set ECHO none
\pset format unaligned

-- Case Study 8: ProvSQL as a Probability Calculator.
-- Backs the five worked problems of doc/source/user/casestudy8.rst: discrete
-- Bayes via the | conditioning operator, correlation-aware disjunction,
-- the probability_evaluate method portfolio, continuous truncation moments,
-- and conditional expectation of a probabilistic aggregate.

-- Setup (mirrors doc/casestudy8/setup.sql).
CREATE TABLE screening(grp int, disease boolean, positive boolean, p float);
INSERT INTO screening VALUES
  (1, true,  true,  0.009), (1, true,  false, 0.001),
  (1, false, true,  0.0495), (1, false, false, 0.9405);
SELECT repair_key('screening', 'grp');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM screening; END $$;

CREATE TABLE risk(id text, p float);
INSERT INTO risk VALUES ('shared', 0.5), ('a1', 0.6), ('a2', 0.7);
SELECT add_provenance('risk');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM risk; END $$;

CREATE TABLE cases(day int, region text, n int, p float);
INSERT INTO cases VALUES (1,'North',3,0.5),(1,'North',4,0.5),(1,'South',2,0.8);
SELECT add_provenance('cases');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM cases; END $$;

-- Problem 1: the base-rate fallacy. P(disease|positive)=0.1538 != sensitivity.
WITH e AS (
  SELECT (SELECT provenance() FROM screening WHERE disease  GROUP BY grp) AS d,
         (SELECT provenance() FROM screening WHERE positive GROUP BY grp) AS pos)
SELECT round(probability_evaluate(d)::numeric,4)        AS p_disease,
       round(probability_evaluate(pos)::numeric,4)      AS p_positive,
       round(probability_evaluate(d | pos)::numeric,4)  AS p_disease_given_pos,
       round(probability_evaluate(pos | d)::numeric,4)  AS p_pos_given_disease
FROM e;

-- Problem 2: correlation that matters.  Exact A v B = 0.44; the independence
-- formula would give 0.545.
WITH t AS (
  SELECT (SELECT provenance() FROM risk WHERE id='shared') AS f,
         (SELECT provenance() FROM risk WHERE id='a1')     AS a1,
         (SELECT provenance() FROM risk WHERE id='a2')     AS a2)
SELECT round(probability_evaluate(provenance_times(f,a1))::numeric,4) AS p_a,
       round(probability_evaluate(provenance_times(f,a2))::numeric,4) AS p_b,
       round(probability_evaluate(
         provenance_plus(ARRAY[provenance_times(f,a1),
                               provenance_times(f,a2)]))::numeric,4)   AS p_or_exact,
       round((1-(1-0.3)*(1-0.35))::numeric,4)                          AS p_or_naive
FROM t;

-- Problem 3: the method portfolio agrees (exact methods exactly; MC within
-- tolerance under a pinned seed).
SET provsql.monte_carlo_seed = 42;
WITH t AS (
  SELECT provenance_plus(ARRAY[
           (SELECT provenance() FROM risk WHERE id='a1'),
           (SELECT provenance() FROM risk WHERE id='a2')]) AS tok)
SELECT round(probability_evaluate(tok)::numeric,4)                 AS p_default,
       round(probability_evaluate(tok,'independent')::numeric,4)   AS p_independent,
       abs(probability_evaluate(tok,'monte-carlo','200000') - 0.88) < 0.01 AS mc_ok
FROM t;

-- Problem 4: continuous posterior (closed-form truncated normal).
SET provsql.rv_mc_samples = 0;
WITH r AS (SELECT normal(20,5) AS x)
SELECT round(expected(r.x)::numeric,3)               AS e_x,
       round(expected(r.x | (r.x > 25))::numeric,3)  AS e_given_referral,
       round(variance(r.x | (r.x > 25))::numeric,3)  AS var_given_referral,
       (support(r.x | (r.x > 25))).lo                AS support_lower
FROM r;

-- Problem 4 (cont.): conditional probability of one event given another.
-- P(x>30 | x>25) = P(x>30)/P(x>25) (correlation-aware; {x>30} subset {x>25}),
-- exact/closed-form even at rv_mc_samples = 0.
WITH r AS (SELECT normal(20,5) AS x)
SELECT round(probability((r.x > 30) | (r.x > 25))::numeric,4) AS p_severe_given_referred,
       abs(probability((r.x > 30) | (r.x > 25))
           - probability(r.x > 30) / probability(r.x > 25)) < 1e-9 AS matches_bayes
FROM r;

-- Problem 5: conditional expectation of a probabilistic aggregate.  The
-- moments are materialised under the rewriter, then read back (the result
-- table carries no content-addressed token, so the output is deterministic).
CREATE TABLE casesum AS SELECT region, sum(n) AS total FROM cases GROUP BY region;
CREATE TABLE p5 AS
  SELECT cs.region,
         expected(cs.total) AS e_total,
         expected(cs.total | (SELECT provenance() FROM cases WHERE n=4)) AS e_given
  FROM casesum cs WHERE cs.region='North';
SET provsql.active = off;
SELECT region, round(e_total::numeric,2) AS e_total,
       round(e_given::numeric,2) AS e_total_given_highday
FROM p5;
RESET provsql.active;
DROP TABLE p5;

-- Problem 7: a skewed waiting time (log-normal), all closed-form under
-- rv_mc_samples = 0.  Quantiles read the skew; exp(normal(mu,sigma)) folds
-- to the same lognormal(mu,sigma) so the mean matches; the lognormal-vs-
-- lognormal comparison has a registered closed form.
WITH m AS (SELECT lognormal(1.6, 0.42) AS incubation)
SELECT round(quantile(incubation, 0.5)::numeric, 2)  AS median_days,
       round(quantile(incubation, 0.95)::numeric, 2) AS p95_days,
       round(expected(incubation)::numeric, 2)       AS mean_days
FROM m;
SELECT abs(expected(exp(normal(1.6, 0.42))) - expected(lognormal(1.6, 0.42)))
         < 1e-9 AS exp_normal_folds_to_lognormal;
WITH s AS (SELECT lognormal(1.6, 0.42) AS wild, lognormal(1.9, 0.42) AS variant)
SELECT round(probability(wild > variant)::numeric, 2) AS p_wild_longer FROM s;

-- Problem 8: discrete counts (Poisson / Binomial enumerate exact mass) and a
-- Beta posterior for an unknown rate (mean closed-form, quantiles by
-- incomplete-beta bisection).  Still analytic (rv_mc_samples = 0).
WITH d AS (SELECT poisson(6) AS cases)
SELECT round(expected(cases)::numeric, 2)         AS mean_cases,
       round(probability(cases > 10)::numeric, 3) AS p_alert
FROM d;
WITH b AS (SELECT binomial(20, 0.3) AS positives)
SELECT round(probability(positives >= 10)::numeric, 3) AS p_ten_plus FROM b;
SELECT round(expected(beta(3, 7))::numeric, 2)       AS rate_estimate,
       round(quantile(beta(3, 7), 0.05)::numeric, 2) AS credible_lo,
       round(quantile(beta(3, 7), 0.95)::numeric, 2) AS credible_hi;
RESET provsql.rv_mc_samples;

SELECT remove_provenance('casesum');
SELECT remove_provenance('risk');
SELECT remove_provenance('cases');
SELECT remove_provenance('screening');
DROP TABLE casesum; DROP TABLE screening; DROP TABLE risk; DROP TABLE cases;
