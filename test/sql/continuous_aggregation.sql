\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Phase 1 of the SUM-over-RV story: provsql.sum is the explicit
-- aggregate that takes a random_variable per row and returns a
-- random_variable representing the (provenance-weighted) sum.  In a
-- provenance-tracked query the planner-hook rewriter wraps each row's
-- argument in mixture(prov_token, x, as_random(0)) so the effective
-- semantics are SUM(x) = sum_i 1{phi_i} * X_i (semimodule provenance,
-- M = random_variable).  See aggregation-of-rvs.md.

-- Pin the MC RNG and sample budget so the cases that fall back to MC
-- (coupling-through-shared-atoms and coupling-through-shared-RVs) are
-- reproducible at a tolerance loose enough to absorb sampler noise but
-- tight enough to distinguish the coupled / decoupled means.
SET provsql.monte_carlo_seed = 1;
SET provsql.rv_mc_samples    = 50000;

-- ---------------------------------------------------------------------
-- 1.  Basic SUM over an RV column with deterministic provenance.
-- ---------------------------------------------------------------------
-- Three rows of normal(mu_i, 1) with the default trivially-true
-- provenance (gate_input default prob 1.0 in the Boolean evaluator).
-- The mixture(prov, X, as_random(0)) wrappers reduce to X under prob=1,
-- so the analytical evaluator collapses to E[X1+X2+X3] = sum mu_i and
-- Var[X1+X2+X3] = 3 (independent unit variances).  Both pinned exactly.

CREATE TABLE rv_basic(label text, x random_variable);
INSERT INTO rv_basic VALUES
  ('a', provsql.normal(1, 1)),
  ('b', provsql.normal(2, 1)),
  ('c', provsql.normal(3, 1));
SELECT add_provenance('rv_basic');

CREATE TABLE rv_basic_sum AS SELECT provsql.sum(x) AS s FROM rv_basic;
SELECT remove_provenance('rv_basic_sum');

-- Structure: the rewriter produces gate_arith(PLUS, [mixture, mixture, mixture]).
SELECT get_gate_type(s::uuid)                         AS basic_root_kind,
       array_length(get_children(s::uuid), 1)         AS basic_root_arity,
       get_gate_type((get_children(s::uuid))[1])      AS basic_child0_kind,
       get_gate_type((get_children(s::uuid))[2])      AS basic_child1_kind,
       get_gate_type((get_children(s::uuid))[3])      AS basic_child2_kind
  FROM rv_basic_sum;

SELECT abs(provsql.expected(s) - 6.0) < 1e-9   AS basic_mean,
       abs(provsql.variance(s) - 3.0) < 1e-9   AS basic_variance
  FROM rv_basic_sum;

DROP TABLE rv_basic_sum;
DROP TABLE rv_basic;

-- ---------------------------------------------------------------------
-- 2.  SUM under uncertain provenance.
-- ---------------------------------------------------------------------
-- Each row carries a Bernoulli atom with probability p_i; X_i is
-- normal(mu_i, 1).  Linearity of expectation gives
--   E[sum] = sum_i p_i * mu_i
-- exactly, regardless of independence between rows; rec_expectation
-- collapses to the closed-form mixture branch and pins to 1e-9.

CREATE TABLE rv_uncertain(label text, x random_variable);
INSERT INTO rv_uncertain VALUES
  ('a', provsql.normal(1.0, 1.0)),
  ('b', provsql.normal(2.0, 1.0)),
  ('c', provsql.normal(3.0, 1.0));
SELECT add_provenance('rv_uncertain');

DO $$
BEGIN
  PERFORM set_prob(provenance(), 0.5) FROM rv_uncertain WHERE label = 'a';
  PERFORM set_prob(provenance(), 0.4) FROM rv_uncertain WHERE label = 'b';
  PERFORM set_prob(provenance(), 0.3) FROM rv_uncertain WHERE label = 'c';
END
$$;

CREATE TABLE rv_uncertain_sum AS
  SELECT provsql.sum(x) AS s FROM rv_uncertain;
