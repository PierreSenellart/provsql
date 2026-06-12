\set ECHO none
\pset format unaligned

-- Conditioning operator: the binary | (function alias cond) over uuid
-- provenance tokens builds a terminal gate_conditioned that
-- probability_evaluate reads as the conditional P(A|B) = P(A∧B)/P(B).
-- The conditional is exact and correlation-aware: content-addressing makes
-- a base tuple shared by A and B the same input gate in both circuits, so
-- the joint P(A∧B) is the true joint, not an independent product.

-- ---------------------------------------------------------------------
-- Medical-test Bayes (the canonical demo: P(disease|positive) != sensitivity)
-- Joint sample space of (disease, positive) as mutually-exclusive worlds,
-- with prevalence .01, sensitivity .9, specificity .95.
-- ---------------------------------------------------------------------
CREATE TABLE world(id int, disease boolean, positive boolean, p float);
INSERT INTO world VALUES
  (1, true,  true,  0.009),   -- disease & test positive
  (1, true,  false, 0.001),   -- disease & test negative
  (1, false, true,  0.0495),  -- healthy & false positive
  (1, false, false, 0.9405);  -- healthy & true negative
SELECT repair_key('world','id');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM world; END $$;

-- Event tokens via inert grouped fetches: disease = OR of the diseased
-- worlds, positive = OR of the positive worlds.
WITH d AS (
  SELECT (SELECT provenance() FROM world WHERE disease  GROUP BY id) AS dtok,
         (SELECT provenance() FROM world WHERE positive GROUP BY id) AS ptok)
SELECT round(probability_evaluate(dtok)::numeric,6)        AS p_disease,
       round(probability_evaluate(ptok)::numeric,6)        AS p_positive,
       round(probability_evaluate(dtok | ptok)::numeric,6) AS p_disease_given_pos,
       round(probability_evaluate(ptok | dtok)::numeric,6) AS p_pos_given_disease
FROM d;

-- ---------------------------------------------------------------------
-- Independence: P(A|B) = P(A) for independent A, B (separate repair_key
-- groups are independent).
-- ---------------------------------------------------------------------
CREATE TABLE coins(id int, c text, p float);
INSERT INTO coins VALUES (1,'H',0.3),(1,'T',0.7),(2,'H',0.5),(2,'T',0.5);
SELECT repair_key('coins','id');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM coins; END $$;

WITH e AS (
  SELECT (SELECT provenance() FROM coins WHERE id=1 AND c='H') AS a,
         (SELECT provenance() FROM coins WHERE id=2 AND c='H') AS b)
SELECT round(probability_evaluate(a)::numeric,3)     AS p_a,
       round(probability_evaluate(a | b)::numeric,3) AS p_a_given_b
FROM e;

-- ---------------------------------------------------------------------
-- Identity conventions: conditioning on a certain / absent event is inert.
-- ---------------------------------------------------------------------
WITH d AS (
  SELECT (SELECT provenance() FROM world WHERE disease GROUP BY id) AS dtok)
SELECT (cond(dtok, NULL)        = dtok) AS null_evidence_identity,
       (cond(dtok, gate_one())  = dtok) AS one_evidence_identity
FROM d;

-- ---------------------------------------------------------------------
-- Folding (sequential Bayesian update): (X|A)|B = X|(A∧B); the gate stays
-- one level deep (a single gate_conditioned with 3 children), and
-- (D|+)|+ = D|+ is idempotent; (D|+)|D = 1.
-- ---------------------------------------------------------------------
WITH d AS (
  SELECT (SELECT provenance() FROM world WHERE disease  GROUP BY id) AS dtok,
         (SELECT provenance() FROM world WHERE positive GROUP BY id) AS ptok)
SELECT get_gate_type(cond(cond(dtok,ptok),ptok))               AS folded_type,
       array_length(get_children(cond(cond(dtok,ptok),ptok)),1) AS folded_children,
       round(probability_evaluate(cond(cond(dtok,ptok),ptok))::numeric,6) AS d_given_pos_pos,
       round(probability_evaluate(cond(cond(dtok,ptok),dtok))::numeric,6) AS d_given_pos_d
FROM d;

-- ---------------------------------------------------------------------
-- Impossible evidence (P(B)=0) -> NULL.  The conjunction of two distinct
-- mutually-exclusive worlds has probability 0.
-- ---------------------------------------------------------------------
WITH d AS (
  SELECT (SELECT provenance() FROM world WHERE disease GROUP BY id) AS dtok,
         (SELECT provenance() FROM world WHERE disease AND positive)         AS w_tt,
         (SELECT provenance() FROM world WHERE NOT disease AND NOT positive) AS w_ff)
SELECT probability_evaluate(cond(dtok, provenance_times(w_tt, w_ff))) IS NULL
         AS impossible_evidence_is_null
FROM d;

-- ---------------------------------------------------------------------
-- Terminal property: a conditioned token may not be composed under a
-- semiring gate.  Building times(conditioned, .) and evaluating it is
-- refused (the same refusal a general sr_* semiring raises).
-- ---------------------------------------------------------------------
WITH d AS (
  SELECT (SELECT provenance() FROM world WHERE disease  GROUP BY id) AS dtok,
         (SELECT provenance() FROM world WHERE positive GROUP BY id) AS ptok)
SELECT probability_evaluate(provenance_times(dtok | ptok, ptok)) AS refused
FROM d;

SELECT remove_provenance('world');
SELECT remove_provenance('coins');
DROP TABLE world;
DROP TABLE coins;
