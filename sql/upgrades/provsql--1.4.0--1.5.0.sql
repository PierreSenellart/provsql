/**
 * @file
 * @brief ProvSQL upgrade script: 1.4.0 -> 1.5.0
 *
 * 1.5.0 introduces continuous-distribution provenance and grows both
 * the on-disk gate enum and the SQL surface substantially:
 *
 * - Three new gate types (`rv`, `arith`, `mixture`) are appended to
 *   the `provenance_gate` enum.  The append-only ordering preserves
 *   every existing integer-to-name mapping, so 1.4.0 mmap circuits
 *   stay valid (no migration required).
 *
 * - A new `random_variable` composite type with its in/out functions,
 *   binary-coercible casts to/from `uuid`, implicit casts from
 *   `integer` / `numeric` / `double precision`, four distribution
 *   constructors (`normal`, `uniform`, `exponential`, `erlang`), two
 *   `mixture` overloads, a `categorical` constructor, and an
 *   `as_random` family.
 *
 * - Arithmetic and comparison operators `+ - * /` (binary), unary `-`,
 *   and the six comparators on `(random_variable, random_variable)`
 *   -- the comparators are placeholders that the planner hook rewrites
 *   into `gate_cmp`.  Direct `rv_cmp_*` constructors expose the same
 *   `gate_cmp` UUID outside the planner-hook path.
 *
 * - Aggregates `sum` / `avg` / `product` over `random_variable` and
 *   their `rv_aggregate_semimod` helper.
 *
 * - A polymorphic moment / support / sample / histogram surface:
 *   `expected`, `variance`, `moment`, `central_moment`, `support`,
 *   the rv-side internals `rv_moment`, `rv_support`, `rv_sample`,
 *   `rv_histogram`, `rv_analytical_curves`, and the agg-side internal
 *   `agg_raw_moment`.  Note: `expected()` was already shipped in 1.4.0
 *   with the same signature but its body is now a thin wrapper over
 *   `moment()`, so this script `CREATE OR REPLACE`s it.
 *
 * - A new `provenance_arith` constructor, an in-memory circuit-walk
 *   helper `simplified_circuit_subgraph`, and an internal
 *   `is_finite_float8` helper.
 *
 * - One existing function body changes:
 *   `provenance_times(VARIADIC uuid[])` now dispatches on the FILTERED
 *   token count so `[one, cmp]` collapses to `cmp` instead of being
 *   wrapped in a one-child times.  Signature is unchanged.
 *
 * Idempotency: `CREATE FUNCTION` calls use `CREATE OR REPLACE`; the
 * type, the two casts to/from uuid, the three implicit casts from
 * numeric types, the eleven operators, and the three aggregates are
 * each guarded by a `DO`-block existence check against the catalog
 * because they do not support `OR REPLACE`.  Re-running this script
 * is therefore safe.
 *
 * @warning ABI: the new gate types are appended at the end of the C
 * `gate_type` enum (`src/provsql_utils.h`), preserving every existing
 * integer-to-name mapping.  Existing 1.4.0 mmap stores remain valid.
 */

SET search_path TO provsql;

-- ----------------------------------------------------------------------
-- 1. New gate-type enum values, in the same order as the C enum.
-- ----------------------------------------------------------------------

ALTER TYPE provenance_gate ADD VALUE IF NOT EXISTS 'rv'      AFTER 'update';
ALTER TYPE provenance_gate ADD VALUE IF NOT EXISTS 'arith'   AFTER 'rv';
ALTER TYPE provenance_gate ADD VALUE IF NOT EXISTS 'mixture' AFTER 'arith';

-- ----------------------------------------------------------------------
-- 2. provenance_times body change.
--    Dispatch on the FILTERED count so [one, cmp] collapses to cmp
--    instead of being wrapped in a one-child times.  Signature is
--    unchanged.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION provenance_times(VARIADIC tokens uuid[])
  RETURNS UUID AS
$$
DECLARE
  times_token uuid;
  filtered_tokens uuid[];
BEGIN
  SELECT array_agg(t) FROM unnest(tokens) t WHERE t IS NOT NULL AND t <> gate_one() INTO filtered_tokens;

  CASE coalesce(array_length(filtered_tokens, 1), 0)
    WHEN 0 THEN
      times_token:=gate_one();
    WHEN 1 THEN
      times_token:=filtered_tokens[1];
    ELSE
      times_token := uuid_generate_v5(uuid_ns_provsql(),concat('times',filtered_tokens));

      PERFORM create_gate(times_token, 'times', ARRAY_AGG(t)) FROM UNNEST(filtered_tokens) AS t WHERE t IS NOT NULL;
  END CASE;

  RETURN times_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE;

-- ----------------------------------------------------------------------
-- 3. Arithmetic-gate constructor and circuit-introspection / sampling
--    helpers (the in-memory simplified subgraph, the histogram, the
--    analytical-curve sampler, the rejection sampler).
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION provenance_arith(
  op       INTEGER,
  children UUID[]
)
RETURNS UUID AS
$$
DECLARE
  arith_token UUID;