SELECT remove_provenance('rv_uncertain_sum');

-- E[SUM] = 0.5*1 + 0.4*2 + 0.3*3 = 0.5 + 0.8 + 0.9 = 2.2
SELECT abs(provsql.expected(s) - 2.2) < 1e-9 AS uncertain_mean
  FROM rv_uncertain_sum;

-- Variance: each row's mixture(b_i, N(mu_i,1), 0) has
--   Var = p_i*(1+mu_i^2) + (1-p_i)*0 - (p_i*mu_i)^2
--       = p_i*(1+mu_i^2) - p_i^2*mu_i^2
-- a = 0.5: 0.5*(1+1) - 0.25*1 = 1.0 - 0.25 = 0.75
-- b = 0.4: 0.4*(1+4) - 0.16*4 = 2.0 - 0.64 = 1.36
-- c = 0.3: 0.3*(1+9) - 0.09*9 = 3.0 - 0.81 = 2.19
-- Independent atoms => total variance 0.75 + 1.36 + 2.19 = 4.30
-- (the analytical evaluator's independence shortcut applies because
-- the per-row footprints {b_i, X_i} are disjoint).
SELECT abs(provsql.variance(s) - 4.30) < 1e-9 AS uncertain_variance
  FROM rv_uncertain_sum;

DROP TABLE rv_uncertain_sum;
DROP TABLE rv_uncertain;

-- ---------------------------------------------------------------------
-- 3.  Coupling through shared atoms.
-- ---------------------------------------------------------------------
-- Two rows whose provenance tokens are the SAME Bernoulli b
-- (perfectly correlated indicators) versus two rows with independent
-- Bernoullis b1, b2.  Each row's X is an INDEPENDENT normal(1, 1).
--
-- Coupled (shared b, P(b)=0.5):
--   SUM = I*(X1 + X2)
--   E[SUM] = 0.5 * 2 = 1.0
--   E[SUM^2] = 0.5 * E[(X1+X2)^2] = 0.5 * (Var(X1+X2) + E[X1+X2]^2)
--           = 0.5 * (2 + 4) = 3.0
--   Var[SUM] = 3 - 1 = 2.0
--
-- Decoupled (independent b1, b2 each P=0.5):
--   SUM = I1*X1 + I2*X2
--   E[SUM] = 0.5 + 0.5 = 1.0
--   Var[SUM] = sum of independent contributions = 2 * 0.75 = 1.5
--   (each mixture(b_i, N(1,1), 0) has variance 0.5*(1+1) - 0.25 = 0.75)
--
-- The means agree but the variances differ (2.0 vs 1.5).  The
-- decoupled side is closed-form (disjoint footprints).  The coupled
-- side falls back to MC because the FootprintCache picks up the shared
-- Bernoulli; tolerance 0.05 leaves headroom for sampler noise at
-- 50k samples while still excluding the decoupled answer.

CREATE TEMP TABLE bern_pair(b1 uuid, b2 uuid);
INSERT INTO bern_pair VALUES (public.uuid_generate_v4(), public.uuid_generate_v4());
SELECT create_gate((SELECT b1 FROM bern_pair), 'input');
SELECT create_gate((SELECT b2 FROM bern_pair), 'input');
SELECT set_prob((SELECT b1 FROM bern_pair), 0.5);
SELECT set_prob((SELECT b2 FROM bern_pair), 0.5);

CREATE TABLE rv_coupled(label text, x random_variable, provsql uuid);
INSERT INTO rv_coupled VALUES
  ('a', provsql.normal(1, 1), (SELECT b1 FROM bern_pair)),
  ('b', provsql.normal(1, 1), (SELECT b1 FROM bern_pair));

CREATE TABLE coupled_sum AS SELECT provsql.sum(x) AS s FROM rv_coupled;
SELECT remove_provenance('coupled_sum');

SELECT abs(provsql.expected(s) - 1.0) < 1e-9 AS coupled_mean,
       abs(provsql.variance(s) - 2.0) < 0.05 AS coupled_variance
  FROM coupled_sum;

