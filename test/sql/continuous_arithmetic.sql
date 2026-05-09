\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Each binary arithmetic operator builds a gate_arith carrying the
-- expected provsql_arith_op tag in info1
-- (PLUS=0, TIMES=1, MINUS=2, DIV=3, NEG=4) and returns a new
-- random_variable whose UUID is the gate's UUID.  We use as_random
-- for stable UUIDs so the test reads as a pure structural check.
-- COALESCE the info1 reads to 0 because get_infos returns NULL when
-- both info1 and info2 are 0 (see provsql_mmap.c:550); the in-memory
-- circuit still stores info1=0 verbatim, so the sampler reads PLUS
-- correctly &ndash; the COALESCE only fixes the SQL accessor.

WITH e AS (
  SELECT provsql.as_random(2) AS a, provsql.as_random(3) AS b
)
SELECT
  get_gate_type((a + b)::uuid) AS plus_kind,
  COALESCE((get_infos((a + b)::uuid)).info1, 0) AS plus_op,
  get_gate_type((a - b)::uuid) AS minus_kind,
  COALESCE((get_infos((a - b)::uuid)).info1, 0) AS minus_op,
  get_gate_type((a * b)::uuid) AS times_kind,
  COALESCE((get_infos((a * b)::uuid)).info1, 0) AS times_op,
  get_gate_type((a / b)::uuid) AS div_kind,
  COALESCE((get_infos((a / b)::uuid)).info1, 0) AS div_op,
  get_gate_type((-a)::uuid)    AS neg_kind,
  COALESCE((get_infos((-a)::uuid)).info1, 0) AS neg_op
FROM e;

-- Children of each arith gate are the operand UUIDs in operand order.
WITH e AS (
  SELECT provsql.as_random(2) AS a, provsql.as_random(3) AS b
)
SELECT
  get_children((a + b)::uuid) = ARRAY[a::uuid, b::uuid] AS plus_children,
  get_children((a - b)::uuid) = ARRAY[a::uuid, b::uuid] AS minus_children_lr,
  get_children((b - a)::uuid) = ARRAY[b::uuid, a::uuid] AS minus_children_rl,
  get_children((-a)::uuid)    = ARRAY[a::uuid]          AS neg_children
FROM e;

-- provenance_arith is deterministic on (op, children): two writes of
-- the same expression land on the same gate (the v5 derivation in the
-- helper makes this so).  Different operators on the same children land
-- on different gates because the op tag enters the namespace key.
WITH e AS (
  SELECT provsql.as_random(2) AS a, provsql.as_random(3) AS b
)
SELECT
  (a + b)::uuid =  (a + b)::uuid AS plus_dedup,
  (a + b)::uuid <> (a * b)::uuid AS plus_neq_times,
  (a - b)::uuid <> (b - a)::uuid AS minus_orientation_matters
FROM e;

-- Implicit casts: PG operator resolution falls back on the implicit
-- integer/numeric/float8 -> random_variable casts declared in
-- priority 1 because we only declared (rv, rv) operators.  Mixed
-- shapes are equivalent to wrapping the literal in as_random by hand.
WITH e AS (SELECT provsql.as_random(2) AS a)
SELECT
  (a + 2)::uuid           = (a + provsql.as_random(2))::uuid           AS plus_int,
  (a + 2.5)::uuid         = (a + provsql.as_random(2.5))::uuid         AS plus_numeric,
  (a + 2.5::float8)::uuid = (a + provsql.as_random(2.5::float8))::uuid AS plus_float8,
  (2 + a)::uuid           = (provsql.as_random(2) + a)::uuid           AS plus_reversed,
  (a > 2)::uuid           = (a > provsql.as_random(2))::uuid           AS gt_int,
  (2 < a)::uuid           = (provsql.as_random(2) < a)::uuid           AS lt_reversed
FROM e;

