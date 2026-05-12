\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Phase 1 of the SUM-over-RV story: provsql.rv_sum is the explicit
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

CREATE TABLE rv_basic_sum AS SELECT provsql.rv_sum(x) AS s FROM rv_basic;
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
--   E[rv_sum] = sum_i p_i * mu_i
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
  SELECT provsql.rv_sum(x) AS s FROM rv_uncertain;
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

CREATE TABLE coupled_sum AS SELECT provsql.rv_sum(x) AS s FROM rv_coupled;
SELECT remove_provenance('coupled_sum');

SELECT abs(provsql.expected(s) - 1.0) < 1e-9 AS coupled_mean,
       abs(provsql.variance(s) - 2.0) < 0.05 AS coupled_variance
  FROM coupled_sum;

CREATE TABLE rv_decoupled(label text, x random_variable, provsql uuid);
INSERT INTO rv_decoupled VALUES
  ('a', provsql.normal(1, 1), (SELECT b1 FROM bern_pair)),
  ('b', provsql.normal(1, 1), (SELECT b2 FROM bern_pair));

CREATE TABLE decoupled_sum AS SELECT provsql.rv_sum(x) AS s FROM rv_decoupled;
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
INSERT INTO rv_anchor VALUES (random_variable_uuid(provsql.normal(0, 1)));

CREATE TABLE rv_shared(label text, x random_variable);
INSERT INTO rv_shared VALUES
  ('a', random_variable_make((SELECT u FROM rv_anchor), 'NaN'::float8)),
  ('b', random_variable_make((SELECT u FROM rv_anchor), 'NaN'::float8));
SELECT add_provenance('rv_shared');

CREATE TABLE shared_rv_sum AS SELECT provsql.rv_sum(x) AS s FROM rv_shared;
SELECT remove_provenance('shared_rv_sum');

SELECT abs(provsql.expected(s) - 0.0) < 1e-9 AS shared_rv_mean,
       abs(provsql.variance(s) - 4.0) < 0.1 AS shared_rv_variance
  FROM shared_rv_sum;

CREATE TABLE rv_distinct(label text, x random_variable);
INSERT INTO rv_distinct VALUES
  ('a', provsql.normal(0, 1)),
  ('b', provsql.normal(0, 1));
SELECT add_provenance('rv_distinct');

CREATE TABLE distinct_rv_sum AS SELECT provsql.rv_sum(x) AS s FROM rv_distinct;
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
  SELECT provsql.rv_sum(x) AS s FROM rv_empty WHERE keep;
SELECT remove_provenance('empty_sum');

SELECT get_gate_type(s::uuid)                AS empty_root_kind,
       random_variable_value(s)              AS empty_root_value,
       abs(provsql.expected(s) - 0.0) < 1e-9 AS empty_mean,
       abs(provsql.variance(s) - 0.0) < 1e-9 AS empty_variance
  FROM empty_sum;

DROP TABLE empty_sum;
DROP TABLE rv_empty;

-- ---------------------------------------------------------------------
-- 6.  COUNT(*) lift via rv_sum(as_random(1)).
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
  SELECT provsql.rv_sum(provsql.as_random(1)) AS s FROM rv_count;
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

RESET provsql.monte_carlo_seed;
RESET provsql.rv_mc_samples;