CREATE TABLE rv_decoupled(label text, x random_variable, provsql uuid);
INSERT INTO rv_decoupled VALUES
  ('a', provsql.normal(1, 1), (SELECT b1 FROM bern_pair)),
  ('b', provsql.normal(1, 1), (SELECT b2 FROM bern_pair));

CREATE TABLE decoupled_sum AS SELECT provsql.sum(x) AS s FROM rv_decoupled;
SELECT remove_provenance('decoupled_sum');

SELECT abs(provsql.expected(s) - 1.0) < 1e-9 AS decoupled_mean,
       abs(provsql.variance(s) - 1.5) < 1e-9 AS decoupled_variance_exact
  FROM decoupled_sum;

DROP TABLE coupled_sum;
DROP TABLE decoupled_sum;
DROP TABLE rv_coupled;
DROP TABLE rv_decoupled;

-- ---------------------------------------------------------------------
-- 4.  Coupling through shared RVs.
-- ---------------------------------------------------------------------
-- Two rows whose x is the SAME normal(0, 1) gate (sharing a UUID).
-- Both rows always contribute the same draw -- when both indicators
-- fire, SUM = 2X, not X1+X2 with independent X1, X2.  The variance
-- distinguishes the shapes:
--
-- Shared X (deterministic provenance, both rows always present):
--   SUM = X + X = 2X, E[SUM] = 0, Var[SUM] = 4 * Var(X) = 4.
--
-- Independent X1, X2 (deterministic provenance):
--   SUM = X1 + X2, E[SUM] = 0, Var[SUM] = 2 * Var(X) = 2.

CREATE TEMP TABLE rv_anchor(u uuid);
INSERT INTO rv_anchor VALUES ((provsql.normal(0, 1))::uuid);

CREATE TABLE rv_shared(label text, x random_variable);
INSERT INTO rv_shared VALUES
  ('a', random_variable_make((SELECT u FROM rv_anchor))),
  ('b', random_variable_make((SELECT u FROM rv_anchor)));
SELECT add_provenance('rv_shared');

CREATE TABLE shared_rv_sum AS SELECT provsql.sum(x) AS s FROM rv_shared;
SELECT remove_provenance('shared_rv_sum');

SELECT abs(provsql.expected(s) - 0.0) < 1e-9 AS shared_rv_mean,
       abs(provsql.variance(s) - 4.0) < 0.1 AS shared_rv_variance
  FROM shared_rv_sum;

CREATE TABLE rv_distinct(label text, x random_variable);
INSERT INTO rv_distinct VALUES
  ('a', provsql.normal(0, 1)),
  ('b', provsql.normal(0, 1));
SELECT add_provenance('rv_distinct');

CREATE TABLE distinct_rv_sum AS SELECT provsql.sum(x) AS s FROM rv_distinct;
SELECT remove_provenance('distinct_rv_sum');

SELECT abs(provsql.expected(s) - 0.0) < 1e-9 AS distinct_rv_mean,
       abs(provsql.variance(s) - 2.0) < 1e-9 AS distinct_rv_variance_exact
  FROM distinct_rv_sum;

DROP TABLE shared_rv_sum;
DROP TABLE distinct_rv_sum;
DROP TABLE rv_shared;
DROP TABLE rv_distinct;

-- ---------------------------------------------------------------------
-- 5.  Empty group: WHERE filters out every row.
-- ---------------------------------------------------------------------
-- The aggregate's INITCOND='{}' fires the FINALFUNC even on an empty
-- group; the FINALFUNC returns as_random(0), the additive identity.
-- The result is a deterministic gate_value Dirac at 0 (mean 0, variance 0,
-- support {0}), the natural extension of the agg_token convention that
-- "no row included" => SUM = 0.

CREATE TABLE rv_empty(label text, x random_variable, keep boolean);
INSERT INTO rv_empty VALUES
  ('a', provsql.normal(1, 1), false),
  ('b', provsql.normal(2, 1), false);
SELECT add_provenance('rv_empty');

