\set ECHO none
-- Regression coverage for foldBooleanIdentities (the load-time
-- Boolean-only simplifier gated on provsql.boolean_provenance).
--
-- The rules :
--   B1 (idempotence) : gate_plus(a, a, b)  -> gate_plus(a, b)
--                      gate_times(a, a, b) -> gate_times(a, b)
--   B2 (plus-with-one absorber) : gate_plus(..., gate_one, ...) -> gate_one
--
-- Each rule application wraps the result in gate_assumed so
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

-- Even without folding, independent succeeds on this shape: the
-- BoolExpr conversion memoises shared gates (GenericCircuit::evaluate),
-- so the duplicated child reaches BoolExpr.times as one value and its
-- (absorptive-by-design) dedup collapses x AND x structurally -- valid
-- for the probability carrier irrespective of the GUC.  Non-Boolean
-- semirings are unaffected (counting still squares the value); the
-- boolean_assumed discipline below concerns those, not probability.
SET provsql.provenance = 'semiring';
SELECT round(probability_evaluate(
                current_setting('bf.shared_root')::uuid, 'independent')
              ::numeric, 9) AS shared_root_p_ind_nofold;

-- With folding, idempotence collapses the duplicate plus(u, v) child,
-- the in-memory boolean_assumed flag fires on the root, and
-- independent yields the read-once probability.
SET provsql.provenance = 'boolean';
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
SET provsql.provenance = 'semiring';

-- ----------------------------------------------------------------------
-- (2) Plus-with-one absorber.  gate_plus(u, gate_one()) collapses to
--     gate_one once the provenance class allows it ; the gate keeps
--     its UUID, foldSemiringIdentities mutates its type to gate_one,
--     and -- plus-with-one being the defining *absorptive* identity --
--     the absorptive_assumed flag is set so non-absorptive semirings
--     refuse.
-- ----------------------------------------------------------------------
DO $$
DECLARE u uuid; root uuid;
BEGIN
  SELECT provsql INTO u FROM bf_t WHERE id = 1;
  root := uuid_generate_v5(uuid_ns_provsql(), concat('bf-one-abs', u));
  PERFORM create_gate(root, 'plus', ARRAY[u, gate_one()]);
  PERFORM set_config('bf.one_root', root::text, false);
END $$;

SET provsql.provenance = 'boolean';
SELECT (simplified_circuit_subgraph(
          current_setting('bf.one_root')::uuid, 2)
         ->0->>'gate_type') AS one_root_simplified_type,
       (simplified_circuit_subgraph(
          current_setting('bf.one_root')::uuid, 2)
         ->0->>'absorptive_assumed') AS one_root_absorptive_assumed;
-- After folding the wrapper sits over a gate_one ; probability is 1.
SELECT round(probability_evaluate(
                current_setting('bf.one_root')::uuid, 'independent')
              ::numeric, 9) AS one_root_p_ind;
SET provsql.provenance = 'semiring';

-- ----------------------------------------------------------------------
-- (3) foldSemiringIdentities collapses a single-wire plus / times to
--     its lone child.  Phase 3 rewires parents straight to the target
--     and re-points the root UUID, so the probability of the target
--     leaf is preserved verbatim (an earlier in-place content-copy had
--     to special-case copying the target's probability; see case (6)
--     for the shared-leaf hazard that copy created).
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

SET provsql.provenance = 'boolean';
SELECT round(probability_evaluate(
                current_setting('bf.abs_plus_root')::uuid, 'independent')
              ::numeric, 9) AS plus_abs_p_ind;
SELECT round(probability_evaluate(
                current_setting('bf.abs_times_root')::uuid, 'independent')
              ::numeric, 9) AS times_abs_p_ind;
SET provsql.provenance = 'semiring';

