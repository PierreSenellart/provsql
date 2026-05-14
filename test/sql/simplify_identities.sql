\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- foldSemiringIdentities (gated by provsql.simplify_on_load) drops
-- identity wires from gate_plus / gate_times and collapses the
-- empty-sum / empty-product to the corresponding identity gate.
-- We assert the simplified shape via simplified_circuit_subgraph,
-- which dispatches through the in-memory loader so the fold result is
-- visible.

SET provsql.simplify_on_load = on;

-- Helper: count the unique nodes and root gate_type of the simplified
-- subgraph rooted at @p g.  A successful empty-sum / empty-product
-- collapse leaves a single-node circuit with the identity gate type.
CREATE TEMP TABLE sim_q (label text, root_kind text, n_nodes int);

-- (1) plus(zero, zero) collapses to gate_zero.  Empty sum is the
-- additive identity.
DO $$
DECLARE g uuid := gen_random_uuid();
  rows jsonb;
BEGIN
  PERFORM provsql.create_gate(g, 'plus',
            ARRAY[provsql.gate_zero(), provsql.gate_zero()]);
  rows := provsql.simplified_circuit_subgraph(g, 5);
  INSERT INTO sim_q VALUES
    ('plus_of_two_zeros',
     rows -> 0 ->> 'gate_type',
     (SELECT COUNT(DISTINCT e->>'node') FROM jsonb_array_elements(rows) e)::int);
END $$;

-- (2) times(one, one) collapses to gate_one.  Empty product is the
-- multiplicative identity.
DO $$
DECLARE g uuid := gen_random_uuid();
  rows jsonb;
BEGIN
  PERFORM provsql.create_gate(g, 'times',
            ARRAY[provsql.gate_one(), provsql.gate_one()]);
  rows := provsql.simplified_circuit_subgraph(g, 5);
  INSERT INTO sim_q VALUES
    ('times_of_two_ones',
     rows -> 0 ->> 'gate_type',
     (SELECT COUNT(DISTINCT e->>'node') FROM jsonb_array_elements(rows) e)::int);
END $$;

-- (3) Nested plus(plus(zero,zero), plus(zero,zero)) cascades to
-- gate_zero through the fixpoint loop in Phase 1: inner pluses
-- collapse to gate_zero, then the outer plus's wires all become
-- identity and it collapses too.
DO $$
DECLARE g     uuid := gen_random_uuid();
        i1    uuid := gen_random_uuid();
        i2    uuid := gen_random_uuid();
  rows jsonb;
BEGIN
  PERFORM provsql.create_gate(i1, 'plus',
            ARRAY[provsql.gate_zero(), provsql.gate_zero()]);
  PERFORM provsql.create_gate(i2, 'plus',
            ARRAY[provsql.gate_zero(), provsql.gate_zero()]);
  PERFORM provsql.create_gate(g, 'plus', ARRAY[i1, i2]);
  rows := provsql.simplified_circuit_subgraph(g, 5);
  INSERT INTO sim_q VALUES
    ('plus_of_two_empty_pluses',
     rows -> 0 ->> 'gate_type',
     (SELECT COUNT(DISTINCT e->>'node') FROM jsonb_array_elements(rows) e)::int);
END $$;

-- (4) times whose first wire is an empty-product times and second
-- wire is an empty-sum plus.  Phase 1 collapses the inner gates;
-- Phase 2's gate_times absorber then resolves the outer times to
-- gate_zero (multiplicative zero is the universal absorber).
DO $$
DECLARE g     uuid := gen_random_uuid();
        i_one uuid := gen_random_uuid();
        i_zero uuid := gen_random_uuid();
  rows jsonb;
BEGIN
  PERFORM provsql.create_gate(i_one, 'times',
            ARRAY[provsql.gate_one(), provsql.gate_one()]);
  PERFORM provsql.create_gate(i_zero, 'plus',
            ARRAY[provsql.gate_zero(), provsql.gate_zero()]);
  PERFORM provsql.create_gate(g, 'times', ARRAY[i_one, i_zero]);
  rows := provsql.simplified_circuit_subgraph(g, 5);
  INSERT INTO sim_q VALUES
    ('times_of_empty_product_and_empty_sum',
     rows -> 0 ->> 'gate_type',
     (SELECT COUNT(DISTINCT e->>'node') FROM jsonb_array_elements(rows) e)::int);
END $$;

-- (5) plus over two times-with-zero-wire children.  Phase 2's
-- gate_times absorber mutates each inner times to gate_zero, which
-- then becomes a fresh empty-sum opportunity for the outer plus on a
-- subsequent iteration of the fixpoint loop.  Models the RangeCheck-
-- driven shape where every UNION branch's gate_cmp is decided false
-- (e.g. WHERE pm25>35 on uniforms with support [10,22] and [15,28]).
DO $$
DECLARE g    uuid := gen_random_uuid();
        i1   uuid := gen_random_uuid();
        i2   uuid := gen_random_uuid();
        in1  uuid := gen_random_uuid();
        in2  uuid := gen_random_uuid();
  rows jsonb;
BEGIN
  PERFORM provsql.create_gate(in1, 'input');
  PERFORM provsql.create_gate(in2, 'input');
  PERFORM provsql.create_gate(i1, 'times', ARRAY[in1, provsql.gate_zero()]);
  PERFORM provsql.create_gate(i2, 'times', ARRAY[in2, provsql.gate_zero()]);
  PERFORM provsql.create_gate(g, 'plus', ARRAY[i1, i2]);
  rows := provsql.simplified_circuit_subgraph(g, 5);
  INSERT INTO sim_q VALUES
    ('plus_of_two_zero_absorbed_times',
     rows -> 0 ->> 'gate_type',
     (SELECT COUNT(DISTINCT e->>'node') FROM jsonb_array_elements(rows) e)::int);
END $$;

SELECT label, root_kind, n_nodes FROM sim_q ORDER BY label;

DROP TABLE sim_q;
