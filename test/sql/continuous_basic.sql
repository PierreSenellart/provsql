\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Constructors create the right gate types and serialise their
-- distribution parameters into the gate's extra byte string.

SELECT get_gate_type(random_variable_uuid(provsql.normal(2.5, 0.5))) AS rv_normal_kind;
SELECT get_extra(random_variable_uuid(provsql.normal(2.5, 0.5))) AS rv_normal_extra;

SELECT get_gate_type(random_variable_uuid(provsql.uniform(1, 3))) AS rv_uniform_kind;
SELECT get_extra(random_variable_uuid(provsql.uniform(1, 3))) AS rv_uniform_extra;

SELECT get_gate_type(random_variable_uuid(provsql.exponential(0.7))) AS rv_exp_kind;
SELECT get_extra(random_variable_uuid(provsql.exponential(0.7))) AS rv_exp_extra;

SELECT get_gate_type(random_variable_uuid(provsql.erlang(3, 0.5))) AS rv_erlang_kind;
SELECT get_extra(random_variable_uuid(provsql.erlang(3, 0.5))) AS rv_erlang_extra;

-- as_random creates a gate_value (constant), not a gate_rv.
SELECT get_gate_type(random_variable_uuid(provsql.as_random(42))) AS const_kind;
SELECT get_extra(random_variable_uuid(provsql.as_random(42))) AS const_extra;

-- The cached scalar value mirrors the constructor argument for
-- as_random; for actual distributions it is NaN.
SELECT random_variable_value(provsql.as_random(7.25)) AS as_random_value;
SELECT random_variable_value(provsql.normal(0, 1)) = 'NaN'::float8 AS normal_value_is_nan;

-- Text IO round-trips: reparsing the printed form must produce a
-- struct whose UUID and cached value match the original.
WITH r AS (SELECT provsql.as_random(3.14) AS v)
SELECT
  random_variable_uuid(v::text::random_variable) = random_variable_uuid(v) AS uuid_roundtrip,
  random_variable_value(v::text::random_variable) = random_variable_value(v) AS value_roundtrip
FROM r;

-- Implicit cast random_variable -> uuid yields the same UUID as the
-- explicit random_variable_uuid() function.
WITH r AS (SELECT provsql.normal(0, 1) AS v)
SELECT v::uuid = random_variable_uuid(v) AS implicit_cast_to_uuid FROM r;

-- A column of a real table can hold random_variable values and survive
-- through the pg_dump-shaped text representation back into a row.
CREATE TABLE sensors(id text, reading provsql.random_variable);
INSERT INTO sensors VALUES
  ('s1', provsql.normal(2.5, 0.5)),
  ('s2', provsql.uniform(1, 3)),
  ('s3', provsql.exponential(0.7)),
  ('s4', provsql.as_random(2));
SELECT id, get_gate_type(reading::uuid) AS gate_type FROM sensors ORDER BY id;
SELECT id, get_extra(reading::uuid) AS extra FROM sensors ORDER BY id;
DROP TABLE sensors;

-- as_random is IMMUTABLE with a deterministic UUID derived from the
-- constant: two calls for the same value resolve to the same gate.
SELECT random_variable_uuid(provsql.as_random(2)) = random_variable_uuid(provsql.as_random(2)) AS as_random_deterministic;
-- Different constants give different UUIDs.
SELECT random_variable_uuid(provsql.as_random(2)) <> random_variable_uuid(provsql.as_random(3)) AS as_random_distinct;

-- Implicit cast double precision -> random_variable: a float8 literal
-- in a random_variable context is auto-lifted to the same gate
-- as_random would produce.
SELECT random_variable_uuid(2.5::double precision::random_variable)
     = random_variable_uuid(provsql.as_random(2.5::double precision)) AS implicit_cast_dedup_float8;
-- Direct integer cast (PG operator resolution does not chain casts
-- across multiple steps, so int -> random_variable is registered as
-- its own cast via an as_random(integer) overload).
SELECT random_variable_uuid(7::integer::random_variable)
     = random_variable_uuid(provsql.as_random(7::integer)) AS int_literal_via_cast;
-- Direct numeric cast for "2.5"-style literals (PG's default literal
-- type for unquoted decimals is numeric).
SELECT random_variable_uuid(2.5::numeric::random_variable)
     = random_variable_uuid(provsql.as_random(2.5::numeric)) AS numeric_literal_via_cast;