BEGIN
  arith_token := public.uuid_generate_v5(
    uuid_ns_provsql(),
    concat('arith', op::text, children::text)
  );
  PERFORM create_gate(arith_token, 'arith', children);
  PERFORM set_infos(arith_token, op);
  RETURN arith_token;
END
$$ LANGUAGE plpgsql
  SET search_path=provsql,pg_temp,public
  SECURITY DEFINER
  IMMUTABLE
  PARALLEL SAFE
  STRICT;

CREATE OR REPLACE FUNCTION simplified_circuit_subgraph(
  root UUID, max_depth INT DEFAULT 8) RETURNS jsonb
  AS 'provsql','simplified_circuit_subgraph'
  LANGUAGE C STABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rv_histogram(
  token UUID, bins INT DEFAULT 30, prov UUID DEFAULT gate_one())
  RETURNS jsonb
  AS 'provsql','rv_histogram'
  LANGUAGE C VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rv_analytical_curves(
  token UUID, samples INT DEFAULT 100, prov UUID DEFAULT gate_one())
  RETURNS jsonb
  AS 'provsql','rv_analytical_curves'
  LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rv_sample(
  token UUID, n integer, prov UUID DEFAULT gate_one())
  RETURNS SETOF float8
  AS 'provsql','rv_sample'
  LANGUAGE C VOLATILE PARALLEL SAFE;

-- ----------------------------------------------------------------------
-- 4. random_variable type, in/out functions, the constructor helper
--    random_variable_make, and the two binary-coercible casts to/from
--    uuid.  The shell type must be declared before its in/out C
--    functions, then the full type definition consumes them.
-- ----------------------------------------------------------------------

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_type t
      JOIN pg_namespace n ON n.oid = t.typnamespace
    WHERE n.nspname = 'provsql' AND t.typname = 'random_variable'
  ) THEN
    CREATE TYPE random_variable;
  END IF;
END $$;

CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

DO $$ BEGIN
  -- Detect whether the type is still a shell (typtype = 'p' = pseudo).
  -- Once promoted to a full base type by the second CREATE TYPE below
  -- typtype becomes 'b'.
  IF EXISTS (
    SELECT 1 FROM pg_type t
      JOIN pg_namespace n ON n.oid = t.typnamespace
    WHERE n.nspname = 'provsql' AND t.typname = 'random_variable'
      AND t.typtype = 'p'
  ) THEN
    CREATE TYPE random_variable (
      internallength = 16,
      input  = random_variable_in,
      output = random_variable_out,
      alignment = char
    );
  END IF;
END $$;

CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_cast
    WHERE castsource = 'provsql.random_variable'::regtype
      AND casttarget = 'uuid'::regtype
  ) THEN
    CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS IMPLICIT;
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_cast
    WHERE castsource = 'uuid'::regtype
      AND casttarget = 'provsql.random_variable'::regtype
  ) THEN
    CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;
  END IF;
END $$;

-- ----------------------------------------------------------------------
-- 5. Distribution constructors and lifting helpers.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

-- as_random must exist before normal / uniform / erlang reference it
-- in the sigma=0 / a=b / k=1 degenerate-routing branches.

CREATE OR REPLACE FUNCTION as_random(c double precision)
  RETURNS random_variable AS
$$
DECLARE
  c_canon double precision := c + 0.0;
  c_text varchar := CAST(c_canon AS VARCHAR);
  token uuid := public.uuid_generate_v5(
    provsql.uuid_ns_provsql(), concat('value', c_text));
BEGIN
  PERFORM provsql.create_gate(token, 'value');
  PERFORM provsql.set_extra(token, c_text);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION as_random(c integer)
  RETURNS random_variable AS
$$ SELECT provsql.as_random(c::double precision); $$
LANGUAGE sql STRICT IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION as_random(c numeric)
  RETURNS random_variable AS
$$ SELECT provsql.as_random(c::double precision); $$
LANGUAGE sql STRICT IMMUTABLE PARALLEL SAFE;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_cast
    WHERE castsource = 'double precision'::regtype
      AND casttarget = 'provsql.random_variable'::regtype
  ) THEN
    CREATE CAST (double precision AS random_variable)
      WITH FUNCTION provsql.as_random(double precision) AS IMPLICIT;
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_cast
    WHERE castsource = 'integer'::regtype
      AND casttarget = 'provsql.random_variable'::regtype
  ) THEN
    CREATE CAST (integer AS random_variable)
      WITH FUNCTION provsql.as_random(integer) AS IMPLICIT;
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_cast
    WHERE castsource = 'numeric'::regtype
      AND casttarget = 'provsql.random_variable'::regtype
  ) THEN
    CREATE CAST (numeric AS random_variable)
      WITH FUNCTION provsql.as_random(numeric) AS IMPLICIT;
  END IF;
END $$;