CREATE TABLE empty_sum AS
  SELECT provsql.sum(x) AS s FROM rv_empty WHERE keep;
SELECT remove_provenance('empty_sum');

SELECT get_gate_type(s::uuid)                AS empty_root_kind,
       get_extra(s::uuid)                    AS empty_root_extra,
       abs(provsql.expected(s) - 0.0) < 1e-9 AS empty_mean,
       abs(provsql.variance(s) - 0.0) < 1e-9 AS empty_variance
  FROM empty_sum;

DROP TABLE empty_sum;
DROP TABLE rv_empty;

-- ---------------------------------------------------------------------
-- 6.  COUNT(*) lift via sum(as_random(1)).
-- ---------------------------------------------------------------------
-- A SUM over Dirac-1 inputs is the lift of COUNT(*) into the
-- continuous-RV space: each row contributes mixture(prov_i, 1, 0),
-- which is the indicator of prov_i interpreted as a {0, 1}-valued RV.
-- The resulting gate_arith(PLUS, ...) realises the Poisson-binomial
-- distribution over {0..n} with weights P(prov_i).  Linearity of
-- expectation pins the mean to sum_i P(prov_i) exactly, which matches
-- expected(COUNT(*)) under the existing agg_token machinery -- the
-- two formulations agree on the first moment.

CREATE TABLE rv_count(label text);
INSERT INTO rv_count VALUES ('a'), ('b'), ('c');
SELECT add_provenance('rv_count');

DO $$
BEGIN
  PERFORM set_prob(provenance(), 0.2) FROM rv_count WHERE label = 'a';
  PERFORM set_prob(provenance(), 0.5) FROM rv_count WHERE label = 'b';
  PERFORM set_prob(provenance(), 0.7) FROM rv_count WHERE label = 'c';
END
$$;

CREATE TABLE count_lift_sum AS
  SELECT provsql.sum(provsql.as_random(1)) AS s FROM rv_count;
SELECT remove_provenance('count_lift_sum');

-- Structural check: gate_arith with one mixture per row.  The mixture
-- branches are both gate_value (Dirac-mixture shape), which is exactly
-- the shape the doc's Issue-3 simplifier is designed to collapse to a
-- single mulinput-over-key categorical block in a follow-up pass --
-- pinned here so the surface stays predictable.
SELECT get_gate_type(s::uuid)                          AS count_root_kind,
       array_length(get_children(s::uuid), 1)          AS count_root_arity,
       get_gate_type((get_children(s::uuid))[1])       AS count_child_kind,
       get_gate_type((get_children((get_children(s::uuid))[1]))[2])
                                                       AS count_branch_x_kind,
       get_gate_type((get_children((get_children(s::uuid))[1]))[3])
                                                       AS count_branch_y_kind
  FROM count_lift_sum;

-- E[SUM] = 0.2 + 0.5 + 0.7 = 1.4 (exact by linearity).
SELECT abs(provsql.expected(s) - 1.4) < 1e-9 AS count_lift_mean
  FROM count_lift_sum;

-- Variance: each Bernoulli indicator I_i has Var = p_i * (1 - p_i),
-- and since the per-row Bernoullis are independent, total variance
--   = 0.2*0.8 + 0.5*0.5 + 0.7*0.3 = 0.16 + 0.25 + 0.21 = 0.62.
-- The analytical evaluator's independence shortcut hits because each
-- mixture's footprint reduces to its own Bernoulli.
SELECT abs(provsql.variance(s) - 0.62) < 1e-9 AS count_lift_variance
  FROM count_lift_sum;

DROP TABLE count_lift_sum;
DROP TABLE rv_count;

