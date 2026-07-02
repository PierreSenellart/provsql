\set ECHO none
\pset format unaligned
SET provsql.rv_mc_samples = 0;

-- Natural Boolean-predicate conditioning: the right operand of | (and the
-- prefix | argument) may be a Boolean combination of random_variable /
-- aggregate comparisons, which the ProvSQL planner converts to a condition
-- gate.  So "X | (X > 3)" stands for "X | rv_cmp_gt(X, as_random(3))", and the
-- same principle works for every carrier (uuid / random_variable / agg_token)
-- and the prefix whole-tuple form.  Parentheses around the predicate are
-- required: a custom | binds tighter than >, so "X | X > 3" would parse as
-- "(X | X) > 3".

-- random_variable: truncation written naturally agrees with the gate form;
-- compound AND predicates work (De Morgan via provenance_times / _plus).
WITH r AS (SELECT normal(0,1) AS rv, uniform(0,10) AS u)
SELECT (expected(r.rv | (r.rv > 3))
          = expected(r.rv | rv_cmp_gt(r.rv, as_random(3)))) AS rv_predicate_matches,
       round(expected(r.u | (r.u > 3 AND r.u < 7))::numeric,6) AS rv_compound_mean
FROM r;

-- agg_token: condition an aggregate on a HAVING-style predicate.  The rewriter
-- must be on (it converts the predicate), so materialise the scalar result and
-- read it back.  E[SUM | SUM>25] = 30 (only the both-present world, SUM=30,
-- qualifies); E[SUM] = 15 unconditionally.
CREATE TABLE s(g int, x int, p float);
INSERT INTO s VALUES (1,10,0.5),(1,20,0.5);
SELECT add_provenance('s');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM s; END $$;
CREATE TABLE agg AS SELECT g, sum(x) AS sx FROM s GROUP BY g;
CREATE TABLE agg_res AS
  SELECT expected(agg.sx | (agg.sx > 25)) AS e_cond, expected(agg.sx) AS e_uncond
  FROM agg;
SET provsql.active = off;
SELECT round(e_cond::numeric,4) AS agg_predicate,
       round(e_uncond::numeric,4) AS agg_uncond FROM agg_res;
RESET provsql.active;

-- prefix | (predicate): whole-tuple output conditioning on a Boolean event.
-- The row's existence is independent of sensor>3, so P stays the row's .8.
CREATE TABLE readings(id int, sensor random_variable, p float);
INSERT INTO readings VALUES (1, normal(0,1), 0.8);
SELECT add_provenance('readings');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM readings; END $$;
CREATE TABLE pc AS SELECT id, | (sensor > 3) FROM readings;
SET provsql.active = off;
SELECT id, get_gate_type(provsql) AS gate,
       round(probability_evaluate(provsql)::numeric,4) AS p_cond
FROM pc ORDER BY id;
RESET provsql.active;

SELECT remove_provenance('s');
SELECT remove_provenance('readings');
DROP TABLE agg_res;
DROP TABLE agg;
DROP TABLE s;
DROP TABLE pc;
DROP TABLE readings;

-- Mixed predicate: an ordinary (regular-column) comparison combined with a
-- probabilistic one.  The regular comparison becomes a deterministic
-- indicator (gate_one / gate_zero) per the HAVING-provenance semantics, so
-- "SUM(x) | (SUM(x) > 25 AND region = 'north')" conditions on SUM>25 when the
-- group is in the north (indicator 1 -> E = 30) and on the impossible event
-- otherwise.  A PURELY regular predicate is rejected (it is an ordinary
-- filter, not a conditioning event).
CREATE TABLE sr(g int, region text, x int, p float);
INSERT INTO sr VALUES (1,'north',10,0.5),(1,'north',20,0.5);
SELECT add_provenance('sr');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM sr; END $$;
CREATE TABLE aggr AS SELECT g, region, sum(x) AS sx FROM sr GROUP BY g, region;
CREATE TABLE mixed_res AS
  SELECT expected(aggr.sx | (aggr.sx > 25 AND aggr.region = 'north')) AS e_north
  FROM aggr;
SET provsql.active = off;
SELECT round(e_north::numeric,4) AS e_mixed_north FROM mixed_res;
RESET provsql.active;

-- Purely regular predicate: rejected with a clear message.
SELECT expected(aggr.sx | (aggr.region = 'north')) FROM aggr;

SELECT remove_provenance('sr');
DROP TABLE mixed_res;
DROP TABLE aggr;
DROP TABLE sr;