-- ----------------------------------------------------------------------
-- (5) Real-world absorption pattern : the UNION's first branch
--     dominates pairs from the second branch.
--
--     The query :
--       SELECT city FROM personnel
--       UNION
--       SELECT p1.city FROM personnel p1, personnel p2
--        WHERE p1.city = p2.city AND p1.name < p2.name
--
--     Branch 1 yields one provenance leaf per personnel row.
--     Branch 2 yields gate_times(p1, p2) for every pair sharing a
--     city.  In the per-city plus, every gate_times(p1, p2) is
--     dominated by p1 (and p2) sitting alongside it in branch 1 :
--     absorption collapses every times-pair, leaving a per-city OR
--     over the branch-1 leaves alone.
--
--     This is unsound in Counting / Tropical / etc. but exact in
--     Boolean ; with provsql.boolean_provenance = on the circuit
--     becomes read-once and 'independent' evaluates it directly.
--     Without the GUC, independent throws "Not an independent
--     circuit" on the shared p1 / p2 leaves.
-- ----------------------------------------------------------------------
CREATE TABLE bf_personnel(id int, name text, city text);
INSERT INTO bf_personnel VALUES
  (1, 'Alice',  'Paris'),
  (2, 'Bob',    'Paris'),
  (3, 'Carol',  'Paris'),
  (4, 'Dave',   'Berlin'),
  (5, 'Ellen',  'Berlin');
SELECT add_provenance('bf_personnel');
DO $$ BEGIN PERFORM set_prob(provsql, 0.6) FROM bf_personnel; END $$;

-- Without absorption (and the rewriter), 'independent' fails on the
-- shared p1 / p2 leaves.
SET provsql.provenance = 'semiring';
DO $$ DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'independent')
      FROM (
        SELECT city FROM bf_personnel
        UNION
        SELECT p1.city FROM bf_personnel p1, bf_personnel p2
         WHERE p1.city = p2.city AND p1.name < p2.name
      ) t;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected ''independent'' to refuse the unrewritten '
                    'union with boolean_provenance = off';
  END IF;
END $$;

-- With absorption, the circuit becomes read-once and independent
-- yields exact per-city probabilities.  Berlin has 2 people, prob
-- 0.6 each : P = 1 - (1 - 0.6)^2 = 0.84.  Paris has 3, P = 1 -
-- 0.4^3 = 0.936.
SET provsql.provenance = 'boolean';
CREATE TEMP TABLE bf_abs_demo AS
  SELECT city, probability_evaluate(provenance(), 'independent') AS p
    FROM (
      SELECT city FROM bf_personnel
      UNION
      SELECT p1.city FROM bf_personnel p1, bf_personnel p2
       WHERE p1.city = p2.city AND p1.name < p2.name
    ) t;
SELECT remove_provenance('bf_abs_demo');
SELECT city, round(p::numeric, 6) AS p_independent
  FROM bf_abs_demo ORDER BY city;
DROP TABLE bf_abs_demo;
SET provsql.provenance = 'semiring';
DROP TABLE bf_personnel;

-- ----------------------------------------------------------------------
-- (6) Phase 3 single-wire collapse must PRESERVE SHARING of the target
--     leaf -- not mint an independent duplicate of it.
--
--     times(x, gate_one()) collapses to x (Phase 1 drops the one wire,
--     Phase 2/3 substitute the singleton).  When x is ALSO referenced
--     elsewhere in the same circuit, the pre-fix Phase 3 copied x's
--     content (type / prob / inputs-set membership) into the collapsed
--     gate under a fresh UUID, so the shared Bernoulli variable became
--     two independent copies -- over-counting every non-read-once
--     circuit (this is what corrupted cyclic recursive reachability
--     reliabilities under boolean_provenance: the gate_one recursion
--     seed produces exactly this times(shared_edge, one) shape).
--
--     root = plus(times(x, a), times(x, gate_one())).
--     Boolean function = (x AND a) OR x = x, so the exact probability
--     is P(x) = 0.5.  Pre-fix the duplicated x evaluated as
--     1 - (1 - 0.5*0.5)*(1 - 0.5) = 0.625.  possible-worlds is exact
--     and tolerates the (non-read-once) shared leaf.
-- ----------------------------------------------------------------------
DO $$
DECLARE x uuid; a uuid; tab uuid; t1 uuid; root uuid;
BEGIN
  SELECT provsql INTO x FROM bf_t WHERE id = 1;
  SELECT provsql INTO a FROM bf_t WHERE id = 2;
  tab  := provenance_times(x, a);
  t1   := uuid_generate_v5(uuid_ns_provsql(), concat('bf-share-one', x));
  PERFORM create_gate(t1, 'times', ARRAY[x, gate_one()]);
  root := uuid_generate_v5(uuid_ns_provsql(), concat('bf-share-root', x));
  PERFORM create_gate(root, 'plus', ARRAY[tab, t1]);
  PERFORM set_config('bf.share_root', root::text, false);