CREATE OR REPLACE FUNCTION normal(mu double precision, sigma double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(mu) OR NOT provsql.is_finite_float8(sigma) THEN
    RAISE EXCEPTION 'provsql.normal: parameters must be finite (got mu=%, sigma=%)', mu, sigma;
  END IF;
  IF sigma < 0 THEN
    RAISE EXCEPTION 'provsql.normal: sigma must be non-negative (got %)', sigma;
  END IF;
  IF sigma = 0 THEN
    RETURN provsql.as_random(mu);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'normal:' || mu || ',' || sigma);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION uniform(a double precision, b double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(a) OR NOT provsql.is_finite_float8(b) THEN
    RAISE EXCEPTION 'provsql.uniform: bounds must be finite (got a=%, b=%)', a, b;
  END IF;
  IF a > b THEN
    RAISE EXCEPTION 'provsql.uniform: a must be <= b (got a=%, b=%)', a, b;
  END IF;
  IF a = b THEN
    RETURN provsql.as_random(a);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'uniform:' || a || ',' || b);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION exponential(lambda double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(lambda) THEN
    RAISE EXCEPTION 'provsql.exponential: lambda must be finite (got %)', lambda;
  END IF;
  IF lambda <= 0 THEN
    RAISE EXCEPTION 'provsql.exponential: lambda must be strictly positive (got %)', lambda;
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'exponential:' || lambda);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION erlang(k integer, lambda double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF k < 1 THEN
    RAISE EXCEPTION 'provsql.erlang: k must be >= 1 (got %)', k;
  END IF;
  IF NOT provsql.is_finite_float8(lambda) THEN
    RAISE EXCEPTION 'provsql.erlang: lambda must be finite (got %)', lambda;
  END IF;
  IF lambda <= 0 THEN
    RAISE EXCEPTION 'provsql.erlang: lambda must be strictly positive (got %)', lambda;
  END IF;
  IF k = 1 THEN
    RETURN provsql.exponential(lambda);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'erlang:' || k || ',' || lambda);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION mixture(
  p uuid, x random_variable, y random_variable)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
  p_kind provsql.provenance_gate;
  x_uuid uuid;
  y_uuid uuid;
  x_kind provsql.provenance_gate;
  y_kind provsql.provenance_gate;
BEGIN
  p_kind := provsql.get_gate_type(p);
  IF p_kind NOT IN ('input','mulinput','update',
                    'plus','times','monus',
                    'project','eq','cmp',
                    'zero','one') THEN
    RAISE EXCEPTION 'provsql.mixture: p must be a Boolean gate '
                    '(input/mulinput/update/plus/times/monus/project/eq/cmp/zero/one), got %', p_kind;
  END IF;

  x_uuid := (x)::uuid;
  y_uuid := (y)::uuid;
  x_kind := provsql.get_gate_type(x_uuid);
  y_kind := provsql.get_gate_type(y_uuid);
  IF x_kind NOT IN ('rv','value','arith','mixture') THEN
    RAISE EXCEPTION 'provsql.mixture: x must be a scalar RV root (rv / value / arith / mixture), got %', x_kind;
  END IF;
  IF y_kind NOT IN ('rv','value','arith','mixture') THEN
    RAISE EXCEPTION 'provsql.mixture: y must be a scalar RV root (rv / value / arith / mixture), got %', y_kind;
  END IF;

  token := public.uuid_generate_v5(
    provsql.uuid_ns_provsql(),
    concat('mixture', p, x_uuid, y_uuid));
  PERFORM provsql.create_gate(token, 'mixture', ARRAY[p, x_uuid, y_uuid]);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION mixture(
  p_value double precision,
  x random_variable,
  y random_variable)
  RETURNS random_variable AS
$$
DECLARE
  p_token uuid;
BEGIN
  IF p_value IS NULL OR p_value <> p_value OR p_value < 0 OR p_value > 1 THEN
    RAISE EXCEPTION 'provsql.mixture: probability must be in [0,1] (got %)', p_value;
  END IF;
  p_token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(p_token, 'input');
  PERFORM provsql.set_prob(p_token, p_value);
  RETURN provsql.mixture(p_token, x, y);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION categorical(
  probs    double precision[],
  outcomes double precision[])
  RETURNS random_variable AS
$$
DECLARE
  n integer;
  p_sum double precision := 0.0;
  i integer;
  key_token uuid;
  mix_token uuid;
  mul_token uuid;
  mul_tokens uuid[] := ARRAY[]::uuid[];
  mix_wires  uuid[];
  pi_i double precision;
  vi_i double precision;