-- ---------------------------------------------------------------------
-- 7.  AVG(random_variable) on a tracked table.
-- ---------------------------------------------------------------------
-- provsql.avg lifts the standard "AVG = SUM / COUNT" identity into the
-- random_variable algebra.  The planner-hook generalises the SUM-rewrite
-- to every RV-returning aggregate (dispatch on aggtype = random_variable
-- in make_aggregation_expression), so each row's argument is wrapped in
-- mixture(prov_i, X_i, as_random(0)) before reaching avg_rv_ffunc.  The
-- FFUNC then walks each mixture to recover prov_i and builds the
-- per-row denominator as mixture(prov_i, as_random(1), as_random(0)),
-- so num/denom share the SAME provenance footprint per row -- the
-- pattern is exactly "provsql.sum(x) / provsql.sum(provsql.as_random(1))"
-- emitted as a single token.

-- 7a.  Deterministic provenance.
--   AVG of N(1,1), N(2,1), N(3,1) under default prob=1.0:
--     SUM = N(6, 3); SUM_ones = 3; AVG = N(2, 1/3).
--   The DIV gate doesn't have a closed-form evaluator in general, so
--   expected/variance go through Monte Carlo; tolerance 0.05 leaves
--   headroom for sampler noise at 50k samples.

CREATE TABLE rv_avg_basic(label text, x random_variable);
INSERT INTO rv_avg_basic VALUES
  ('a', provsql.normal(1, 1)),
  ('b', provsql.normal(2, 1)),
  ('c', provsql.normal(3, 1));
SELECT add_provenance('rv_avg_basic');

CREATE TABLE rv_avg_basic_res AS
  SELECT provsql.avg(x) AS m FROM rv_avg_basic;
SELECT remove_provenance('rv_avg_basic_res');

-- Structure: gate_arith DIV with two gate_arith PLUS children (num,
-- denom), each over three mixture per-row contributions.
SELECT get_gate_type(m::uuid)                              AS root_kind,
       array_length(get_children(m::uuid), 1)              AS root_arity,
       get_gate_type((get_children(m::uuid))[1])           AS num_kind,
       get_gate_type((get_children(m::uuid))[2])           AS den_kind,
       array_length(get_children((get_children(m::uuid))[1]), 1) AS num_arity,
       array_length(get_children((get_children(m::uuid))[2]), 1) AS den_arity
  FROM rv_avg_basic_res;

SELECT abs(provsql.expected(m) - 2.0)       < 0.05 AS avg_basic_mean,
       abs(provsql.variance(m) - 1.0 / 3.0) < 0.05 AS avg_basic_variance
  FROM rv_avg_basic_res;

DROP TABLE rv_avg_basic_res;
DROP TABLE rv_avg_basic;

-- 7b.  Uncertain provenance, guaranteed non-empty.
--   p_a = p_b = 1.0, p_c = 0.5.  Decompose by survivor of c:
--     W1 (prob 0.5, c absent):  AVG = (X_a + X_b)/2, mean 1.5, var 0.5
--     W2 (prob 0.5, c present): AVG = (X_a + X_b + X_c)/3, mean 2.0, var 1/3
--   Mixture means and variances:
--     E[AVG]    = 0.5 * 1.5 + 0.5 * 2.0 = 1.75
--     E[AVG^2]  = 0.5 * (0.5 + 1.5^2) + 0.5 * (1/3 + 2.0^2)
--               = 0.5 * 2.75 + 0.5 * 4.3333  = 3.5417
--     Var[AVG]  = 3.5417 - 1.75^2 = 0.4792
--   Conditioning on at least one row keeps num/denom from hitting 0/0,
--   so the MC mean is well-defined.

CREATE TABLE rv_avg_uncert(label text, x random_variable);
INSERT INTO rv_avg_uncert VALUES
  ('a', provsql.normal(1, 1)),
  ('b', provsql.normal(2, 1)),
  ('c', provsql.normal(3, 1));
SELECT add_provenance('rv_avg_uncert');

DO $$
BEGIN
  PERFORM set_prob(provenance(), 1.0) FROM rv_avg_uncert WHERE label = 'a';
  PERFORM set_prob(provenance(), 1.0) FROM rv_avg_uncert WHERE label = 'b';
  PERFORM set_prob(provenance(), 0.5) FROM rv_avg_uncert WHERE label = 'c';
END
$$;

