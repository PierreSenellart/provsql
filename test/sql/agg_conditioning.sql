\set ECHO none
\pset format unaligned

-- SUM(x) | C conditions a discrete aggregate's distribution on an event,
-- returning a conditioned agg_token that flows onward: expected / variance /
-- moment report the conditional distribution.  It reuses the conditional
-- agg_raw_moment machinery (the same evaluator as the two-argument
-- expected(agg, condition) form).

CREATE TABLE s(g int, x int, p float);
INSERT INTO s VALUES (1,10,0.5),(1,20,0.5);
SELECT add_provenance('s');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM s; END $$;

-- SUM(x) over the two independent probabilistic rows.
CREATE TABLE agg AS SELECT g, sum(x) AS sx FROM s GROUP BY g;

-- Capture the x=10 row's token (an inert fetch; needs the rewriter on).
CREATE TEMP TABLE tk AS
  SELECT (SELECT provenance() FROM s WHERE x=10) AS tok10;

-- Read the stored aggregate with the rewriter off (no extra provsql layer).
SET provsql.active = off;

-- Unconditional E[SUM] = .5*10 + .5*20 = 15.
-- Conditioned on the x=10 row being present, SUM = 10 + 20*Bernoulli(.5):
--   E[SUM | x=10] = 20, Var[SUM | x=10] = 20^2 * .25 = 100.
-- The operator form agrees with the two-argument conditional form.
SELECT round(expected(agg.sx)::numeric,4)            AS e_uncond,
       round(expected(agg.sx | tk.tok10)::numeric,4) AS e_cond_operator,
       round(expected(agg.sx, tk.tok10)::numeric,4)  AS e_cond_twoarg,
       round(variance(agg.sx | tk.tok10)::numeric,4) AS var_cond
FROM agg, tk;

-- The conditioned aggregate is the composable two-child conditioned gate.
SELECT get_gate_type((agg.sx | tk.tok10)::uuid) AS gate,
       array_length(get_children((agg.sx | tk.tok10)::uuid),1) AS nchildren
FROM agg, tk;

-- Folding (SUM|A)|A = SUM|A: conditioning twice on the same event is
-- idempotent (the gate stays one level deep).
SELECT round(expected((agg.sx | tk.tok10) | tk.tok10)::numeric,4) AS e_idempotent,
       array_length(get_children(((agg.sx | tk.tok10) | tk.tok10)::uuid),1) AS nchildren_folded
FROM agg, tk;

RESET provsql.active;

SELECT remove_provenance('s');
DROP TABLE agg;
DROP TABLE s;