BEGIN
  IF probs IS NULL OR outcomes IS NULL THEN
    RAISE EXCEPTION 'provsql.categorical: probs and outcomes must be non-null';
  END IF;
  n := array_length(probs, 1);
  IF n IS NULL OR n < 1 THEN
    RAISE EXCEPTION 'provsql.categorical: probs must be non-empty';
  END IF;
  IF array_length(outcomes, 1) <> n THEN
    RAISE EXCEPTION 'provsql.categorical: probs and outcomes must have the same length (got % and %)',
      n, array_length(outcomes, 1);
  END IF;

  FOR i IN 1..n LOOP
    pi_i := probs[i];
    vi_i := outcomes[i];
    IF pi_i IS NULL OR pi_i = 'NaN'::float8 OR pi_i < 0 OR pi_i > 1 THEN
      RAISE EXCEPTION 'provsql.categorical: probs[%] must be in [0,1] (got %)', i, pi_i;
    END IF;
    IF vi_i IS NULL OR vi_i = 'NaN'::float8
       OR vi_i = 'Infinity'::float8 OR vi_i = '-Infinity'::float8 THEN
      RAISE EXCEPTION 'provsql.categorical: outcomes[%] must be finite (got %)', i, vi_i;
    END IF;
    p_sum := p_sum + pi_i;
  END LOOP;
  IF abs(p_sum - 1.0) > 1e-9 THEN
    RAISE EXCEPTION 'provsql.categorical: probs must sum to 1 within 1e-9 (got %)', p_sum;
  END IF;

  DECLARE
    nb_positive integer := 0;
    only_idx    integer := 0;
  BEGIN
    FOR i IN 1..n LOOP
      IF probs[i] > 0.0 THEN
        nb_positive := nb_positive + 1;
        only_idx := i;
      END IF;
    END LOOP;
    IF nb_positive = 1 THEN
      RETURN provsql.as_random(outcomes[only_idx]);
    END IF;
  END;

  key_token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(key_token, 'input');
  PERFORM provsql.set_prob(key_token, 1.0);

  FOR i IN 1..n LOOP
    pi_i := probs[i];
    IF pi_i <= 0.0 THEN CONTINUE; END IF;
    mul_token := public.uuid_generate_v4();
    PERFORM provsql.create_gate(mul_token, 'mulinput', ARRAY[key_token]);
    PERFORM provsql.set_prob(mul_token, pi_i);
    PERFORM provsql.set_infos(mul_token, (i - 1));
    PERFORM provsql.set_extra(mul_token, outcomes[i]::text);
    mul_tokens := mul_tokens || mul_token;
  END LOOP;

  mix_wires := ARRAY[key_token] || mul_tokens;
  mix_token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(mix_token, 'mixture', mix_wires);
  RETURN provsql.random_variable_make(mix_token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

-- ----------------------------------------------------------------------
-- 6. Arithmetic and comparison helpers, plus the operator declarations.
--    Arithmetic helpers build gate_arith via provenance_arith.  The six
--    comparison procedures are placeholders rewritten by the planner
--    hook; the rv_cmp_* family builds the gate_cmp UUID directly for
--    callers that want to bypass the hook.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION random_variable_plus(
  a random_variable, b random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      0,  -- PROVSQL_ARITH_PLUS
      ARRAY[(a)::uuid,
            (b)::uuid]));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_minus(
  a random_variable, b random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      2,  -- PROVSQL_ARITH_MINUS
      ARRAY[(a)::uuid,
            (b)::uuid]));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_times(
  a random_variable, b random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      1,  -- PROVSQL_ARITH_TIMES
      ARRAY[(a)::uuid,
            (b)::uuid]));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_div(
  a random_variable, b random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      3,  -- PROVSQL_ARITH_DIV
      ARRAY[(a)::uuid,
            (b)::uuid]));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_neg(a random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      4,  -- PROVSQL_ARITH_NEG
      ARRAY[(a)::uuid]));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_cmp_oid(sym text)
  RETURNS oid AS
$$
  SELECT (sym || '(double precision,double precision)')::regoperator::oid;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_cmp_placeholder(
  a random_variable, b random_variable)
  RETURNS boolean AS
$$
BEGIN
  RAISE EXCEPTION 'random_variable comparison must be rewritten by the '
                  'ProvSQL planner hook (is provsql.active off?)';
END
$$ LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_lt(
  a random_variable, b random_variable) RETURNS boolean AS
$$ SELECT provsql.random_variable_cmp_placeholder(a, b); $$
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_le(
  a random_variable, b random_variable) RETURNS boolean AS
$$ SELECT provsql.random_variable_cmp_placeholder(a, b); $$
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_eq(
  a random_variable, b random_variable) RETURNS boolean AS
$$ SELECT provsql.random_variable_cmp_placeholder(a, b); $$
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_ne(
  a random_variable, b random_variable) RETURNS boolean AS
$$ SELECT provsql.random_variable_cmp_placeholder(a, b); $$
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_ge(
  a random_variable, b random_variable) RETURNS boolean AS
$$ SELECT provsql.random_variable_cmp_placeholder(a, b); $$
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_gt(
  a random_variable, b random_variable) RETURNS boolean AS
$$ SELECT provsql.random_variable_cmp_placeholder(a, b); $$
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rv_cmp_lt(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    (a)::uuid,
    provsql.random_variable_cmp_oid('<'),
    (b)::uuid);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rv_cmp_le(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    (a)::uuid,
    provsql.random_variable_cmp_oid('<='),
    (b)::uuid);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rv_cmp_eq(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    (a)::uuid,
    provsql.random_variable_cmp_oid('='),
    (b)::uuid);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rv_cmp_ne(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    (a)::uuid,
    provsql.random_variable_cmp_oid('<>'),
    (b)::uuid);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rv_cmp_ge(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    (a)::uuid,
    provsql.random_variable_cmp_oid('>='),
    (b)::uuid);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rv_cmp_gt(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    (a)::uuid,
    provsql.random_variable_cmp_oid('>'),
    (b)::uuid);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

