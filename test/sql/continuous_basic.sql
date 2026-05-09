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
