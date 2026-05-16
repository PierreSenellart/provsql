-- Regression coverage for foldBooleanIdentities (the load-time
-- Boolean-only simplifier gated on provsql.boolean_provenance).
--
-- The rules :
--   B1 (idempotence) : gate_plus(a, a, b)  -> gate_plus(a, b)
--                      gate_times(a, a, b) -> gate_times(a, b)
--   B2 (plus-with-one absorber) : gate_plus(..., gate_one, ...) -> gate_one
--
-- Each rule application wraps the result in gate_assumed_boolean so
-- semiring evaluators incompatible with the Boolean assumption refuse.

\pset format unaligned
SET search_path TO public, provsql;

CREATE TABLE bf_t(id int, lbl text);
INSERT INTO bf_t VALUES (1, 'u'), (2, 'v');
SELECT add_provenance('bf_t');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM bf_t; END $$;
-- Numeric mapping used to drive sr_counting refusal (the only way to
-- exercise a non-Boolean-compatible semiring against the wrapped
-- root, since sr_* are STRICT on the mapping argument).
CREATE TABLE bf_t_cnt AS SELECT 1::int AS value, provsql AS provenance FROM bf_t;

-- ----------------------------------------------------------------------
-- (1) Idempotence on a SHARED INTERIOR subexpression unlocks
--     independent on a circuit that is otherwise not read-once.
--     times(plus(u, v), plus(u, v)) has the same plus(u, v) subgate
--     reached from the root through two distinct paths -- independent's
--     read-once check rejects it.  With boolean_provenance on, B1
--     dedups the times wires to a single plus(u, v), the trailing
--     semiring-safe pass substitutes single-wire times -> plus(u, v),
--     and independent succeeds with the read-once OR probability
--     1 - (1 - 0.5)*(1 - 0.5) = 0.75.
-- ----------------------------------------------------------------------
DO $$
DECLARE u uuid; v uuid; sub uuid; root uuid;
BEGIN
  SELECT provsql INTO u FROM bf_t WHERE id = 1;
  SELECT provsql INTO v FROM bf_t WHERE id = 2;
  sub  := provenance_plus(ARRAY[u, v]);
  root := uuid_generate_v5(uuid_ns_provsql(),
                           concat('bf-tested-shared-sub', sub));
  PERFORM create_gate(root, 'times', ARRAY[sub, sub]);
  PERFORM set_config('bf.shared_root', root::text, false);
END $$;

-- Without folding, independent refuses (not read-once).
SET provsql.boolean_provenance = off;
DO $$ DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(
      current_setting('bf.shared_root')::uuid, 'independent');
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected ''independent'' to refuse the non-read-once '
                    'circuit with boolean_provenance = off';
  END IF;
END $$;

-- With folding, idempotence collapses the duplicate plus(u, v) child,
-- the in-memory boolean_assumed flag fires on the root, and
-- independent yields the read-once probability.
SET provsql.boolean_provenance = on;
SELECT round(probability_evaluate(
                current_setting('bf.shared_root')::uuid, 'independent')
              ::numeric, 9) AS shared_root_p_ind;

-- simplified_circuit_subgraph exposes the in-memory folded form ;
-- the root keeps its original gate type (no wrapper minted) and
-- carries the boolean_assumed flag set by foldBooleanIdentities.
SELECT (simplified_circuit_subgraph(
          current_setting('bf.shared_root')::uuid, 1)
         ->0->>'gate_type') AS shared_root_simplified_type,
       (simplified_circuit_subgraph(
          current_setting('bf.shared_root')::uuid, 1)
         ->0->>'boolean_assumed') AS shared_root_boolean_assumed;

-- sr_counting refuses on the wrapped circuit : counting is not
-- compatible with the Boolean assumption.
DO $$ DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM sr_counting(
      current_setting('bf.shared_root')::uuid, 'bf_t_cnt'::regclass);
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected sr_counting to refuse a Boolean-wrapped circuit';
  END IF;
END $$;
SET provsql.boolean_provenance = off;

-- ----------------------------------------------------------------------
-- (2) Plus-with-one absorber.  gate_plus(u, gate_one()) collapses to
--     gate_one once boolean_provenance is on ; the gate keeps its
--     UUID, foldSemiringIdentities mutates its type to gate_one, and
--     the boolean_assumed flag is set so non-Boolean-compatible
--     semirings refuse.
-- ----------------------------------------------------------------------
DO $$
DECLARE u uuid; root uuid;
BEGIN
  SELECT provsql INTO u FROM bf_t WHERE id = 1;
  root := uuid_generate_v5(uuid_ns_provsql(), concat('bf-one-abs', u));
  PERFORM create_gate(root, 'plus', ARRAY[u, gate_one()]);
  PERFORM set_config('bf.one_root', root::text, false);