-- Operators on (random_variable, random_variable).  Each is guarded by
-- a pg_operator existence check on the (provsql, oprname, left, right)
-- key so re-running the script is idempotent.

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '+'
      AND o.oprleft = 'provsql.random_variable'::regtype
      AND o.oprright = 'provsql.random_variable'::regtype
  ) THEN
    CREATE OPERATOR + (
      LEFTARG    = random_variable,
      RIGHTARG   = random_variable,
      PROCEDURE  = random_variable_plus,
      COMMUTATOR = +
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '-'
      AND o.oprleft = 'provsql.random_variable'::regtype
      AND o.oprright = 'provsql.random_variable'::regtype
  ) THEN
    CREATE OPERATOR - (
      LEFTARG    = random_variable,
      RIGHTARG   = random_variable,
      PROCEDURE  = random_variable_minus
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '*'
      AND o.oprleft = 'provsql.random_variable'::regtype
      AND o.oprright = 'provsql.random_variable'::regtype
  ) THEN
    CREATE OPERATOR * (
      LEFTARG    = random_variable,
      RIGHTARG   = random_variable,
      PROCEDURE  = random_variable_times,
      COMMUTATOR = *
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '/'
      AND o.oprleft = 'provsql.random_variable'::regtype
      AND o.oprright = 'provsql.random_variable'::regtype
  ) THEN
    CREATE OPERATOR / (
      LEFTARG    = random_variable,
      RIGHTARG   = random_variable,
      PROCEDURE  = random_variable_div
    );
  END IF;
END $$;

-- Prefix unary minus: oprleft is 0 for unary operators in pg_operator.
DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '-'
      AND o.oprleft = 0
      AND o.oprright = 'provsql.random_variable'::regtype
  ) THEN
    CREATE OPERATOR - (
      RIGHTARG  = random_variable,
      PROCEDURE = random_variable_neg
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '<'
      AND o.oprleft = 'provsql.random_variable'::regtype
      AND o.oprright = 'provsql.random_variable'::regtype
  ) THEN
    CREATE OPERATOR < (
      LEFTARG    = random_variable,
      RIGHTARG   = random_variable,
      PROCEDURE  = random_variable_lt,
      COMMUTATOR = >,
      NEGATOR    = >=
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '<='
      AND o.oprleft = 'provsql.random_variable'::regtype
      AND o.oprright = 'provsql.random_variable'::regtype
  ) THEN
    CREATE OPERATOR <= (
      LEFTARG    = random_variable,
      RIGHTARG   = random_variable,
      PROCEDURE  = random_variable_le,
      COMMUTATOR = >=,
      NEGATOR    = >
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '='
      AND o.oprleft = 'provsql.random_variable'::regtype
      AND o.oprright = 'provsql.random_variable'::regtype
  ) THEN
    CREATE OPERATOR = (
      LEFTARG    = random_variable,
      RIGHTARG   = random_variable,
      PROCEDURE  = random_variable_eq,
      COMMUTATOR = =,
      NEGATOR    = <>
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '<>'
      AND o.oprleft = 'provsql.random_variable'::regtype
      AND o.oprright = 'provsql.random_variable'::regtype
  ) THEN
    CREATE OPERATOR <> (
      LEFTARG    = random_variable,
      RIGHTARG   = random_variable,
      PROCEDURE  = random_variable_ne,
      COMMUTATOR = <>,
      NEGATOR    = =
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '>='
      AND o.oprleft = 'provsql.random_variable'::regtype
      AND o.oprright = 'provsql.random_variable'::regtype
  ) THEN
    CREATE OPERATOR >= (
      LEFTARG    = random_variable,
      RIGHTARG   = random_variable,
      PROCEDURE  = random_variable_ge,
      COMMUTATOR = <=,
      NEGATOR    = <
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '>'
      AND o.oprleft = 'provsql.random_variable'::regtype
      AND o.oprright = 'provsql.random_variable'::regtype
  ) THEN
    CREATE OPERATOR > (
      LEFTARG    = random_variable,
      RIGHTARG   = random_variable,
      PROCEDURE  = random_variable_gt,
      COMMUTATOR = <,
      NEGATOR    = <=
    );
  END IF;
END $$;

