\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Safe-query rewrite (provsql.boolean_provenance = on) wraps every
-- per-row root in a fresh gate_assumed_boolean.  The wrapper is a
-- structural marker (single child = original root) that:
--   * is transparent (identity) for evaluators whose Semiring
--     subclass returns true from compatibleWithBooleanRewrite();
--   * raises CircuitException for the rest, since the safe-query
--     rewrite collapses derivation multiplicities / monomial
--     structure / witness sets in ways those semirings would
--     otherwise distinguish.
-- The structural design (vs. a root info-bit tag) survives
-- content-addressed UUID deduplication and propagates naturally into
-- any downstream query that consumes the rewritten output.

CREATE TABLE sqs_a(x int, lbl text);
CREATE TABLE sqs_b(x int, lbl text);
INSERT INTO sqs_a VALUES (1, 'a1'), (2, 'a2');
INSERT INTO sqs_b VALUES (1, 'b1'), (2, 'b2');

SELECT add_provenance('sqs_a');
SELECT add_provenance('sqs_b');

DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM sqs_a;
  PERFORM set_prob(provsql, 0.4) FROM sqs_b;
END $$;

SELECT create_provenance_mapping('sqs_a_lbl', 'sqs_a', 'lbl');
SELECT create_provenance_mapping('sqs_b_lbl', 'sqs_b', 'lbl');

-- A two-atom hierarchical CQ q(x) :- A(x), B(x) under the GUC.
SET provsql.boolean_provenance = on;

CREATE TEMP TABLE sqs_t AS
  SELECT a.x AS x, provenance() AS p
    FROM sqs_a a, sqs_b b
   WHERE a.x = b.x
   GROUP BY a.x;
SELECT remove_provenance('sqs_t');

-- (1) Every root gate is a gate_assumed_boolean (single-child wrapper).
SELECT x, get_gate_type(p) AS root_type,
       array_length(get_children(p), 1) AS nb_children
  FROM sqs_t ORDER BY x;

-- (2) probability_evaluate is always sound (it routes through Boolean
--     factoring): 0.5 * 0.4 = 0.20 at each x.
SELECT x, ROUND(probability_evaluate(p)::numeric, 6) AS prob
  FROM sqs_t ORDER BY x;

-- (3) Compatible semirings evaluate without error on a wrapped
--     circuit.  We don't check the values themselves (other tests do
--     that); the assertion is "no exception".
DO $$ BEGIN
  PERFORM sr_formula(p, 'sqs_a_lbl') FROM sqs_t;
  PERFORM sr_boolexpr(p)            FROM sqs_t;
END $$;
SELECT 'compatible semirings ok' AS step3;

-- (4) Incompatible semirings each raise CircuitException whose
--     message mentions the Boolean-rewrite incompatibility.
DO $$
DECLARE tok uuid; raised text;
  BEGIN
    SELECT p INTO tok FROM sqs_t ORDER BY x LIMIT 1;

    raised := NULL;
    BEGIN PERFORM sr_counting(tok, 'sqs_a_lbl');
    EXCEPTION WHEN OTHERS THEN raised := SQLERRM; END;
    IF raised IS NULL OR raised !~ 'homomorphism from Boolean' THEN
      RAISE EXCEPTION 'sr_counting: %', COALESCE(raised, '<no error>');
    END IF;

    raised := NULL;
    BEGIN PERFORM sr_how(tok, 'sqs_a_lbl');
    EXCEPTION WHEN OTHERS THEN raised := SQLERRM; END;
    IF raised IS NULL OR raised !~ 'homomorphism from Boolean' THEN
      RAISE EXCEPTION 'sr_how: %', COALESCE(raised, '<no error>');
    END IF;

    raised := NULL;
    BEGIN PERFORM sr_why(tok, 'sqs_a_lbl');
    EXCEPTION WHEN OTHERS THEN raised := SQLERRM; END;
    IF raised IS NULL OR raised !~ 'homomorphism from Boolean' THEN
      RAISE EXCEPTION 'sr_why: %', COALESCE(raised, '<no error>');
    END IF;

    raised := NULL;
    BEGIN PERFORM sr_which(tok, 'sqs_a_lbl');
    EXCEPTION WHEN OTHERS THEN raised := SQLERRM; END;
    IF raised IS NULL OR raised !~ 'homomorphism from Boolean' THEN
      RAISE EXCEPTION 'sr_which: %', COALESCE(raised, '<no error>');
    END IF;

    raised := NULL;
    BEGIN PERFORM sr_tropical(tok, 'sqs_a_lbl');
    EXCEPTION WHEN OTHERS THEN raised := SQLERRM; END;
    IF raised IS NULL OR raised !~ 'homomorphism from Boolean' THEN
      RAISE EXCEPTION 'sr_tropical: %', COALESCE(raised, '<no error>');
    END IF;

    raised := NULL;
    BEGIN PERFORM sr_viterbi(tok, 'sqs_a_lbl');
    EXCEPTION WHEN OTHERS THEN raised := SQLERRM; END;
    IF raised IS NULL OR raised !~ 'homomorphism from Boolean' THEN
      RAISE EXCEPTION 'sr_viterbi: %', COALESCE(raised, '<no error>');
    END IF;

    raised := NULL;
    BEGIN PERFORM sr_lukasiewicz(tok, 'sqs_a_lbl');
    EXCEPTION WHEN OTHERS THEN raised := SQLERRM; END;
    IF raised IS NULL OR raised !~ 'homomorphism from Boolean' THEN
      RAISE EXCEPTION 'sr_lukasiewicz: %', COALESCE(raised, '<no error>');
    END IF;
  END $$;
SELECT 'incompatible semirings rejected' AS step4;

-- (5) Cross-check: the same conceptual circuit evaluated without the
--     rewrite has no gate_assumed_boolean wrapper, so the same
--     incompatible semirings succeed.  The OFF-path root is a
--     gate_times whose UUID differs from the ON-path's
--     gate_assumed_boolean UUID, so no shared persistent state
--     leaks between the two runs.
SET provsql.boolean_provenance = off;

CREATE TEMP TABLE sqs_t_off AS
  SELECT a.x AS x, provenance() AS p
    FROM sqs_a a, sqs_b b
   WHERE a.x = b.x
   GROUP BY a.x;
SELECT remove_provenance('sqs_t_off');

SELECT x, get_gate_type(p) AS root_type FROM sqs_t_off ORDER BY x;
SELECT x, sr_how(p, 'sqs_a_lbl') FROM sqs_t_off ORDER BY x;
SELECT x, sr_counting(p, 'sqs_a_lbl') FROM sqs_t_off ORDER BY x;

DROP TABLE sqs_a_lbl, sqs_b_lbl, sqs_a, sqs_b;
