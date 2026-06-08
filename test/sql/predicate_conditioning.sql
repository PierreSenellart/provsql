\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;
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
