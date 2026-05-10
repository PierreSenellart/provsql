\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- The sampler uses std::mt19937_64 seeded from provsql.monte_carlo_seed
-- so all assertions in this file are deterministic across runs.
SET provsql.monte_carlo_seed = 42;

-- Helper: build a cmp gate over two random_variable arguments and one
-- comparison operator OID, then evaluate it via Monte Carlo.  The
-- comparison operator is the standard float8 comparator: every gate_rv
-- and gate_value child is scalar-valued, so the sampler produces
-- doubles for both sides and applies the comparator.
CREATE OR REPLACE FUNCTION mc_prob(
  l random_variable, r random_variable, op text, samples int
) RETURNS double precision AS
$$
  SELECT provsql.probability_evaluate(
    provsql.provenance_cmp(
      provsql.random_variable_uuid(l),
      (op || '(double precision,double precision)')::regoperator::oid,
      provsql.random_variable_uuid(r)),
    'monte-carlo', samples::text)
$$ LANGUAGE sql VOLATILE;

-- Per-iteration scalar memoisation: when the same gate_rv UUID feeds
-- both sides of a comparator, the two evaluations must use the SAME
-- draw, so the comparator collapses to "x = x" / "x > x" / etc.
WITH r AS (SELECT provsql.normal(0, 1) AS rv)
SELECT
  provsql.probability_evaluate(
    provsql.provenance_cmp(
      provsql.random_variable_uuid(rv),
      '=(double precision,double precision)'::regoperator::oid,
      provsql.random_variable_uuid(rv)),
    'monte-carlo', '1000') = 1.0 AS rv_equals_itself,
  provsql.probability_evaluate(
    provsql.provenance_cmp(
      provsql.random_variable_uuid(rv),
      '<(double precision,double precision)'::regoperator::oid,
      provsql.random_variable_uuid(rv)),
    'monte-carlo', '1000') = 0.0 AS rv_lt_itself
FROM r;

-- Distribution sampling: P(N(0, 1) > 0) is exactly 0.5 by symmetry;
-- a 100k-sample MC pull should be within 0.01 of the truth.
SELECT abs(mc_prob(provsql.normal(0, 1), provsql.as_random(0), '>', 100000) - 0.5)
       < 0.01 AS normal_gt_zero_within_tolerance;

-- P(U(0, 1) <= 0.3) = 0.3
SELECT abs(mc_prob(provsql.uniform(0, 1), provsql.as_random(0.3), '<=', 100000) - 0.3)
       < 0.01 AS uniform_le_0_3_within_tolerance;

-- P(Exp(1) > 1) = 1/e ≈ 0.367879
SELECT abs(mc_prob(provsql.exponential(1), provsql.as_random(1), '>', 100000) - 0.36787944117)
       < 0.01 AS exponential_gt_1_within_tolerance;

-- Determinism: same seed -> same MC result, even with a non-trivial
-- sample count.  Run twice, expect bit-identical doubles.
SELECT mc_prob(provsql.normal(0, 1), provsql.as_random(0), '>', 5000)
     = mc_prob(provsql.normal(0, 1), provsql.as_random(0), '>', 5000) AS deterministic;

-- Hand-built gate_arith over two independent normals: P(X + Y > 0) is
-- exactly 0.5 because X+Y ~ N(0, 2).  The arith gate's info1 holds
-- the PROVSQL_ARITH_PLUS tag (= 0).  We hand-build the circuit here
-- to exercise the gate_arith branch of the sampler directly, without
-- relying on the user-facing rv arithmetic operators.
DO $$
DECLARE
  x uuid := provsql.random_variable_uuid(provsql.normal(0, 1));
  y uuid := provsql.random_variable_uuid(provsql.normal(0, 1));
  zero uuid := provsql.random_variable_uuid(provsql.as_random(0));
  sum_token uuid := public.uuid_generate_v4();
  cmp_token uuid;
  p double precision;