-- ----------------------------------------------------------------------
-- 7. Aggregates over random_variable (sum / avg / product) and their
--    state-transition / final-function helpers.  rv_aggregate_semimod
--    is the per-row mixture wrapper used by the planner hook to lift
--    SUM(rv) into the provenance-aware form.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION rv_aggregate_semimod(
  prov uuid, rv random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.mixture(prov, rv, provsql.as_random(0::double precision));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION sum_rv_sfunc(
  state uuid[], rv random_variable)
  RETURNS uuid[] AS
$$
  SELECT CASE
    WHEN rv IS NULL THEN state
    ELSE array_append(state, (rv)::uuid)
  END;
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION sum_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
DECLARE
  arith_token uuid;
BEGIN
  IF state IS NULL OR array_length(state, 1) IS NULL THEN
    RETURN provsql.as_random(0::double precision);
  END IF;
  IF array_length(state, 1) = 1 THEN
    RETURN provsql.random_variable_make(state[1]);
  END IF;
  arith_token := provsql.provenance_arith(0, state);  -- 0 = PROVSQL_ARITH_PLUS
  RETURN provsql.random_variable_make(arith_token);
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION avg_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
DECLARE
  n integer;
  i integer;
  num_token uuid;
  denom_token uuid;
  denom_state uuid[] := '{}';
  one_uuid uuid;
  gtype provsql.provenance_gate;
  children uuid[];
  prov_i uuid;
BEGIN
  IF state IS NULL THEN
    RETURN NULL;
  END IF;
  n := array_length(state, 1);
  IF n IS NULL THEN
    RETURN NULL;
  END IF;

  one_uuid := (
                provsql.as_random(1::double precision))::uuid;

  FOR i IN 1..n LOOP
    gtype := provsql.get_gate_type(state[i]);
    IF gtype = 'mixture'::provsql.provenance_gate THEN
      children := provsql.get_children(state[i]);
      prov_i := children[1];
      denom_state := array_append(
        denom_state,
        (
          provsql.rv_aggregate_semimod(
            prov_i, provsql.as_random(1::double precision)))::uuid);
    ELSE
      denom_state := array_append(denom_state, one_uuid);
    END IF;
  END LOOP;

  IF n = 1 THEN
    num_token := state[1];
    denom_token := denom_state[1];
  ELSE
    num_token := provsql.provenance_arith(0, state);          -- 0 = PLUS
    denom_token := provsql.provenance_arith(0, denom_state);  -- 0 = PLUS
  END IF;

  RETURN provsql.random_variable_make(
    provsql.provenance_arith(
      3,  -- 3 = PROVSQL_ARITH_DIV
      ARRAY[num_token, denom_token]));
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION product_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
DECLARE
  n integer;
  i integer;
  prod_state uuid[] := '{}';
  one_rv provsql.random_variable;
  gtype provsql.provenance_gate;
  children uuid[];
  prov_i uuid;
  x_uuid uuid;
BEGIN
  one_rv := provsql.as_random(1::double precision);

  IF state IS NULL THEN
    RETURN one_rv;
  END IF;
  n := array_length(state, 1);
  IF n IS NULL THEN
    RETURN one_rv;
  END IF;

  FOR i IN 1..n LOOP
    gtype := provsql.get_gate_type(state[i]);
    IF gtype = 'mixture'::provsql.provenance_gate THEN
      children := provsql.get_children(state[i]);
      prov_i := children[1];
      x_uuid := children[2];
      prod_state := array_append(
        prod_state,
        (
          provsql.mixture(
            prov_i,
            provsql.random_variable_make(x_uuid),
            one_rv))::uuid);
    ELSE
      prod_state := array_append(prod_state, state[i]);
    END IF;
  END LOOP;

  IF n = 1 THEN
    RETURN provsql.random_variable_make(prod_state[1]);
  END IF;
  RETURN provsql.random_variable_make(
    provsql.provenance_arith(1, prod_state));  -- 1 = PROVSQL_ARITH_TIMES
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'sum'
      AND p.pronargs = 1
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE sum(random_variable) (
      SFUNC     = sum_rv_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = sum_rv_ffunc
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'avg'
      AND p.pronargs = 1
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE avg(random_variable) (
      SFUNC     = sum_rv_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = avg_rv_ffunc
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'product'
      AND p.pronargs = 1
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE product(random_variable) (
      SFUNC     = sum_rv_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = product_rv_ffunc
    );
  END IF;
END $$;

-- ----------------------------------------------------------------------
-- 8. Polymorphic moment / support surface.
--    rv_moment / rv_support are the rv-side C entry points;
--    agg_raw_moment handles the agg_token side; expected / variance /
--    moment / central_moment / support are the user-facing polymorphic
--    dispatchers (expected() existed in 1.4.0 with the same signature
--    but a different body).
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION rv_moment(
  token uuid, k integer, central boolean,
  prov uuid DEFAULT gate_one())
  RETURNS double precision
  AS 'provsql','rv_moment' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rv_support(
  token uuid, prov uuid DEFAULT gate_one(),
  OUT lo float8, OUT hi float8)
  AS 'provsql','rv_support' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_raw_moment(
  token agg_token,
  k integer,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
DECLARE
  aggregation_function VARCHAR;
  child_pairs uuid[];
  pair_children uuid[];
  n integer;
  i integer;
  j integer;
  vals float8[];
  toks uuid[];
  total float8;
  total_probability float8;
  tup integer[];
  d integer;
  prod_v float8;
  distinct_tok uuid[];
  conj_token uuid;
  prob float8;
  sign_max float8;
BEGIN
  IF token IS NULL OR k IS NULL THEN
    RETURN NULL;
  END IF;
  IF k < 0 THEN
    RAISE EXCEPTION 'agg_raw_moment(): k must be non-negative (got %)', k;
  END IF;
  IF get_gate_type(token) <> 'agg' THEN
    RAISE EXCEPTION USING MESSAGE='Wrong gate type for agg_raw_moment computation';
  END IF;
  IF k = 0 THEN
    RETURN 1;
  END IF;

  SELECT pp.proname::varchar FROM pg_proc pp
    WHERE oid=(get_infos(token)).info1
    INTO aggregation_function;

  child_pairs := get_children(token);
  n := COALESCE(array_length(child_pairs, 1), 0);

  IF aggregation_function = 'sum' THEN
    IF n = 0 THEN
      RETURN 0;
    END IF;

    vals := ARRAY[]::float8[];
    toks := ARRAY[]::uuid[];
    FOR i IN 1..n LOOP
      pair_children := get_children(child_pairs[i]);
      toks := toks || pair_children[1];
      vals := vals || CAST(get_extra(pair_children[2]) AS float8);
    END LOOP;

    total := 0;
    tup := array_fill(1, ARRAY[k]);
    LOOP
      prod_v := 1;
      FOR j IN 1..k LOOP
        prod_v := prod_v * vals[tup[j]];
      END LOOP;

      SELECT array_agg(DISTINCT toks[idx]) INTO distinct_tok
        FROM unnest(tup) AS idx;

      IF prov <> gate_one() THEN
        distinct_tok := distinct_tok || prov;
      END IF;
      conj_token := provenance_times(VARIADIC distinct_tok);
      prob := probability_evaluate(conj_token, method, arguments);

      total := total + prod_v * prob;

      d := k;
      WHILE d >= 1 AND tup[d] = n LOOP
        tup[d] := 1;
        d := d - 1;
      END LOOP;
      EXIT WHEN d = 0;
      tup[d] := tup[d] + 1;
    END LOOP;
  ELSIF aggregation_function = 'min' OR aggregation_function = 'max' THEN
    sign_max := CASE
                  WHEN aggregation_function = 'max'
                  THEN power(-1::float8, k)
                  ELSE 1
                END;

    WITH tok_value AS (
      SELECT (get_children(c))[1] AS tok,
             (CASE WHEN aggregation_function='max' THEN -1 ELSE 1 END)
               * CAST(get_extra((get_children(c))[2]) AS DOUBLE PRECISION) AS v
      FROM UNNEST(child_pairs) AS c
    ) SELECT probability_evaluate(provenance_monus(prov, provenance_plus(ARRAY_AGG(tok))))
        FROM tok_value
        INTO total_probability;

    IF total_probability > epsilon() THEN
      total := sign_max * 'Infinity'::float8;
    ELSE
      WITH tok_value AS (
        SELECT (get_children(c))[1] AS tok,
               (CASE WHEN aggregation_function='max' THEN -1 ELSE 1 END)
                 * CAST(get_extra((get_children(c))[2]) AS DOUBLE PRECISION) AS v
        FROM UNNEST(child_pairs) AS c
      ) SELECT sign_max * SUM(p * power(v, k)) FROM (
          SELECT t1.v AS v,
            probability_evaluate(
              provenance_monus(provenance_plus(ARRAY_AGG(t1.tok)),
                               provenance_plus(ARRAY_AGG(t2.tok))),
              method, arguments) AS p
          FROM tok_value t1 LEFT OUTER JOIN tok_value t2 ON t1.v > t2.v
          GROUP BY t1.v) tmp
        INTO total;
    END IF;
  ELSE
    RAISE EXCEPTION USING MESSAGE=
      'Cannot compute moment for aggregation function ' || aggregation_function;
  END IF;

  IF prov <> gate_one()
     AND total <> 0
     AND total <> 'Infinity'::float8
     AND total <> '-Infinity'::float8 THEN
    total := total / probability_evaluate(prov, method, arguments);
  END IF;

  RETURN total;
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

-- moment() must be created before expected(), since expected() is now
-- a thin wrapper over it.

CREATE OR REPLACE FUNCTION moment(
  input ANYELEMENT,
  k integer,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
BEGIN
  IF pg_typeof(input) = 'random_variable'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    RETURN provsql.rv_moment(
      (input::random_variable)::uuid, k, false, prov);
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    RETURN agg_raw_moment(input::agg_token, k, prov, method, arguments);
  END IF;

  RAISE EXCEPTION 'moment() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

-- expected() existed in 1.4.0 with the same signature; CREATE OR
-- REPLACE swaps its body for the new thin-wrapper-over-moment form.

CREATE OR REPLACE FUNCTION expected(
  input ANYELEMENT,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
  SELECT moment(input, 1, prov, method, arguments);
$$ LANGUAGE sql PARALLEL SAFE STABLE SET search_path=provsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION variance(
  input ANYELEMENT,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
DECLARE
  m1 float8;
  m2 float8;
BEGIN
  IF pg_typeof(input) = 'random_variable'::regtype THEN
    IF input IS NULL THEN
      RETURN NULL;
    END IF;
    RETURN provsql.rv_moment(
      (input::random_variable)::uuid, 2, true, prov);
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    IF input IS NULL THEN
      RETURN NULL;
    END IF;
    m1 := agg_raw_moment(input::agg_token, 1, prov, method, arguments);
    m2 := agg_raw_moment(input::agg_token, 2, prov, method, arguments);
    IF m1 IS NULL OR m2 IS NULL THEN
      RETURN NULL;
    END IF;
    RETURN m2 - m1 * m1;
  END IF;

  RAISE EXCEPTION 'variance() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION central_moment(
  input ANYELEMENT,
  k integer,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
DECLARE
  mu float8;
  total float8;
  i integer;
  raw_i float8;
  binom float8;
  k_double float8;
BEGIN
  IF pg_typeof(input) = 'random_variable'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    RETURN provsql.rv_moment(
      (input::random_variable)::uuid, k, true, prov);
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    IF k < 0 THEN
      RAISE EXCEPTION 'central_moment(): k must be non-negative (got %)', k;
    END IF;
    IF k = 0 THEN RETURN 1; END IF;
    IF k = 1 THEN RETURN 0; END IF;

    mu := agg_raw_moment(input::agg_token, 1, prov, method, arguments);
    IF mu IS NULL THEN RETURN NULL; END IF;
    IF mu = 'Infinity'::float8 OR mu = '-Infinity'::float8 THEN
      RETURN mu;
    END IF;

    total := 0;
    binom := 1;  -- C(k, 0)
    k_double := k;
    FOR i IN 0..k LOOP
      raw_i := agg_raw_moment(input::agg_token, i, prov, method, arguments);
      IF raw_i IS NULL THEN RETURN NULL; END IF;
      total := total + binom * power(-mu, k - i) * raw_i;
      IF i < k THEN
        binom := binom * (k_double - i) / (i + 1);
      END IF;
    END LOOP;
    RETURN total;
  END IF;

  RAISE EXCEPTION 'central_moment() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION support(
  input ANYELEMENT,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL,
  OUT lo float8,
  OUT hi float8)
  AS $$
DECLARE
  aggregation_function VARCHAR;
  child_pairs uuid[];
  values_arr float8[];
  total_probability float8;
BEGIN
  IF input IS NULL THEN
    lo := NULL; hi := NULL; RETURN;
  END IF;

  IF pg_typeof(input) IN (
       'smallint'::regtype, 'integer'::regtype, 'bigint'::regtype,
       'numeric'::regtype, 'real'::regtype, 'double precision'::regtype) THEN
    lo := input::double precision;
    hi := input::double precision;
    RETURN;
  END IF;

  IF pg_typeof(input) IN ('random_variable'::regtype, 'uuid'::regtype) THEN
    SELECT r.lo, r.hi INTO lo, hi
      FROM provsql.rv_support(input::uuid, prov) r;
    RETURN;
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    IF get_gate_type(input::agg_token) <> 'agg' THEN
      RAISE EXCEPTION USING MESSAGE='Wrong gate type for support computation';
    END IF;
    SELECT pp.proname::varchar FROM pg_proc pp
      WHERE oid=(get_infos(input::agg_token)).info1
      INTO aggregation_function;
    child_pairs := get_children(input::agg_token);

    IF aggregation_function = 'sum' THEN
      IF COALESCE(array_length(child_pairs, 1), 0) = 0 THEN
        lo := 0; hi := 0; RETURN;
      END IF;
      SELECT sum(LEAST(v, 0::float8)), sum(GREATEST(v, 0::float8))
        INTO lo, hi
        FROM (SELECT CAST(get_extra((get_children(c))[2]) AS float8) AS v
              FROM unnest(child_pairs) AS c) sub;
    ELSIF aggregation_function = 'min' OR aggregation_function = 'max' THEN
      IF COALESCE(array_length(child_pairs, 1), 0) = 0 THEN
        IF aggregation_function = 'min' THEN
          lo := 'Infinity'::float8; hi := 'Infinity'::float8;
        ELSE
          lo := '-Infinity'::float8; hi := '-Infinity'::float8;
        END IF;
        RETURN;
      END IF;

      WITH tok_value AS (
        SELECT (get_children(c))[1] AS tok,
               CAST(get_extra((get_children(c))[2]) AS float8) AS v
        FROM UNNEST(child_pairs) AS c
      )
      SELECT min(v), max(v),
             probability_evaluate(
               provenance_monus(prov, provenance_plus(ARRAY_AGG(tok))),
               method, arguments)
        INTO lo, hi, total_probability
        FROM tok_value;

      IF total_probability > epsilon() THEN
        IF aggregation_function = 'min' THEN
          hi := 'Infinity'::float8;
        ELSE
          lo := '-Infinity'::float8;
        END IF;
      END IF;
    ELSE
      RAISE EXCEPTION USING MESSAGE=
        'Cannot compute support for aggregation function ' || aggregation_function;
    END IF;
    RETURN;
  END IF;

  RAISE EXCEPTION 'support() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

-- ----------------------------------------------------------------------
-- Final step: invalidate the per-session OID constants cache.
--
-- The C side caches the OID of each `provenance_gate` enum value at
-- session-first-use of get_constants(); the three new values appended
-- above (`rv`, `arith`, `mixture`) are not in that cache for any
-- backend that warmed it under 1.4.0.  Without this reset, the first
-- create_gate(_, 'rv') in the upgrading session raises
-- "ProvSQL: Invalid gate type".  reset_constants_cache() forces a
-- fresh look-up on next get_constants(), so the new enum values
-- become usable immediately after ALTER EXTENSION provsql UPDATE.
-- ----------------------------------------------------------------------

SELECT reset_constants_cache();