CREATE TABLE rv_avg_uncert_res AS
  SELECT provsql.avg(x) AS m FROM rv_avg_uncert;
SELECT remove_provenance('rv_avg_uncert_res');

SELECT abs(provsql.expected(m) - 1.75)   < 0.05 AS avg_uncert_mean,
       abs(provsql.variance(m) - 0.4792) < 0.05 AS avg_uncert_variance
  FROM rv_avg_uncert_res;

DROP TABLE rv_avg_uncert_res;
DROP TABLE rv_avg_uncert;

-- 7c.  Empty group: WHERE filters out every row.
--   avg_rv_ffunc returns NULL on the INITCOND='{}' state, matching the
--   standard SQL AVG convention and intentionally differing from
--   provsql.sum (which returns as_random(0) for an empty group).

CREATE TABLE rv_avg_empty(label text, x random_variable, keep boolean);
INSERT INTO rv_avg_empty VALUES
  ('a', provsql.normal(1, 1), false),
  ('b', provsql.normal(2, 1), false);
SELECT add_provenance('rv_avg_empty');

SELECT provsql.avg(x) IS NULL AS avg_empty_is_null
  FROM rv_avg_empty WHERE keep;

DROP TABLE rv_avg_empty;

-- 7d.  Direct (untracked) call.
--   No add_provenance => the planner hook leaves the Aggref alone and
--   the FFUNC sees raw RV uuids, not mixture-wrapped ones.  The fallback
--   branch in avg_rv_ffunc counts each row unconditionally
--   (as_random(1) per row), so AVG matches the straight arithmetic mean.

CREATE TABLE rv_avg_direct(label text, x random_variable);
INSERT INTO rv_avg_direct VALUES
  ('a', provsql.normal(1, 1)),
  ('b', provsql.normal(2, 1)),
  ('c', provsql.normal(3, 1));

CREATE TABLE rv_avg_direct_res AS
  SELECT provsql.avg(x) AS m FROM rv_avg_direct;

-- The denominator is gate_arith(PLUS, [value, value, value]) -- three
-- raw as_random(1) Dirac gates, no mixtures.
SELECT get_gate_type((get_children(m::uuid))[2])                       AS den_kind,
       get_gate_type((get_children((get_children(m::uuid))[2]))[1])    AS den_child_kind
  FROM rv_avg_direct_res;

SELECT abs(provsql.expected(m) - 2.0)       < 0.05 AS avg_direct_mean,
       abs(provsql.variance(m) - 1.0 / 3.0) < 0.05 AS avg_direct_variance
  FROM rv_avg_direct_res;

DROP TABLE rv_avg_direct_res;
DROP TABLE rv_avg_direct;

-- ---------------------------------------------------------------------
-- 8.  PRODUCT(random_variable) on a tracked table.
-- ---------------------------------------------------------------------
-- provsql.product is the multiplicative analogue of provsql.sum: each
-- row contributes a per-row mixture whose else-branch is the
-- multiplicative identity as_random(1) (so absent rows contribute 1,
-- not 0), and the FFUNC builds a gate_arith TIMES root.  Implementation
-- detail: the C-side wrap always emits mixture(prov, x, as_random(0));
-- product_rv_ffunc patches each mixture's else-branch to as_random(1)
-- by reconstructing it.  Empty group returns the multiplicative
-- identity as_random(1), the natural counterpart to sum's as_random(0).

-- 8a.  Deterministic provenance (prob=1.0 default).
--   PRODUCT of independent N(1,1), N(2,1), N(3,1):
--     E[X1*X2*X3]   = mu1*mu2*mu3 = 6
--     E[(X1*X2*X3)^2] = (mu1^2+1)*(mu2^2+1)*(mu3^2+1) = 2*5*10 = 100
--     Var[PRODUCT]  = 100 - 36 = 64
--   The closed-form evaluator's independence shortcut applies because
--   each mixture's footprint reduces to its own X_i (the prov gate is
--   gate_one() under default probability).  Pin to 1e-9.