-- Conditioning TWO comparison events with | : "(A) | (B)".  A random_variable
-- / agg_token comparison is statically boolean-typed, so neither "uuid | uuid"
-- nor "uuid | boolean" resolves; the boolean | boolean operator (planner-hook
-- lowered to cond(A, B)) makes probability((A) | (B)) type-check and return the
-- correlation-aware Pr(A ∧ B) / Pr(B).  The two comparisons share the x leaf,
-- so the joint is NOT the product of marginals.  The monotone-shared-scalar
-- fast path resolves the joint analytically (via the CDF), so the answer is
-- exact and holds even with rv_mc_samples = 0 (below).
--
-- x ~ N(1500, 400): P(x>=2000)=0.10565, P(x>=1000)=0.89435; since {x>=2000} is
-- a subset of {x>=1000}, P(x>=2000 | x>=1000) = P(x>=2000)/P(x>=1000) = 0.11813.
SET provsql.rv_mc_samples = 100000;
SET provsql.monte_carlo_seed = 42;
WITH r AS (SELECT normal(1500, 400) AS x)
SELECT round(probability((x >= 2000) | (x >= 1000))::numeric, 5) AS p_cond,
       abs(probability((x >= 2000) | (x >= 1000))
           - probability(x >= 2000) / probability(x >= 1000)) < 1e-9
         AS matches_bayes_ratio
FROM r;

-- "A | B" is a first-class event token (uuid) in every position: a projected
-- column surfaces the token, which probability_evaluate consumes directly.
WITH r AS (SELECT normal(1500, 400) AS x)
SELECT pg_typeof((x >= 2000) | (x >= 1000)) AS event_type FROM r;

-- Conditioning on an independent event is a no-op: P(A | B) = P(A) when A and B
-- share no random_variable leaf.
WITH r AS (SELECT normal(1500, 400) AS x, normal(500, 400) AS y)
SELECT abs(probability((x >= 2000) | (y >= 1000)) - probability(x >= 2000)) < 1e-9
         AS independent_condition_noop
FROM r;

-- The shared-leaf joint (and hence the conditioning) is analytical: it is EXACT
-- even under rv_mc_samples = 0.  Before the fast path was allowed to run at
-- rv_mc_samples = 0, each comparison collapsed to its independent marginal and
-- this silently returned Pr(A)·Pr(B) instead of the correlation-aware joint.
SET provsql.rv_mc_samples = 0;
WITH r AS (SELECT normal(1500, 400) AS x)
SELECT abs(probability((x >= 2000) | (x >= 1000))
           - probability(x >= 2000) / probability(x >= 1000)) < 1e-9
         AS cond_exact_at_zero_samples,
       abs(probability((x >= 2000) AND (x >= 1000)) - probability(x >= 2000)) < 1e-9
         AS conjunction_exact_at_zero_samples
FROM r;

-- A correlated RV-vs-RV joint sharing a leaf (x compared to two independent
-- RVs) is resolved analytically via the shared-pivot joint table: the k
-- comparisons share the pivot x, so the 2^k joint is a table of pivot-
-- conjunction integrals -- exact even under rv_mc_samples = 0.  For i.i.d.
-- N(0,1), Pr(x>y ∧ x>z) = 1/3 (x is the max) and Pr(x>z) = 1/2, so the
-- conditional Pr((x>y) | (x>z)) = 2/3.
WITH r AS (SELECT normal(0, 1) AS x, normal(0, 1) AS y, normal(0, 1) AS z)
SELECT abs(probability((x > y) | (x > z)) - 2.0/3) < 1e-6
         AS pivot_joint_cond_exact_at_zero,
       abs(probability((x > y) AND (x > z)) - 1.0/3) < 1e-6
         AS pivot_joint_and_exact_at_zero
FROM r;

-- The moment side of the same conjunction: E[X | X>Y ∧ X>Z] conditions X on
-- being the max of three i.i.d. U(0,1), i.e. X | A ~ Beta(3,1): mean 3/4,
-- variance 3/80.  The guard-partition integrator evaluates the ratio of the
-- two pivot-conjunction integrals exactly, no Monte Carlo.
WITH r AS (SELECT uniform(0,1) AS x, uniform(0,1) AS y, uniform(0,1) AS z)
SELECT abs(expected(x | (x > y AND x > z)) - 0.75) < 1e-6 AS cond_moment_exact,
       abs(variance(x | (x > y AND x > z)) - 3.0/80) < 1e-6 AS cond_var_exact
FROM r;
RESET provsql.rv_mc_samples;
RESET provsql.monte_carlo_seed;