-- Comparison operators build a gate_cmp whose info1 holds the float8
-- comparator OID (any OID with the right opname works -- cmpOpFromOid
-- in src/Aggregation.cpp keys on the symbol).  Children are the two
-- operand UUIDs in left-right order.
WITH e AS (
  SELECT provsql.as_random(2) AS a, provsql.as_random(3) AS b
)
SELECT
  get_gate_type(a < b)  AS lt_kind,
  get_gate_type(a <= b) AS le_kind,
  get_gate_type(a = b)  AS eq_kind,
  get_gate_type(a <> b) AS ne_kind,
  get_gate_type(a >= b) AS ge_kind,
  get_gate_type(a > b)  AS gt_kind,
  get_children(a > b)   = ARRAY[a::uuid, b::uuid] AS gt_children
FROM e;

-- The OID stored in info1 round-trips through pg_operator back to
-- the symbol.  Pin the six comparators.
WITH e AS (
  SELECT provsql.as_random(2) AS a, provsql.as_random(3) AS b
)
SELECT
  (SELECT oprname FROM pg_operator
    WHERE oid = ((get_infos(a < b)).info1)::oid)  AS lt_sym,
  (SELECT oprname FROM pg_operator
    WHERE oid = ((get_infos(a <= b)).info1)::oid) AS le_sym,
  (SELECT oprname FROM pg_operator
    WHERE oid = ((get_infos(a = b)).info1)::oid)  AS eq_sym,
  (SELECT oprname FROM pg_operator
    WHERE oid = ((get_infos(a <> b)).info1)::oid) AS ne_sym,
  (SELECT oprname FROM pg_operator
    WHERE oid = ((get_infos(a >= b)).info1)::oid) AS ge_sym,
  (SELECT oprname FROM pg_operator
    WHERE oid = ((get_infos(a > b)).info1)::oid)  AS gt_sym
FROM e;

-- Composition: (a + b) > c builds a gate_cmp whose left child is a
-- gate_arith over (a, b) and whose right child is c.  This is the
-- shape the planner hook will produce in priority 4.
WITH e AS (
  SELECT provsql.as_random(1) AS a,
         provsql.as_random(2) AS b,
         provsql.as_random(3) AS c
)
SELECT
  get_gate_type((a + b) > c)                      AS root_kind,
  get_gate_type((get_children((a + b) > c))[1])   AS lhs_kind,
  get_gate_type((get_children((a + b) > c))[2])   AS rhs_kind,
  COALESCE((get_infos((get_children((a + b) > c))[1])).info1, 0) AS lhs_arith_op
FROM e;

-- End-to-end Monte Carlo sanity check.  P(N(0,1) + N(0,1) > 0) = 0.5
-- by symmetry: the operator-built circuit must agree with the
-- hand-built one in continuous_sampler.sql.  100k samples with a
-- pinned seed lands well inside 0.01 of the truth.
SET provsql.monte_carlo_seed = 42;
WITH r AS (
  SELECT provsql.normal(0, 1) AS x, provsql.normal(0, 1) AS y
)
SELECT abs(provsql.probability_evaluate(
             (x + y) > 0,
             'monte-carlo', '100000') - 0.5) < 0.01 AS sum_of_normals_gt_zero
FROM r;

-- Same expression, mixed shape: P(U(0,1) + U(0,1) > 1) = 0.5 by
-- symmetry (Irwin-Hall on [0,2] is symmetric around 1).  Exercises
-- the rv + rv > literal path (literal is auto-cast to random_variable).
WITH r AS (
  SELECT provsql.uniform(0, 1) AS x, provsql.uniform(0, 1) AS y
)
SELECT abs(provsql.probability_evaluate(
             (x + y) > 1,
             'monte-carlo', '100000') - 0.5) < 0.01 AS sum_of_uniforms_gt_one
FROM r;
RESET provsql.monte_carlo_seed;

SELECT 'ok'::text AS continuous_arithmetic_done;