END $$;
SET provsql.provenance = 'boolean';
SELECT round(probability_evaluate(
                current_setting('bf.share_root')::uuid, 'possible-worlds')
              ::numeric, 9) AS shared_leaf_collapse_p;
SET provsql.provenance = 'semiring';

-- ----------------------------------------------------------------------
-- (7) Joint-fixpoint interleave of absorption (B3) and the single-wire
--     collapse (foldSemiringIdentities).
--
--     root = plus(times(x, x), times(x, y)).
--     B1 dedups times(x, x) to a single-wire times(x) ; the dominating
--     literal x is exposed as a direct sibling of the plus only once
--     foldSemiringIdentities rewrites that wrapper to x.  A
--     "B-rules to fixpoint, THEN collapse once" order would surface x too
--     late and leave plus(x, times(x, y)) -- not read-once, so
--     independent would refuse even with boolean_provenance on.  Interleaving
--     the two passes to a joint fixpoint absorbs times(x, y) and
--     collapses the whole circuit to the single leaf x.
--
--     Boolean function = (x AND x) OR (x AND y) = x, so the exact
--     probability is P(x) = 0.5 and the folded root is the input x.
-- ----------------------------------------------------------------------
DO $$
DECLARE x uuid; y uuid; diag uuid; off_diag uuid; root uuid;
BEGIN
  SELECT provsql INTO x FROM bf_t WHERE id = 1;
  SELECT provsql INTO y FROM bf_t WHERE id = 2;
  diag := uuid_generate_v5(uuid_ns_provsql(), concat('bf-interleave-diag', x));
  PERFORM create_gate(diag, 'times', ARRAY[x, x]);
  off_diag := provenance_times(x, y);
  root := uuid_generate_v5(uuid_ns_provsql(), concat('bf-interleave', x));
  PERFORM create_gate(root, 'plus', ARRAY[diag, off_diag]);
  PERFORM set_config('bf.interleave_root', root::text, false);
END $$;

-- Without folding the shared x leaves the circuit non-read-once and
-- independent refuses.
SET provsql.provenance = 'semiring';
DO $$ DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(
      current_setting('bf.interleave_root')::uuid, 'independent');
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected independent to refuse the unfolded interleave '
                    'circuit with boolean_provenance = off';
  END IF;
END $$;

-- With the interleaved joint fixpoint the circuit collapses to x :
-- independent yields P(x) = 0.5 and the folded root is the input gate.
SET provsql.provenance = 'boolean';
SELECT round(probability_evaluate(
                current_setting('bf.interleave_root')::uuid, 'independent')
              ::numeric, 9) AS interleave_root_p_ind;
SELECT (simplified_circuit_subgraph(
          current_setting('bf.interleave_root')::uuid, 1)
         ->0->>'gate_type') AS interleave_root_simplified_type;
SET provsql.provenance = 'semiring';

DROP TABLE bf_t_cnt;
DROP TABLE bf_t;