CREATE TABLE rv_prod_basic(label text, x random_variable);
INSERT INTO rv_prod_basic VALUES
  ('a', provsql.normal(1, 1)),
  ('b', provsql.normal(2, 1)),
  ('c', provsql.normal(3, 1));
SELECT add_provenance('rv_prod_basic');

CREATE TABLE rv_prod_basic_res AS
  SELECT provsql.product(x) AS p FROM rv_prod_basic;
SELECT remove_provenance('rv_prod_basic_res');

-- Structure: gate_arith TIMES over three mixture per-row contributions.
SELECT get_gate_type(p::uuid)                              AS root_kind,
       array_length(get_children(p::uuid), 1)              AS root_arity,
       get_gate_type((get_children(p::uuid))[1])           AS child_kind
  FROM rv_prod_basic_res;

SELECT abs(provsql.expected(p) - 6.0)  < 1e-9 AS prod_basic_mean,
       abs(provsql.variance(p) - 64.0) < 1e-9 AS prod_basic_variance
  FROM rv_prod_basic_res;

DROP TABLE rv_prod_basic_res;
DROP TABLE rv_prod_basic;

-- 8b.  Uncertain provenance with independent Bernoullis.
--   Each row's per-row contribution is mixture(b_i, X_i, as_random(1)):
--     E[mix_a] = 0.5 * 1 + 0.5 * 1 = 1.0
--     E[mix_b] = 0.4 * 2 + 0.6 * 1 = 1.4
--     E[mix_c] = 0.3 * 3 + 0.7 * 1 = 1.6
--   Footprints are disjoint (b_i and X_i are per-row independent), so
--   the closed-form evaluator factorises:
--     E[PRODUCT] = 1.0 * 1.4 * 1.6 = 2.24  (exact to 1e-9)

CREATE TABLE rv_prod_uncert(label text, x random_variable);
INSERT INTO rv_prod_uncert VALUES
  ('a', provsql.normal(1, 1)),
  ('b', provsql.normal(2, 1)),
  ('c', provsql.normal(3, 1));
SELECT add_provenance('rv_prod_uncert');
DO $$
BEGIN
  PERFORM set_prob(provenance(), 0.5) FROM rv_prod_uncert WHERE label = 'a';
  PERFORM set_prob(provenance(), 0.4) FROM rv_prod_uncert WHERE label = 'b';
  PERFORM set_prob(provenance(), 0.3) FROM rv_prod_uncert WHERE label = 'c';
END
$$;

CREATE TABLE rv_prod_uncert_res AS
  SELECT provsql.product(x) AS p FROM rv_prod_uncert;
SELECT remove_provenance('rv_prod_uncert_res');

SELECT abs(provsql.expected(p) - 2.24) < 1e-9 AS prod_uncert_mean
  FROM rv_prod_uncert_res;

DROP TABLE rv_prod_uncert_res;
DROP TABLE rv_prod_uncert;

-- 8c.  Empty group: PRODUCT over zero rows is the multiplicative
--   identity 1 (a gate_value Dirac), counterpart to sum's empty-group
--   as_random(0).

CREATE TABLE rv_prod_empty(label text, x random_variable, keep boolean);
INSERT INTO rv_prod_empty VALUES
  ('a', provsql.normal(1, 1), false),
  ('b', provsql.normal(2, 1), false);
SELECT add_provenance('rv_prod_empty');

CREATE TABLE empty_prod AS
  SELECT provsql.product(x) AS p FROM rv_prod_empty WHERE keep;
SELECT remove_provenance('empty_prod');

SELECT get_gate_type(p::uuid)                AS empty_prod_kind,
       get_extra(p::uuid)                    AS empty_prod_extra,
       abs(provsql.expected(p) - 1.0) < 1e-9 AS empty_prod_mean,
       abs(provsql.variance(p) - 0.0) < 1e-9 AS empty_prod_variance
  FROM empty_prod;

DROP TABLE empty_prod;
DROP TABLE rv_prod_empty;

RESET provsql.monte_carlo_seed;
RESET provsql.rv_mc_samples;