BEGIN
  PERFORM provsql.create_gate(sum_token, 'arith', ARRAY[x, y]);
  PERFORM provsql.set_infos(sum_token, 0);  -- PROVSQL_ARITH_PLUS = 0
  cmp_token := provsql.provenance_cmp(
    sum_token,
    '>(double precision,double precision)'::regoperator::oid,
    zero);
  p := provsql.probability_evaluate(cmp_token, 'monte-carlo', '100000');
  IF abs(p - 0.5) >= 0.01 THEN
    RAISE EXCEPTION 'arith PLUS sampler P(X+Y>0)=% out of tolerance', p;
  END IF;
END $$;

-- Same circuit, structurally-dependent variant: arith PLUS over the
-- *same* x twice gives 2x, and P(2x > 0) = P(x > 0) = 0.5 — but the
-- per-iteration memoisation must produce the same x twice.
DO $$
DECLARE
  x uuid := provsql.random_variable_uuid(provsql.normal(0, 1));
  zero uuid := provsql.random_variable_uuid(provsql.as_random(0));
  sum_token uuid := public.uuid_generate_v4();
  cmp_token uuid;
  p double precision;
BEGIN
  PERFORM provsql.create_gate(sum_token, 'arith', ARRAY[x, x]);
  PERFORM provsql.set_infos(sum_token, 0);
  cmp_token := provsql.provenance_cmp(
    sum_token,
    '>(double precision,double precision)'::regoperator::oid,
    zero);
  p := provsql.probability_evaluate(cmp_token, 'monte-carlo', '100000');
  IF abs(p - 0.5) >= 0.01 THEN
    RAISE EXCEPTION 'arith PLUS (same-x twice) sampler P out of tolerance: %', p;
  END IF;
END $$;

-- gate_arith TIMES, MINUS, DIV, NEG round-trip through the sampler.
-- We don't pin tight tolerances here — just sanity-check that the
-- branches don't throw and the result is a finite probability.
DO $$
DECLARE
  x uuid := provsql.random_variable_uuid(provsql.uniform(1, 2));
  y uuid := provsql.random_variable_uuid(provsql.uniform(1, 2));
  zero uuid := provsql.random_variable_uuid(provsql.as_random(0));
  arith_token uuid;
  cmp_token uuid;
  p double precision;
  op_tag int;
BEGIN
  -- TIMES (1)
  arith_token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(arith_token, 'arith', ARRAY[x, y]);
  PERFORM provsql.set_infos(arith_token, 1);
  cmp_token := provsql.provenance_cmp(arith_token,
    '>(double precision,double precision)'::regoperator::oid,
    zero);
  p := provsql.probability_evaluate(cmp_token, 'monte-carlo', '1000');
  IF p <> 1.0 THEN
    RAISE EXCEPTION 'arith TIMES of two U(1,2) > 0 should be 1.0, got %', p;
  END IF;

  -- MINUS (2): U(1,2) - U(1,2) > 0 has probability 0.5
  arith_token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(arith_token, 'arith', ARRAY[x, y]);
  PERFORM provsql.set_infos(arith_token, 2);
  cmp_token := provsql.provenance_cmp(arith_token,
    '>(double precision,double precision)'::regoperator::oid,
    zero);
  p := provsql.probability_evaluate(cmp_token, 'monte-carlo', '100000');
  IF abs(p - 0.5) >= 0.01 THEN
    RAISE EXCEPTION 'arith MINUS sampler P(X-Y>0)=% out of tolerance', p;
  END IF;

  -- NEG (4): -U(1,2) > 0 has probability 0
  arith_token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(arith_token, 'arith', ARRAY[x]);
  PERFORM provsql.set_infos(arith_token, 4);
  cmp_token := provsql.provenance_cmp(arith_token,
    '>(double precision,double precision)'::regoperator::oid,
    zero);
  p := provsql.probability_evaluate(cmp_token, 'monte-carlo', '1000');
  IF p <> 0.0 THEN
    RAISE EXCEPTION 'arith NEG of U(1,2) > 0 should be 0.0, got %', p;
  END IF;
END $$;

-- All asserts above either return a Boolean t/f or raise; this single
-- final SELECT terminates the test with a stable "ok" line.
SELECT 'ok'::text AS continuous_sampler_done;

DROP FUNCTION mc_prob(random_variable, random_variable, text, int);
RESET provsql.monte_carlo_seed;