-- The continuous-distribution constructors are VOLATILE so that each
-- call mints a FRESH uuid_generate_v4(): two calls to normal(0, 1) in
-- the same query must produce *independent* random variables.  If
-- anyone weakens these to STABLE or IMMUTABLE, PostgreSQL would fold
-- the call and the resulting circuit would silently collapse the two
-- RVs into one shared gate, breaking the c-table model.  These tests
-- pin the contract.
SELECT random_variable_uuid(provsql.normal(0, 1))
    <> random_variable_uuid(provsql.normal(0, 1)) AS normal_calls_independent;
SELECT random_variable_uuid(provsql.uniform(0, 1))
    <> random_variable_uuid(provsql.uniform(0, 1)) AS uniform_calls_independent;
SELECT random_variable_uuid(provsql.exponential(1))
    <> random_variable_uuid(provsql.exponential(1)) AS exponential_calls_independent;
SELECT random_variable_uuid(provsql.erlang(3, 1))
    <> random_variable_uuid(provsql.erlang(3, 1)) AS erlang_calls_independent;

-- Erlang(1, λ) is exactly Exp(λ): the constructor silently routes
-- through exponential so the gate's extra reflects the underlying
-- exponential form, sharing the entire downstream sampler / analytic
-- path with vanilla Exp(λ).
SELECT get_extra(random_variable_uuid(provsql.erlang(1, 0.7))) AS erlang_one_routes_to_exp;

-- Degenerate distributions: silently routed through as_random so the
-- resulting gate is a gate_value, sharing its UUID with as_random(x).
SELECT get_gate_type(random_variable_uuid(provsql.normal(5, 0))) AS normal_zero_sigma_kind;
SELECT random_variable_uuid(provsql.normal(5, 0))
     = random_variable_uuid(provsql.as_random(5)) AS normal_zero_sigma_dedups_with_as_random;
SELECT get_gate_type(random_variable_uuid(provsql.uniform(7, 7))) AS uniform_degenerate_kind;
SELECT random_variable_uuid(provsql.uniform(7, 7))
     = random_variable_uuid(provsql.as_random(7)) AS uniform_degenerate_dedups_with_as_random;

-- Rejected parameters.  Use VERBOSITY terse to keep error output
-- compact; restore default afterwards so other tests in the suite are
-- unaffected if they share the connection.
\set VERBOSITY terse
SELECT provsql.normal('NaN'::float8, 1);
SELECT provsql.normal(0, 'Infinity'::float8);
SELECT provsql.normal(0, -1);
SELECT provsql.uniform('NaN'::float8, 1);
SELECT provsql.uniform(3, 1);
SELECT provsql.exponential('Infinity'::float8);
SELECT provsql.exponential(0);
SELECT provsql.exponential(-0.5);
SELECT provsql.erlang(0, 1);
SELECT provsql.erlang(-2, 1);
SELECT provsql.erlang(3, 'NaN'::float8);
SELECT provsql.erlang(3, 0);
SELECT provsql.erlang(3, -0.5);
\set VERBOSITY default

-- as_random allows non-finite floats: NaN and ±Infinity are valid
-- float8 values with well-defined IEEE 754 comparison semantics.
-- Each round-trips through the text IO and produces a distinct gate.
SELECT random_variable_value(provsql.as_random('NaN'::float8)) = 'NaN'::float8 AS as_random_nan_value;
SELECT random_variable_value(provsql.as_random('Infinity'::float8))
     = 'Infinity'::float8 AS as_random_pos_inf_value;
SELECT random_variable_value(provsql.as_random('-Infinity'::float8))
     = '-Infinity'::float8 AS as_random_neg_inf_value;
WITH r AS (SELECT provsql.as_random('NaN'::float8) AS v)
SELECT random_variable_uuid(v::text::random_variable) = random_variable_uuid(v) AS nan_text_roundtrip FROM r;
-- The three special values produce three distinct gates.
SELECT count(DISTINCT u) AS distinct_special_uuids
  FROM (VALUES
    (random_variable_uuid(provsql.as_random('NaN'::float8))),
    (random_variable_uuid(provsql.as_random('Infinity'::float8))),
    (random_variable_uuid(provsql.as_random('-Infinity'::float8)))
  ) AS t(u);

-- IEEE 754 has two zeros (-0.0 and +0.0) but they denote the same
-- constant.  as_random canonicalises -0.0 to +0.0 so both yield the
-- same gate, and uniform(-0, +0) (which routes through as_random)
-- inherits the canonicalisation.
SELECT random_variable_uuid(provsql.as_random((-0)::float8))
     = random_variable_uuid(provsql.as_random((0)::float8)) AS as_random_signed_zero_dedup;
SELECT random_variable_uuid(provsql.uniform((-0)::float8, (0)::float8))
     = random_variable_uuid(provsql.as_random((0)::float8)) AS uniform_signed_zero_dedup;