END $$;

SET provsql.boolean_provenance = on;
SELECT (simplified_circuit_subgraph(
          current_setting('bf.one_root')::uuid, 2)
         ->0->>'gate_type') AS one_root_simplified_type,
       (simplified_circuit_subgraph(
          current_setting('bf.one_root')::uuid, 2)
         ->0->>'boolean_assumed') AS one_root_boolean_assumed;
-- After folding the wrapper sits over a gate_one ; probability is 1.
SELECT round(probability_evaluate(
                current_setting('bf.one_root')::uuid, 'independent')
              ::numeric, 9) AS one_root_p_ind;
SET provsql.boolean_provenance = off;

-- ----------------------------------------------------------------------
-- (3) Phase 3 of foldSemiringIdentities, when chained after the
--     Boolean dedup, substitutes single-wire plus / times in place.
--     Pre-fix, that substitution copied target type / wires / infos
--     / extra but not the target's probability nor its membership in
--     the inputs set : a substituted gate that landed as a
--     gate_input with prob 0.5 instead read as an input with the
--     addGate-default prob 1.
--
--     Probe : gate_plus(u, gate_zero()) collapses to u via
--     Phase 1 (drop the zero wire) then Phase 2 / Phase 3
--     (singleton substitute).  u has prob 0.5 ; the probability
--     evaluated on the root must equal 0.5, not 1.
-- ----------------------------------------------------------------------
DO $$
DECLARE u uuid; root uuid;
BEGIN
  SELECT provsql INTO u FROM bf_t WHERE id = 1;
  root := uuid_generate_v5(uuid_ns_provsql(), concat('bf-zero', u));
  PERFORM create_gate(root, 'plus', ARRAY[u, gate_zero()]);
  PERFORM set_config('bf.zero_root', root::text, false);
END $$;
SELECT round(probability_evaluate(
                current_setting('bf.zero_root')::uuid, 'independent')
              ::numeric, 9) AS zero_root_p_ind;

-- ----------------------------------------------------------------------
-- (4) Absorption (B3, Boolean-only).
--     gate_plus(x, gate_times(x, y))   -> gate_plus(x)   -> x
--     gate_times(x, gate_plus(x, y))   -> gate_times(x)  -> x
--     The absorbed times / plus child is dominated by sibling x.
--     Sound under Boolean algebra (x OR (x AND y) = x ;
--     x AND (x OR y) = x) but unsound in general semirings, so the
--     parent is marked boolean_assumed.
-- ----------------------------------------------------------------------
DO $$
DECLARE u uuid; v uuid; tab uuid; root_plus uuid; root_times uuid;
BEGIN
  SELECT provsql INTO u FROM bf_t WHERE id = 1;
  SELECT provsql INTO v FROM bf_t WHERE id = 2;
  -- plus(u, times(u, v)) absorbs to u.  P(u) = 0.5.
  tab := provenance_times(u, v);
  root_plus := uuid_generate_v5(uuid_ns_provsql(), concat('bf-abs-plus', tab));
  PERFORM create_gate(root_plus, 'plus', ARRAY[u, tab]);
  PERFORM set_config('bf.abs_plus_root', root_plus::text, false);
  -- times(u, plus(u, v)) absorbs to u.  P(u) = 0.5.
  tab := provenance_plus(ARRAY[u, v]);
  root_times := uuid_generate_v5(uuid_ns_provsql(), concat('bf-abs-times', tab));
  PERFORM create_gate(root_times, 'times', ARRAY[u, tab]);
  PERFORM set_config('bf.abs_times_root', root_times::text, false);
END $$;

SET provsql.boolean_provenance = on;
SELECT round(probability_evaluate(
                current_setting('bf.abs_plus_root')::uuid, 'independent')
              ::numeric, 9) AS plus_abs_p_ind;
SELECT round(probability_evaluate(
                current_setting('bf.abs_times_root')::uuid, 'independent')
              ::numeric, 9) AS times_abs_p_ind;
SET provsql.boolean_provenance = off;

DROP TABLE bf_t_cnt;
DROP TABLE bf_t;
