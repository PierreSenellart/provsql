-- ----------------------------------------------------------------------
-- provsql 1.8.0 -> 1.9.0
--
-- New SQL surface since 1.8.0:
--   * comparison of an aggregate (agg_token) with a text constant:
--     the placeholder procedures agg_token_comp_text / text_comp_agg_token
--     and the = / <> operators between agg_token and text (rewritten by
--     ProvSQL at plan time, like the existing agg_token/numeric operators);
--   * explode_table(text, text), which expands an agg_token column back
--     into one row per aggregated child, recombining value and provenance.
-- ----------------------------------------------------------------------

SET search_path TO provsql;

-- 1. Placeholder comparison procedures.  Never actually executed: they
--    only let the parser accept agg_token <-> text comparisons, which the
--    ProvSQL query rewriter replaces at plan time.
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

-- 2. = / <> operators between agg_token and text (both argument orders).
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

-- 3. explode_table: expand an aggregate column into one row per child.
CREATE OR REPLACE FUNCTION explode_table(_tbl text, agg_token text)
RETURNS void AS $$
DECLARE
  _nsp text;
BEGIN
    -- Resolve the schema actually holding _tbl so the rebuilt table is
    -- recreated in place (the provsql helper functions are schema-qualified
    -- so this works whatever the caller's search_path is).
    SELECT n.nspname INTO _nsp
    FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE c.oid = _tbl::regclass;

    EXECUTE format('
    CREATE TABLE %1$I.temp_exploded AS
    SELECT
        %2$I.*,
        provsql.get_extra(children[2]) AS new_t,
        provsql.provenance_times(children[1], provsql) AS new_provsql
    FROM %1$I.%2$I,
    LATERAL (
        SELECT provsql.get_children(sm) AS children
        FROM UNNEST(provsql.get_children(%3$I)) AS sm
    ) AS sub', _nsp, _tbl, agg_token);
    EXECUTE format('DROP TABLE %I.%I', _nsp, _tbl);
    EXECUTE format('ALTER TABLE %I.temp_exploded DROP COLUMN %I, DROP COLUMN provsql', _nsp, agg_token);
    EXECUTE format('ALTER TABLE %I.temp_exploded RENAME COLUMN new_t TO %I', _nsp, agg_token);
    EXECUTE format('ALTER TABLE %I.temp_exploded RENAME COLUMN new_provsql TO provsql', _nsp);
    EXECUTE format('ALTER TABLE %I.temp_exploded RENAME TO %I', _nsp, _tbl);
END;
$$ LANGUAGE plpgsql;

-- 4. probability_bounds: cheap lower / upper marginal-probability bounds for a
--    monotone-DNF token (new C function).
CREATE OR REPLACE FUNCTION probability_bounds(
  token UUID,
  OUT lower DOUBLE PRECISION,
  OUT upper DOUBLE PRECISION) AS
  'provsql','probability_bounds' LANGUAGE C STABLE;

-- 5. Aggregate semantics: a NULL input never participates in an aggregate.
--    provenance_semimod now returns NULL for a NULL value (so it builds no
--    semimod gate), and provenance_aggregate drops those NULLs before building
--    the agg gate.  provenance_semimod's body changed, signature unchanged.
--    provenance_aggregate ALSO gained a fifth argument is_scalar (the empty-group
--    scalar-aggregation flag, hashed into the gate UUID and stored in the high
--    bit of info2): the argument count changed (4 -> 5), so the old 4-arg
--    function is dropped first and the cached OID is refreshed by the
--    reset_constants_cache() at the end.
CREATE OR REPLACE FUNCTION provenance_semimod(val anyelement, token UUID)
  RETURNS UUID AS
$$
DECLARE
  semimod_token uuid;
  value_token uuid;
BEGIN
  -- A NULL value means this row does not participate in the aggregate (SQL
  -- aggregates ignore NULL inputs; only count(*) counts rows unconditionally,
  -- and it passes a constant 1 here).  Produce no semimod gate so the row is
  -- skipped when provenance_aggregate builds the agg gate.
  IF val IS NULL THEN
    RETURN NULL;
  END IF;

  SELECT uuid_generate_v5(uuid_ns_provsql(),concat('value',CAST(val AS VARCHAR)))
    INTO value_token;
  SELECT uuid_generate_v5(uuid_ns_provsql(),concat('semimod',value_token,token))
    INTO semimod_token;

  --create value gates
  PERFORM create_gate(value_token,'value');
  PERFORM set_extra(value_token, CAST(val AS VARCHAR));

  --create semimod gate
  PERFORM create_gate(semimod_token,'semimod',ARRAY[token::uuid,value_token]);

  RETURN semimod_token;
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql,pg_temp,public SECURITY DEFINER;

DROP FUNCTION IF EXISTS provenance_aggregate(integer, integer, anyelement, uuid[]);
CREATE OR REPLACE FUNCTION provenance_aggregate(
    aggfnoid integer,
    aggtype integer,
    val anyelement,
    tokens uuid[],
    is_scalar boolean DEFAULT false)
  RETURNS agg_token AS
$$
DECLARE
  c INTEGER;
  agg_tok uuid;
  agg_val varchar;
BEGIN
  -- Drop the NULL placeholders array_agg keeps for rows that did not produce a
  -- semimod gate (provenance_semimod returns NULL for a NULL aggregated value),
  -- so a NULL input never participates in the aggregate.
  tokens := array_remove(tokens, NULL);
  c:=COALESCE(array_length(tokens, 1), 0);

  agg_val = CAST(val as VARCHAR);

  IF c = 0 THEN
    agg_tok := gate_zero();
  ELSE
    -- aggfnoid must be part of the UUID: SUM(id) and AVG(id) over the
    -- same children would otherwise collapse to a single gate, and
    -- their concurrent set_infos calls would overwrite each other's
    -- aggregation operator (resulting in the wrong agg_kind being
    -- read by provsql_having under cross-backend contention).  The
    -- scalar-aggregation flag must likewise be hashed: a scalar and a
    -- grouped aggregate over identical children carry different info2 and
    -- must stay distinct gates, else the concurrent set_infos calls would
    -- clobber the flag.  The flag is stored in the high bit of info2 (the
    -- low 31 bits keep the result-type OID); aggtype itself is passed clean
    -- so the agg_token->scalar cast still finds a valid type.
    agg_tok := uuid_generate_v5(
      uuid_ns_provsql(),
      concat('agg',aggfnoid,tokens,CASE WHEN is_scalar THEN 'S' ELSE '' END));
    PERFORM create_gate(agg_tok, 'agg', tokens);
    PERFORM set_infos(agg_tok, aggfnoid,
                      CASE WHEN is_scalar THEN aggtype | (-2147483648) ELSE aggtype END);
    PERFORM set_extra(agg_tok, agg_val);
  END IF;

  RETURN '( '||agg_tok||' , '||agg_val||' )';
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql,pg_temp,public SECURITY DEFINER;

-- 5b. Empty-group moment surface: the agg_token raw-moment and support
--     functions were made empty-group-faithful (min/max conditional on
--     non-empty so they stay finite; count(col) treated as a SUM of 0/1
--     indicators whose empty group is the real value 0).  Bodies changed,
--     signatures unchanged.  Replicated verbatim from provsql.common.sql.
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

  IF aggregation_function = 'sum' OR aggregation_function = 'count' THEN
    -- count(col) keeps the COUNT identity at the gate level but its value is a
    -- SUM of per-row 0/1 indicators, so its moments are computed exactly like
    -- SUM (and its empty group is the real value 0, like SUM).  count(*)
    -- arrives here as 'sum' (it normalises to F_SUM_INT4); count(col) as 'count'.
    -- Trivial empty aggregation: SUM = 0, so SUM^k = 0 for k >= 1.
    -- Note: agg_token semantics treat the "no row included" world as
    -- SUM = 0, so this stays consistent with k = 1 (= expected()).
    IF n = 0 THEN
      RETURN 0;
    END IF;

    -- Extract per-child token + value arrays.
    vals := ARRAY[]::float8[];
    toks := ARRAY[]::uuid[];
    FOR i IN 1..n LOOP
      pair_children := get_children(child_pairs[i]);
      toks := toks || pair_children[1];
      vals := vals || CAST(get_extra(pair_children[2]) AS float8);
    END LOOP;

    -- Enumerate all k-tuples (i_1, ..., i_k) in {1..n}^k.  tup is the
    -- current tuple; we step through them in lexicographic order.
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
    -- Rank enumeration: per distinct value v, P(MIN = v) is the
    -- probability that some t_i with v_i=v is true and all t_j with
    -- smaller v are false.  For MAX we negate values so the same
    -- "smaller-than" rank logic computes MIN-of-negated, then flip.
    -- The outer multiplier picks up the right sign for the k-th moment
    -- of MAX: E[MAX^k] = (-1)^k * E[MIN(-v)^k], so sign_max = (-1)^k.
    sign_max := CASE
                  WHEN aggregation_function = 'max'
                  THEN power(-1::float8, k)
                  ELSE 1
                END;

    -- MIN/MAX over the empty input world are NULL (no elements), not ±Infinity:
    -- SQL returns one row with a NULL value.  The moment is therefore CONDITIONAL
    -- on the aggregate being defined (non-empty) -- the empty world is excluded
    -- and the result renormalised by P(prov AND non-empty).  (count, whose empty
    -- value 0 is a real value, keeps the empty world; sum keeps it too, as 0.)
    IF n = 0 THEN
      RETURN NULL;  -- structurally empty: MIN/MAX undefined
    END IF;

    -- Numerator E[MIN^k . 1{prov AND non-empty}] (the rank sum naturally omits
    -- the empty world, since every term requires a present token).
    WITH tok_value AS (
      SELECT (get_children(c))[1] AS tok,
             (CASE WHEN aggregation_function='max' THEN -1 ELSE 1 END)
               * CAST(get_extra((get_children(c))[2]) AS DOUBLE PRECISION) AS v
      FROM UNNEST(child_pairs) AS c
    ) SELECT sign_max * COALESCE(SUM(p * power(v, k)), 0) FROM (
        SELECT t1.v AS v,
          probability_evaluate(
            CASE WHEN prov = gate_one()
                 THEN provenance_monus(provenance_plus(ARRAY_AGG(t1.tok)),
                                       provenance_plus(ARRAY_AGG(t2.tok)))
                 ELSE provenance_times(prov,
                        provenance_monus(provenance_plus(ARRAY_AGG(t1.tok)),
                                         provenance_plus(ARRAY_AGG(t2.tok)))) END,
            method, arguments) AS p
        FROM tok_value t1 LEFT OUTER JOIN tok_value t2 ON t1.v > t2.v
        GROUP BY t1.v) tmp
      INTO total;

    -- Denominator P(prov AND non-empty) = P(prov (x) (+) tokens).
    SELECT probability_evaluate(
             CASE WHEN prov = gate_one()
                  THEN provenance_plus(ARRAY_AGG(tok))
                  ELSE provenance_times(prov, provenance_plus(ARRAY_AGG(tok))) END,
             method, arguments)
      FROM (SELECT (get_children(c))[1] AS tok FROM UNNEST(child_pairs) AS c) s
      INTO total_probability;

    IF total_probability <= epsilon() THEN
      RETURN NULL;  -- never defined under prov: MIN/MAX undefined
    END IF;
    RETURN total / total_probability;  -- already conditional; skip generic norm
  ELSE
    RAISE EXCEPTION USING MESSAGE=
      'Cannot compute moment for aggregation function ' || aggregation_function;
  END IF;

  -- Conditional normalisation: E[X^k · 1_A] / P(A) = E[X^k | A].
  IF prov <> gate_one()
     AND total <> 0
     AND total <> 'Infinity'::float8
     AND total <> '-Infinity'::float8 THEN
    total := total / probability_evaluate(prov, method, arguments);
  END IF;

  RETURN total;
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

  -- Plain numeric: degenerate point support.  Lets `support(2.5)` /
  -- `support(42)` / etc.  return (2.5, 2.5) without making the user
  -- wrap in `as_random`.
  IF pg_typeof(input) IN (
       'smallint'::regtype, 'integer'::regtype, 'bigint'::regtype,
       'numeric'::regtype, 'real'::regtype, 'double precision'::regtype) THEN
    lo := input::double precision;
    hi := input::double precision;
    RETURN;
  END IF;

  -- random_variable has an IMPLICIT cast to uuid, so a single
  -- rv_support call covers both shapes.  rv_support handles
  -- gate_value (point), gate_rv (distribution), gate_arith
  -- (propagated), and falls back to the conservative all-real
  -- interval for any other gate kind.  Conditioning on prov is not
  -- supported (would require restricting the underlying joint
  -- distribution by the indicator of prov, which has no closed form
  -- for the basic distributions we ship).
  IF pg_typeof(input) IN ('random_variable'::regtype, 'uuid'::regtype) THEN
    -- Conditional support: rv_support folds the AND-conjunct interval
    -- constraints from prov into the unconditional support.  When
    -- prov is gate_one() the unconditional support is returned
    -- unchanged.
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

    IF aggregation_function = 'sum' OR aggregation_function = 'count' THEN
      -- count(col) is a SUM of per-row 0/1 indicators (empty group = 0), so its
      -- support is computed like SUM; count(*) arrives as 'sum'.
      -- Empty agg_token: SUM is identically 0.
      IF COALESCE(array_length(child_pairs, 1), 0) = 0 THEN
        lo := 0; hi := 0; RETURN;
      END IF;
      SELECT sum(LEAST(v, 0::float8)), sum(GREATEST(v, 0::float8))
        INTO lo, hi
        FROM (SELECT CAST(get_extra((get_children(c))[2]) AS float8) AS v
              FROM unnest(child_pairs) AS c) sub;
    ELSIF aggregation_function = 'min' OR aggregation_function = 'max' THEN
      -- MIN/MAX over the empty input world are NULL, not ±Infinity (matching the
      -- moment surface): the empty world carries no value, so the support is just
      -- the range of the per-row values [min(v), max(v)].  A structurally empty
      -- aggregate has no defined value at all -> NULL support.
      IF COALESCE(array_length(child_pairs, 1), 0) = 0 THEN
        lo := NULL; hi := NULL; RETURN;
      END IF;

      SELECT min(v), max(v)
        INTO lo, hi
        FROM (SELECT CAST(get_extra((get_children(c))[2]) AS float8) AS v
              FROM UNNEST(child_pairs) AS c) sub;
    ELSE
      RAISE EXCEPTION USING MESSAGE=
        'Cannot compute support for aggregation function ' || aggregation_function;
    END IF;
    RETURN;
  END IF;

  RAISE EXCEPTION 'support() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

-- 6. The probability_benchmark helpers (added in 1.7.0) were removed in 1.9.0;
--    drop them so an upgraded database matches a fresh install.
DROP FUNCTION IF EXISTS probability_benchmark(UUID, INT, TEXT);
DROP FUNCTION IF EXISTS _probability_benchmark_one(UUID, TEXT, TEXT);

-- 7. Arithmetic on aggregates (agg_token): native + - * / and unary -
--    operators that build a gate_arith over the operand provenance, plus
--    the agg_token <op> agg_token comparison diagonal, and demotion of the
--    agg_token -> numeric cast to ASSIGNMENT (so `s + 1` is provenance-
--    preserving arithmetic rather than a silent numeric coercion).
CREATE OR REPLACE FUNCTION agg_token_value(agg_token)
  RETURNS numeric
  AS 'provsql','agg_token_value' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_token_make(tok uuid, val numeric)
  RETURNS agg_token AS
$$ SELECT format('( %s , %s )', tok::text, val::text)::provsql.agg_token; $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

CREATE OR REPLACE FUNCTION agg_value_gate(v numeric)
  RETURNS uuid AS
$body$
DECLARE
  token uuid := public.uuid_generate_v5(
    provsql.uuid_ns_provsql(), concat('value', v::text));
BEGIN
  PERFORM provsql.create_gate(token, 'value');
  PERFORM provsql.set_extra(token, v::text);
  RETURN token;
END
$body$ LANGUAGE plpgsql STRICT IMMUTABLE PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE OR REPLACE FUNCTION agg_token_plus(a agg_token, b agg_token) RETURNS agg_token AS
$$ SELECT provsql.agg_token_make(provsql.provenance_arith(0, ARRAY[(a)::uuid,(b)::uuid]),
     provsql.agg_token_value(a)+provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;
CREATE OR REPLACE FUNCTION agg_token_minus(a agg_token, b agg_token) RETURNS agg_token AS
$$ SELECT provsql.agg_token_make(provsql.provenance_arith(2, ARRAY[(a)::uuid,(b)::uuid]),
     provsql.agg_token_value(a)-provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;
CREATE OR REPLACE FUNCTION agg_token_times(a agg_token, b agg_token) RETURNS agg_token AS
$$ SELECT provsql.agg_token_make(provsql.provenance_arith(1, ARRAY[(a)::uuid,(b)::uuid]),
     provsql.agg_token_value(a)*provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;
CREATE OR REPLACE FUNCTION agg_token_div(a agg_token, b agg_token) RETURNS agg_token AS
$$ SELECT provsql.agg_token_make(provsql.provenance_arith(3, ARRAY[(a)::uuid,(b)::uuid]),
     provsql.agg_token_value(a)/provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;
CREATE OR REPLACE FUNCTION agg_token_neg(a agg_token) RETURNS agg_token AS
$$ SELECT provsql.agg_token_make(provsql.provenance_arith(4, ARRAY[(a)::uuid]),
     - provsql.agg_token_value(a)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

CREATE OR REPLACE FUNCTION agg_token_plus_numeric(a agg_token, b numeric) RETURNS agg_token AS
$$ SELECT provsql.agg_token_make(provsql.provenance_arith(0, ARRAY[(a)::uuid,provsql.agg_value_gate(b)]),
     provsql.agg_token_value(a)+b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;
CREATE OR REPLACE FUNCTION agg_token_minus_numeric(a agg_token, b numeric) RETURNS agg_token AS
$$ SELECT provsql.agg_token_make(provsql.provenance_arith(2, ARRAY[(a)::uuid,provsql.agg_value_gate(b)]),
     provsql.agg_token_value(a)-b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;
CREATE OR REPLACE FUNCTION agg_token_times_numeric(a agg_token, b numeric) RETURNS agg_token AS
$$ SELECT provsql.agg_token_make(provsql.provenance_arith(1, ARRAY[(a)::uuid,provsql.agg_value_gate(b)]),
     provsql.agg_token_value(a)*b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;
CREATE OR REPLACE FUNCTION agg_token_div_numeric(a agg_token, b numeric) RETURNS agg_token AS
$$ SELECT provsql.agg_token_make(provsql.provenance_arith(3, ARRAY[(a)::uuid,provsql.agg_value_gate(b)]),
     provsql.agg_token_value(a)/b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

CREATE OR REPLACE FUNCTION numeric_plus_agg_token(a numeric, b agg_token) RETURNS agg_token AS
$$ SELECT provsql.agg_token_make(provsql.provenance_arith(0, ARRAY[provsql.agg_value_gate(a),(b)::uuid]),
     a+provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;
CREATE OR REPLACE FUNCTION numeric_minus_agg_token(a numeric, b agg_token) RETURNS agg_token AS
$$ SELECT provsql.agg_token_make(provsql.provenance_arith(2, ARRAY[provsql.agg_value_gate(a),(b)::uuid]),
     a-provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;
CREATE OR REPLACE FUNCTION numeric_times_agg_token(a numeric, b agg_token) RETURNS agg_token AS
$$ SELECT provsql.agg_token_make(provsql.provenance_arith(1, ARRAY[provsql.agg_value_gate(a),(b)::uuid]),
     a*provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;
CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token) RETURNS agg_token AS
$$ SELECT provsql.agg_token_make(provsql.provenance_arith(3, ARRAY[provsql.agg_value_gate(a),(b)::uuid]),
     a/provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
  RETURNS boolean LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END; $$;

-- Operators (plain CREATE, like section 2; this upgrade runs once).
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);
CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);
CREATE OPERATOR <  (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token, COMMUTATOR = >,  NEGATOR = >=);
CREATE OPERATOR <= (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token, COMMUTATOR = >=, NEGATOR = >);
CREATE OPERATOR >  (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token, COMMUTATOR = <,  NEGATOR = <=);
CREATE OPERATOR >= (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token, COMMUTATOR = <=, NEGATOR = <);
CREATE OPERATOR =  (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token, COMMUTATOR = =,  NEGATOR = <>);
CREATE OPERATOR <> (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token, COMMUTATOR = <>, NEGATOR = =);

-- Demote agg_token -> numeric from IMPLICIT to ASSIGNMENT (idempotent).
DO $$
BEGIN
  IF EXISTS (
    SELECT 1 FROM pg_cast c
      JOIN pg_type s ON s.oid = c.castsource
      JOIN pg_type t ON t.oid = c.casttarget
     WHERE s.typname = 'agg_token' AND t.typname = 'numeric'
       AND c.castcontext = 'i')
  THEN
    DROP CAST (agg_token AS numeric);
    CREATE CAST (agg_token AS numeric) WITH FUNCTION agg_token_to_numeric(agg_token) AS ASSIGNMENT;
  END IF;
END;
$$;

-- 8. Demote the random_variable -> uuid cast from IMPLICIT to ASSIGNMENT.
--    Operators are resolved through search_path but casts are not, so an
--    implicit cross-domain cast silently reroutes `v < w` to `uuid < uuid`
--    (raw byte comparison) whenever provsql is not in search_path.  As
--    ASSIGNMENT that becomes a clean parse error instead of wrong results.
--    The cast is extension-owned, so DROP/CREATE here re-registers it as an
--    extension member.  Idempotent: only acts while the cast is implicit.
DO $$
BEGIN
  IF EXISTS (
    SELECT 1 FROM pg_cast c
      JOIN pg_type s ON s.oid = c.castsource
      JOIN pg_type t ON t.oid = c.casttarget
     WHERE s.typname = 'random_variable' AND t.typname = 'uuid'
       AND c.castcontext = 'i')
  THEN
    DROP CAST (random_variable AS uuid);
    CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
  END IF;
END;
$$;

-- 9. setup_search_path(): append provsql to the database's default
--    search_path if missing.  See the function comment in
--    provsql.common.sql.
CREATE OR REPLACE FUNCTION setup_search_path()
  RETURNS text
  LANGUAGE plpgsql AS $body$
DECLARE
  db        text := current_database();
  cfg       text[];
  cur       text;
  new_path  text;
BEGIN
  SELECT s.setconfig INTO cfg
    FROM pg_db_role_setting s
    JOIN pg_database d ON d.oid = s.setdatabase
   WHERE d.datname = db AND s.setrole = 0;

  IF cfg IS NOT NULL THEN
    SELECT substr(e, length('search_path=') + 1) INTO cur
      FROM unnest(cfg) AS e
     WHERE e LIKE 'search_path=%';
  END IF;

  IF cur IS NULL THEN
    new_path := '"$user", public, provsql';
    EXECUTE format('ALTER DATABASE %I SET search_path = %s', db, new_path);
    RAISE NOTICE 'ProvSQL: set search_path = % for database "%" (no previous database-level setting). Only new sessions are affected.',
      new_path, db;
    RETURN new_path;
  END IF;

  IF EXISTS (
       SELECT 1 FROM unnest(string_to_array(cur, ',')) AS p
        WHERE btrim(btrim(p), '"') = 'provsql')
  THEN
    RAISE NOTICE 'ProvSQL: search_path for database "%" already contains provsql (= %); no change.',
      db, cur;
    RETURN cur;
  END IF;

  new_path := cur || ', provsql';
  EXECUTE format('ALTER DATABASE %I SET search_path = %s', db, new_path);
  RAISE NOTICE 'ProvSQL: appended provsql to search_path for database "%" (now: %). Only new sessions are affected.',
    db, new_path;
  RETURN new_path;
END;
$body$;


-- Idempotent re-runs of add_provenance / create_provenance_mapping:
-- both now NOTICE and return when their work is already done (notebook
-- cells and setup scripts re-run freely). PG14+ and pre-14
-- add_provenance bodies differ (statement-level update-provenance
-- triggers), hence the version branch.

DO $$
BEGIN
  IF current_setting('server_version_num')::int >= 140000 THEN
    EXECUTE $sql$
CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$func$
BEGIN
  -- Idempotence: a second add_provenance on an already-tracked table is
  -- a no-op with a NOTICE, so setup scripts and notebook cells can be
  -- re-run freely.
  IF EXISTS (
    SELECT 1 FROM pg_attribute
    WHERE attrelid = _tbl AND attname = 'provsql' AND NOT attisdropped
  ) THEN
    RAISE NOTICE 'table % already has provenance tracking', _tbl;
    RETURN;
  END IF;
  -- See the common-version body for the rationale of dropping the
  -- column DEFAULT and UNIQUE in favour of provenance_guard + a
  -- plain index.
  EXECUTE format('ALTER TABLE %s ADD COLUMN provsql UUID', _tbl);
  EXECUTE format(
    'UPDATE %s SET provsql = public.uuid_generate_v4() WHERE provsql IS NULL',
    _tbl);
  EXECUTE format('CREATE INDEX ON %s(provsql)', _tbl);
  EXECUTE format(
    'CREATE TRIGGER provenance_guard BEFORE INSERT OR UPDATE OF provsql '
    'ON %s FOR EACH ROW EXECUTE PROCEDURE provsql.provenance_guard()',
    _tbl);

  EXECUTE format('CREATE TRIGGER insert_statement AFTER INSERT ON %s REFERENCING NEW TABLE AS NEW_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.insert_statement_trigger()', _tbl);
  EXECUTE format('CREATE TRIGGER delete_statement AFTER DELETE ON %s REFERENCING OLD TABLE AS OLD_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.delete_statement_trigger()', _tbl);
  EXECUTE format('CREATE TRIGGER update_statement AFTER UPDATE ON %s REFERENCING OLD TABLE AS OLD_TABLE NEW TABLE AS NEW_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.update_statement_trigger()', _tbl);

  PERFORM provsql.set_table_info(_tbl::oid, 'tid');
  PERFORM provsql.set_ancestors(_tbl::oid, ARRAY[_tbl::oid]);
END
$func$ LANGUAGE plpgsql SECURITY DEFINER;
    $sql$;
  ELSE
    EXECUTE $sql$
CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$func$
BEGIN
  -- Idempotence: a second add_provenance on an already-tracked table is
  -- a no-op with a NOTICE, so setup scripts and notebook cells can be
  -- re-run freely.
  IF EXISTS (
    SELECT 1 FROM pg_attribute
    WHERE attrelid = _tbl AND attname = 'provsql' AND NOT attisdropped
  ) THEN
    RAISE NOTICE 'table % already has provenance tracking', _tbl;
    RETURN;
  END IF;
  -- No DEFAULT: the guard trigger mints the UUID, so the trigger can
  -- distinguish "user omitted" (NULL) from "user supplied a value".
  -- No UNIQUE: we no longer rely on it to keep the table TID -- the
  -- guard does that semantically -- and a UNIQUE would reject the
  -- legitimate cross-table UUID copy that just flips the table to
  -- OPAQUE.  We keep a plain index for fast UUID-keyed lookups.
  EXECUTE format('ALTER TABLE %s ADD COLUMN provsql UUID', _tbl);
  EXECUTE format(
    'UPDATE %s SET provsql = public.uuid_generate_v4() WHERE provsql IS NULL',
    _tbl);
  EXECUTE format('CREATE INDEX ON %s(provsql)', _tbl);
  EXECUTE format(
    'CREATE TRIGGER provenance_guard BEFORE INSERT OR UPDATE OF provsql '
    'ON %s FOR EACH ROW EXECUTE PROCEDURE provsql.provenance_guard()',
    _tbl);
  PERFORM provsql.set_table_info(_tbl::oid, 'tid');
  -- Seed the base-ancestor set to {self}: a base TID table's atoms
  -- come from itself and no other relation.  CTAS-derived tables
  -- inherit unions of source ancestor sets; that is handled by the
  -- CTAS hook (a separate slice), not here.
  PERFORM provsql.set_ancestors(_tbl::oid, ARRAY[_tbl::oid]);
END
$func$ LANGUAGE plpgsql SECURITY DEFINER;
    $sql$;
  END IF;
END
$$;

CREATE OR REPLACE FUNCTION create_provenance_mapping(
  newtbl text,
  oldtbl regclass,
  att text,
  preserve_case bool DEFAULT 'f'
) RETURNS void AS
$$
DECLARE
BEGIN
  -- Idempotence: when the mapping table already exists, leave it alone
  -- with a NOTICE (re-runnable setup scripts / notebook cells). Drop it
  -- first to rebuild a stale mapping.
  IF (CASE WHEN preserve_case THEN to_regclass(format('%I', newtbl))
           ELSE to_regclass(newtbl) END) IS NOT NULL THEN
    RAISE NOTICE 'mapping table % already exists', newtbl;
    RETURN;
  END IF;
  -- ON COMMIT DROP only fires at COMMIT: several mapping creations in
  -- one transaction (a notebook cell, a setup script run via psql -1)
  -- would otherwise collide on the leftover temp table. The to_regclass
  -- probe (rather than DROP IF EXISTS) keeps the first call NOTICE-free.
  IF to_regclass('pg_temp.tmp_provsql') IS NOT NULL THEN
    DROP TABLE tmp_provsql;
  END IF;
  EXECUTE format('CREATE TEMP TABLE tmp_provsql ON COMMIT DROP AS TABLE %s', oldtbl);
  ALTER TABLE tmp_provsql RENAME provsql TO provenance;
  IF preserve_case THEN
    EXECUTE format('CREATE TABLE %I AS SELECT %s AS value, provenance FROM tmp_provsql', newtbl, att);
    EXECUTE format('CREATE INDEX ON %I(provenance)', newtbl);
  ELSE
    EXECUTE format('CREATE TABLE %s AS SELECT %s AS value, provenance FROM tmp_provsql', newtbl, att);
    EXECUTE format('CREATE INDEX ON %s(provenance)', newtbl);
  END IF;
END
$$ LANGUAGE plpgsql;


-- agg_token arithmetic now records the computed scalar in the
-- gate_arith's extra (via agg_arith_make), and
-- agg_token_value_text recovers the "value (*)" display for
-- arith tokens like it does for agg ones (Studio result cells).

CREATE OR REPLACE FUNCTION agg_arith_make(op int, children uuid[], val numeric)
  RETURNS agg_token AS
$$
DECLARE
  token uuid := provsql.provenance_arith(op, children);
BEGIN
  PERFORM provsql.set_extra(token, val::text);
  RETURN provsql.agg_token_make(token, val);
END
$$ LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE OR REPLACE FUNCTION agg_token_value_text(token UUID)
  RETURNS text AS
$$
  SELECT CASE
    -- agg gates: extra is set by aggregate evaluation; arith gates
    -- (agg_token arithmetic): extra is recorded by agg_arith_make.
    WHEN provsql.get_gate_type(token) IN ('agg', 'arith')
      THEN provsql.get_extra(token) || ' (*)'
    ELSE NULL
  END;
$$ LANGUAGE sql STABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_token_neg(a agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(4, ARRAY[(a)::uuid],
     - provsql.agg_token_value(a)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- agg_token \<op\> numeric ----------------------------------------------
/** @brief agg_token + numeric (gate_arith PLUS, constant lifted to a value gate). */
CREATE OR REPLACE FUNCTION agg_token_plus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) + b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token - numeric. */
CREATE OR REPLACE FUNCTION agg_token_minus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) - b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token * numeric. */
CREATE OR REPLACE FUNCTION agg_token_times_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) * b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token / numeric. */
CREATE OR REPLACE FUNCTION agg_token_div_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) / b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- numeric \<op\> agg_token ----------------------------------------------
/** @brief numeric + agg_token. */
CREATE OR REPLACE FUNCTION numeric_plus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a + provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric - agg_token. */
CREATE OR REPLACE FUNCTION numeric_minus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric * agg_token. */
CREATE OR REPLACE FUNCTION numeric_times_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric / agg_token. */
CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- Operator declarations -----------------------------------------------
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);

CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);

CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);

/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

/**
 * @brief Placeholder comparison of agg_token with numeric
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and numeric values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_numeric(a agg_token, b numeric)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-numeric not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of numeric with agg_token
 *
 * Symmetric to agg_token_comp_numeric; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION numeric_comp_agg_token(a numeric, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison numeric-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >,
  NEGATOR    = >=
);
/** @brief SQL operator numeric < agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >,
  NEGATOR    = >=
);

/** @brief SQL operator agg_token <= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >=,
  NEGATOR    = >
);
/** @brief SQL operator numeric <= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >=,
  NEGATOR    = >
);

/** @brief SQL operator agg_token = numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator numeric = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator numeric <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @brief SQL operator agg_token >= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <=,
  NEGATOR    = <
);
/** @brief SQL operator numeric >= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <=,
  NEGATOR    = <
);

/** @brief SQL operator agg_token > numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <,
  NEGATOR    = <=
);
/** @brief SQL operator numeric > agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Placeholder comparison of two agg_token values (the diagonal)
 *
 * Never actually called; lets the parser accept agg_token \<op\> agg_token
 * (e.g. sum(x) > sum(y) on materialised tokens), which the ProvSQL
 * rewriter lowers to a gate_cmp at plan time.  Declaring this diagonal
 * also disambiguates `s = s2` (previously "operator is not unique"
 * because both agg_token -> uuid and agg_token -> numeric casts applied).
 */
CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR < (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >, NEGATOR = >=
);
/** @brief SQL operator agg_token <= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >=, NEGATOR = >
);
/** @brief SQL operator agg_token > agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR > (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <, NEGATOR = <=
);
/** @brief SQL operator agg_token >= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR >= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <=, NEGATOR = <
);
/** @brief SQL operator agg_token = agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR = (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = =, NEGATOR = <>
);
/** @brief SQL operator agg_token <> agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <> (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <>, NEGATOR = =
);

/**
 * @brief Placeholder comparison of agg_token with text
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and text values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of text with agg_token
 *
 * Symmetric to agg_token_comp_text; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token = text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator text = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator text <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt>: a thin wrapper around a
 *  provenance gate UUID, used to expose continuous probabilistic
 *  c-tables in SQL.  The UUID indexes either a <tt>gate_rv</tt>
 *  (an actual distribution) or a <tt>gate_value</tt> (a
 *  zero-variance constant produced by <tt>provsql.as_random</tt>).
 *  Binary-coercible with <tt>uuid</tt> (same 16-byte layout), so an
 *  <tt>rv</tt>-typed expression flows directly into any function
 *  expecting a uuid at zero runtime cost.
 *
 *  Constructors live in this group: <tt>provsql.normal(μ, σ)</tt>,
 *  <tt>provsql.uniform(a, b)</tt>, <tt>provsql.exponential(λ)</tt>,
 *  <tt>provsql.erlang(k, λ)</tt>, and <tt>provsql.as_random(c)</tt>.
 *  Operator overloads
 *  (<tt>+ - * /</tt> and the six comparators) are defined further
 *  below, alongside direct <tt>rv_cmp_*</tt> UUID constructors for
 *  callers that want a <tt>gate_cmp</tt> token without going through
 *  the planner hook.
 *  @{
 */

CREATE TYPE random_variable;

/** @brief Input function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Output function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE random_variable (
  internallength = 16,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = char
);

/** @brief Build a random_variable from a UUID (internal). */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Binary-coercible cast random_variable -> uuid.
 *  A random_variable is byte-for-byte a pg_uuid_t (alignment char,
 *  length 16), so WITHOUT FUNCTION lets PostgreSQL reinterpret the
 *  bytes at zero runtime cost.  The cast is ASSIGNMENT (not IMPLICIT):
 *  an implicit cross-domain cast would silently reroute a comparison
 *  such as `v < w` to `uuid < uuid` (raw byte comparison) whenever
 *  `provsql` is not in search_path, since operators are resolved
 *  through search_path but casts are not.  Demoting to ASSIGNMENT
 *  turns that silent wrong result into a clean parse error.  Passing a
 *  random_variable to a uuid-taking function now needs an explicit
 *  `v::uuid` (function resolution never applies assignment casts). */
CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;

/**
 * @brief Internal: true iff @p x is a finite (non-NaN, non-±∞) float8.
 *
 * PostgreSQL's <tt>isnan</tt> is defined for <tt>numeric</tt> only,
 * not for <tt>double precision</tt>; we use the inequality form,
 * which works because PG defines <tt>NaN = NaN</tt> as <tt>TRUE</tt>
 * for floats (so <tt>NaN <> 'NaN'::float8</tt> is <tt>FALSE</tt>).
 */
CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_token_plus(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) + provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token - agg_token (gate_arith MINUS). */
CREATE OR REPLACE FUNCTION agg_token_minus(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token * agg_token (gate_arith TIMES). */
CREATE OR REPLACE FUNCTION agg_token_times(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token / agg_token (gate_arith DIV). */
CREATE OR REPLACE FUNCTION agg_token_div(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief Unary -agg_token (gate_arith NEG). */
CREATE OR REPLACE FUNCTION agg_token_neg(a agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(4, ARRAY[(a)::uuid],
     - provsql.agg_token_value(a)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- agg_token \<op\> numeric ----------------------------------------------
/** @brief agg_token + numeric (gate_arith PLUS, constant lifted to a value gate). */
CREATE OR REPLACE FUNCTION agg_token_plus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) + b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token - numeric. */
CREATE OR REPLACE FUNCTION agg_token_minus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) - b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token * numeric. */
CREATE OR REPLACE FUNCTION agg_token_times_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) * b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token / numeric. */
CREATE OR REPLACE FUNCTION agg_token_div_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) / b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- numeric \<op\> agg_token ----------------------------------------------
/** @brief numeric + agg_token. */
CREATE OR REPLACE FUNCTION numeric_plus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a + provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric - agg_token. */
CREATE OR REPLACE FUNCTION numeric_minus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric * agg_token. */
CREATE OR REPLACE FUNCTION numeric_times_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric / agg_token. */
CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- Operator declarations -----------------------------------------------
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);

CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);

CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);

/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

/**
 * @brief Placeholder comparison of agg_token with numeric
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and numeric values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_numeric(a agg_token, b numeric)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-numeric not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of numeric with agg_token
 *
 * Symmetric to agg_token_comp_numeric; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION numeric_comp_agg_token(a numeric, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison numeric-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >,
  NEGATOR    = >=
);
/** @brief SQL operator numeric < agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >,
  NEGATOR    = >=
);

/** @brief SQL operator agg_token <= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >=,
  NEGATOR    = >
);
/** @brief SQL operator numeric <= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >=,
  NEGATOR    = >
);

/** @brief SQL operator agg_token = numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator numeric = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator numeric <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @brief SQL operator agg_token >= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <=,
  NEGATOR    = <
);
/** @brief SQL operator numeric >= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <=,
  NEGATOR    = <
);

/** @brief SQL operator agg_token > numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <,
  NEGATOR    = <=
);
/** @brief SQL operator numeric > agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Placeholder comparison of two agg_token values (the diagonal)
 *
 * Never actually called; lets the parser accept agg_token \<op\> agg_token
 * (e.g. sum(x) > sum(y) on materialised tokens), which the ProvSQL
 * rewriter lowers to a gate_cmp at plan time.  Declaring this diagonal
 * also disambiguates `s = s2` (previously "operator is not unique"
 * because both agg_token -> uuid and agg_token -> numeric casts applied).
 */
CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR < (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >, NEGATOR = >=
);
/** @brief SQL operator agg_token <= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >=, NEGATOR = >
);
/** @brief SQL operator agg_token > agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR > (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <, NEGATOR = <=
);
/** @brief SQL operator agg_token >= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR >= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <=, NEGATOR = <
);
/** @brief SQL operator agg_token = agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR = (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = =, NEGATOR = <>
);
/** @brief SQL operator agg_token <> agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <> (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <>, NEGATOR = =
);

/**
 * @brief Placeholder comparison of agg_token with text
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and text values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of text with agg_token
 *
 * Symmetric to agg_token_comp_text; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token = text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator text = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator text <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt>: a thin wrapper around a
 *  provenance gate UUID, used to expose continuous probabilistic
 *  c-tables in SQL.  The UUID indexes either a <tt>gate_rv</tt>
 *  (an actual distribution) or a <tt>gate_value</tt> (a
 *  zero-variance constant produced by <tt>provsql.as_random</tt>).
 *  Binary-coercible with <tt>uuid</tt> (same 16-byte layout), so an
 *  <tt>rv</tt>-typed expression flows directly into any function
 *  expecting a uuid at zero runtime cost.
 *
 *  Constructors live in this group: <tt>provsql.normal(μ, σ)</tt>,
 *  <tt>provsql.uniform(a, b)</tt>, <tt>provsql.exponential(λ)</tt>,
 *  <tt>provsql.erlang(k, λ)</tt>, and <tt>provsql.as_random(c)</tt>.
 *  Operator overloads
 *  (<tt>+ - * /</tt> and the six comparators) are defined further
 *  below, alongside direct <tt>rv_cmp_*</tt> UUID constructors for
 *  callers that want a <tt>gate_cmp</tt> token without going through
 *  the planner hook.
 *  @{
 */

CREATE TYPE random_variable;

/** @brief Input function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Output function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE random_variable (
  internallength = 16,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = char
);

/** @brief Build a random_variable from a UUID (internal). */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Binary-coercible cast random_variable -> uuid.
 *  A random_variable is byte-for-byte a pg_uuid_t (alignment char,
 *  length 16), so WITHOUT FUNCTION lets PostgreSQL reinterpret the
 *  bytes at zero runtime cost.  The cast is ASSIGNMENT (not IMPLICIT):
 *  an implicit cross-domain cast would silently reroute a comparison
 *  such as `v < w` to `uuid < uuid` (raw byte comparison) whenever
 *  `provsql` is not in search_path, since operators are resolved
 *  through search_path but casts are not.  Demoting to ASSIGNMENT
 *  turns that silent wrong result into a clean parse error.  Passing a
 *  random_variable to a uuid-taking function now needs an explicit
 *  `v::uuid` (function resolution never applies assignment casts). */
CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;

/**
 * @brief Internal: true iff @p x is a finite (non-NaN, non-±∞) float8.
 *
 * PostgreSQL's <tt>isnan</tt> is defined for <tt>numeric</tt> only,
 * not for <tt>double precision</tt>; we use the inequality form,
 * which works because PG defines <tt>NaN = NaN</tt> as <tt>TRUE</tt>
 * for floats (so <tt>NaN <> 'NaN'::float8</tt> is <tt>FALSE</tt>).
 */
CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_token_minus(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token * agg_token (gate_arith TIMES). */
CREATE OR REPLACE FUNCTION agg_token_times(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token / agg_token (gate_arith DIV). */
CREATE OR REPLACE FUNCTION agg_token_div(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief Unary -agg_token (gate_arith NEG). */
CREATE OR REPLACE FUNCTION agg_token_neg(a agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(4, ARRAY[(a)::uuid],
     - provsql.agg_token_value(a)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- agg_token \<op\> numeric ----------------------------------------------
/** @brief agg_token + numeric (gate_arith PLUS, constant lifted to a value gate). */
CREATE OR REPLACE FUNCTION agg_token_plus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) + b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token - numeric. */
CREATE OR REPLACE FUNCTION agg_token_minus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) - b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token * numeric. */
CREATE OR REPLACE FUNCTION agg_token_times_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) * b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token / numeric. */
CREATE OR REPLACE FUNCTION agg_token_div_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) / b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- numeric \<op\> agg_token ----------------------------------------------
/** @brief numeric + agg_token. */
CREATE OR REPLACE FUNCTION numeric_plus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a + provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric - agg_token. */
CREATE OR REPLACE FUNCTION numeric_minus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric * agg_token. */
CREATE OR REPLACE FUNCTION numeric_times_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric / agg_token. */
CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- Operator declarations -----------------------------------------------
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);

CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);

CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);

/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

/**
 * @brief Placeholder comparison of agg_token with numeric
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and numeric values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_numeric(a agg_token, b numeric)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-numeric not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of numeric with agg_token
 *
 * Symmetric to agg_token_comp_numeric; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION numeric_comp_agg_token(a numeric, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison numeric-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >,
  NEGATOR    = >=
);
/** @brief SQL operator numeric < agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >,
  NEGATOR    = >=
);

/** @brief SQL operator agg_token <= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >=,
  NEGATOR    = >
);
/** @brief SQL operator numeric <= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >=,
  NEGATOR    = >
);

/** @brief SQL operator agg_token = numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator numeric = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator numeric <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @brief SQL operator agg_token >= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <=,
  NEGATOR    = <
);
/** @brief SQL operator numeric >= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <=,
  NEGATOR    = <
);

/** @brief SQL operator agg_token > numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <,
  NEGATOR    = <=
);
/** @brief SQL operator numeric > agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Placeholder comparison of two agg_token values (the diagonal)
 *
 * Never actually called; lets the parser accept agg_token \<op\> agg_token
 * (e.g. sum(x) > sum(y) on materialised tokens), which the ProvSQL
 * rewriter lowers to a gate_cmp at plan time.  Declaring this diagonal
 * also disambiguates `s = s2` (previously "operator is not unique"
 * because both agg_token -> uuid and agg_token -> numeric casts applied).
 */
CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR < (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >, NEGATOR = >=
);
/** @brief SQL operator agg_token <= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >=, NEGATOR = >
);
/** @brief SQL operator agg_token > agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR > (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <, NEGATOR = <=
);
/** @brief SQL operator agg_token >= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR >= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <=, NEGATOR = <
);
/** @brief SQL operator agg_token = agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR = (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = =, NEGATOR = <>
);
/** @brief SQL operator agg_token <> agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <> (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <>, NEGATOR = =
);

/**
 * @brief Placeholder comparison of agg_token with text
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and text values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of text with agg_token
 *
 * Symmetric to agg_token_comp_text; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token = text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator text = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator text <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt>: a thin wrapper around a
 *  provenance gate UUID, used to expose continuous probabilistic
 *  c-tables in SQL.  The UUID indexes either a <tt>gate_rv</tt>
 *  (an actual distribution) or a <tt>gate_value</tt> (a
 *  zero-variance constant produced by <tt>provsql.as_random</tt>).
 *  Binary-coercible with <tt>uuid</tt> (same 16-byte layout), so an
 *  <tt>rv</tt>-typed expression flows directly into any function
 *  expecting a uuid at zero runtime cost.
 *
 *  Constructors live in this group: <tt>provsql.normal(μ, σ)</tt>,
 *  <tt>provsql.uniform(a, b)</tt>, <tt>provsql.exponential(λ)</tt>,
 *  <tt>provsql.erlang(k, λ)</tt>, and <tt>provsql.as_random(c)</tt>.
 *  Operator overloads
 *  (<tt>+ - * /</tt> and the six comparators) are defined further
 *  below, alongside direct <tt>rv_cmp_*</tt> UUID constructors for
 *  callers that want a <tt>gate_cmp</tt> token without going through
 *  the planner hook.
 *  @{
 */

CREATE TYPE random_variable;

/** @brief Input function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Output function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE random_variable (
  internallength = 16,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = char
);

/** @brief Build a random_variable from a UUID (internal). */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Binary-coercible cast random_variable -> uuid.
 *  A random_variable is byte-for-byte a pg_uuid_t (alignment char,
 *  length 16), so WITHOUT FUNCTION lets PostgreSQL reinterpret the
 *  bytes at zero runtime cost.  The cast is ASSIGNMENT (not IMPLICIT):
 *  an implicit cross-domain cast would silently reroute a comparison
 *  such as `v < w` to `uuid < uuid` (raw byte comparison) whenever
 *  `provsql` is not in search_path, since operators are resolved
 *  through search_path but casts are not.  Demoting to ASSIGNMENT
 *  turns that silent wrong result into a clean parse error.  Passing a
 *  random_variable to a uuid-taking function now needs an explicit
 *  `v::uuid` (function resolution never applies assignment casts). */
CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;

/**
 * @brief Internal: true iff @p x is a finite (non-NaN, non-±∞) float8.
 *
 * PostgreSQL's <tt>isnan</tt> is defined for <tt>numeric</tt> only,
 * not for <tt>double precision</tt>; we use the inequality form,
 * which works because PG defines <tt>NaN = NaN</tt> as <tt>TRUE</tt>
 * for floats (so <tt>NaN <> 'NaN'::float8</tt> is <tt>FALSE</tt>).
 */
CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_token_times(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token / agg_token (gate_arith DIV). */
CREATE OR REPLACE FUNCTION agg_token_div(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief Unary -agg_token (gate_arith NEG). */
CREATE OR REPLACE FUNCTION agg_token_neg(a agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(4, ARRAY[(a)::uuid],
     - provsql.agg_token_value(a)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- agg_token \<op\> numeric ----------------------------------------------
/** @brief agg_token + numeric (gate_arith PLUS, constant lifted to a value gate). */
CREATE OR REPLACE FUNCTION agg_token_plus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) + b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token - numeric. */
CREATE OR REPLACE FUNCTION agg_token_minus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) - b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token * numeric. */
CREATE OR REPLACE FUNCTION agg_token_times_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) * b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token / numeric. */
CREATE OR REPLACE FUNCTION agg_token_div_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) / b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- numeric \<op\> agg_token ----------------------------------------------
/** @brief numeric + agg_token. */
CREATE OR REPLACE FUNCTION numeric_plus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a + provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric - agg_token. */
CREATE OR REPLACE FUNCTION numeric_minus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric * agg_token. */
CREATE OR REPLACE FUNCTION numeric_times_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric / agg_token. */
CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- Operator declarations -----------------------------------------------
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);

CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);

CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);

/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

/**
 * @brief Placeholder comparison of agg_token with numeric
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and numeric values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_numeric(a agg_token, b numeric)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-numeric not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of numeric with agg_token
 *
 * Symmetric to agg_token_comp_numeric; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION numeric_comp_agg_token(a numeric, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison numeric-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >,
  NEGATOR    = >=
);
/** @brief SQL operator numeric < agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >,
  NEGATOR    = >=
);

/** @brief SQL operator agg_token <= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >=,
  NEGATOR    = >
);
/** @brief SQL operator numeric <= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >=,
  NEGATOR    = >
);

/** @brief SQL operator agg_token = numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator numeric = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator numeric <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @brief SQL operator agg_token >= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <=,
  NEGATOR    = <
);
/** @brief SQL operator numeric >= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <=,
  NEGATOR    = <
);

/** @brief SQL operator agg_token > numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <,
  NEGATOR    = <=
);
/** @brief SQL operator numeric > agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Placeholder comparison of two agg_token values (the diagonal)
 *
 * Never actually called; lets the parser accept agg_token \<op\> agg_token
 * (e.g. sum(x) > sum(y) on materialised tokens), which the ProvSQL
 * rewriter lowers to a gate_cmp at plan time.  Declaring this diagonal
 * also disambiguates `s = s2` (previously "operator is not unique"
 * because both agg_token -> uuid and agg_token -> numeric casts applied).
 */
CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR < (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >, NEGATOR = >=
);
/** @brief SQL operator agg_token <= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >=, NEGATOR = >
);
/** @brief SQL operator agg_token > agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR > (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <, NEGATOR = <=
);
/** @brief SQL operator agg_token >= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR >= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <=, NEGATOR = <
);
/** @brief SQL operator agg_token = agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR = (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = =, NEGATOR = <>
);
/** @brief SQL operator agg_token <> agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <> (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <>, NEGATOR = =
);

/**
 * @brief Placeholder comparison of agg_token with text
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and text values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of text with agg_token
 *
 * Symmetric to agg_token_comp_text; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token = text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator text = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator text <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt>: a thin wrapper around a
 *  provenance gate UUID, used to expose continuous probabilistic
 *  c-tables in SQL.  The UUID indexes either a <tt>gate_rv</tt>
 *  (an actual distribution) or a <tt>gate_value</tt> (a
 *  zero-variance constant produced by <tt>provsql.as_random</tt>).
 *  Binary-coercible with <tt>uuid</tt> (same 16-byte layout), so an
 *  <tt>rv</tt>-typed expression flows directly into any function
 *  expecting a uuid at zero runtime cost.
 *
 *  Constructors live in this group: <tt>provsql.normal(μ, σ)</tt>,
 *  <tt>provsql.uniform(a, b)</tt>, <tt>provsql.exponential(λ)</tt>,
 *  <tt>provsql.erlang(k, λ)</tt>, and <tt>provsql.as_random(c)</tt>.
 *  Operator overloads
 *  (<tt>+ - * /</tt> and the six comparators) are defined further
 *  below, alongside direct <tt>rv_cmp_*</tt> UUID constructors for
 *  callers that want a <tt>gate_cmp</tt> token without going through
 *  the planner hook.
 *  @{
 */

CREATE TYPE random_variable;

/** @brief Input function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Output function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE random_variable (
  internallength = 16,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = char
);

/** @brief Build a random_variable from a UUID (internal). */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Binary-coercible cast random_variable -> uuid.
 *  A random_variable is byte-for-byte a pg_uuid_t (alignment char,
 *  length 16), so WITHOUT FUNCTION lets PostgreSQL reinterpret the
 *  bytes at zero runtime cost.  The cast is ASSIGNMENT (not IMPLICIT):
 *  an implicit cross-domain cast would silently reroute a comparison
 *  such as `v < w` to `uuid < uuid` (raw byte comparison) whenever
 *  `provsql` is not in search_path, since operators are resolved
 *  through search_path but casts are not.  Demoting to ASSIGNMENT
 *  turns that silent wrong result into a clean parse error.  Passing a
 *  random_variable to a uuid-taking function now needs an explicit
 *  `v::uuid` (function resolution never applies assignment casts). */
CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;

/**
 * @brief Internal: true iff @p x is a finite (non-NaN, non-±∞) float8.
 *
 * PostgreSQL's <tt>isnan</tt> is defined for <tt>numeric</tt> only,
 * not for <tt>double precision</tt>; we use the inequality form,
 * which works because PG defines <tt>NaN = NaN</tt> as <tt>TRUE</tt>
 * for floats (so <tt>NaN <> 'NaN'::float8</tt> is <tt>FALSE</tt>).
 */
CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_token_div(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief Unary -agg_token (gate_arith NEG). */
CREATE OR REPLACE FUNCTION agg_token_neg(a agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(4, ARRAY[(a)::uuid],
     - provsql.agg_token_value(a)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- agg_token \<op\> numeric ----------------------------------------------
/** @brief agg_token + numeric (gate_arith PLUS, constant lifted to a value gate). */
CREATE OR REPLACE FUNCTION agg_token_plus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) + b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token - numeric. */
CREATE OR REPLACE FUNCTION agg_token_minus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) - b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token * numeric. */
CREATE OR REPLACE FUNCTION agg_token_times_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) * b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token / numeric. */
CREATE OR REPLACE FUNCTION agg_token_div_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) / b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- numeric \<op\> agg_token ----------------------------------------------
/** @brief numeric + agg_token. */
CREATE OR REPLACE FUNCTION numeric_plus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a + provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric - agg_token. */
CREATE OR REPLACE FUNCTION numeric_minus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric * agg_token. */
CREATE OR REPLACE FUNCTION numeric_times_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric / agg_token. */
CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- Operator declarations -----------------------------------------------
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);

CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);

CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);

/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

/**
 * @brief Placeholder comparison of agg_token with numeric
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and numeric values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_numeric(a agg_token, b numeric)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-numeric not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of numeric with agg_token
 *
 * Symmetric to agg_token_comp_numeric; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION numeric_comp_agg_token(a numeric, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison numeric-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >,
  NEGATOR    = >=
);
/** @brief SQL operator numeric < agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >,
  NEGATOR    = >=
);

/** @brief SQL operator agg_token <= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >=,
  NEGATOR    = >
);
/** @brief SQL operator numeric <= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >=,
  NEGATOR    = >
);

/** @brief SQL operator agg_token = numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator numeric = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator numeric <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @brief SQL operator agg_token >= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <=,
  NEGATOR    = <
);
/** @brief SQL operator numeric >= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <=,
  NEGATOR    = <
);

/** @brief SQL operator agg_token > numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <,
  NEGATOR    = <=
);
/** @brief SQL operator numeric > agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Placeholder comparison of two agg_token values (the diagonal)
 *
 * Never actually called; lets the parser accept agg_token \<op\> agg_token
 * (e.g. sum(x) > sum(y) on materialised tokens), which the ProvSQL
 * rewriter lowers to a gate_cmp at plan time.  Declaring this diagonal
 * also disambiguates `s = s2` (previously "operator is not unique"
 * because both agg_token -> uuid and agg_token -> numeric casts applied).
 */
CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR < (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >, NEGATOR = >=
);
/** @brief SQL operator agg_token <= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >=, NEGATOR = >
);
/** @brief SQL operator agg_token > agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR > (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <, NEGATOR = <=
);
/** @brief SQL operator agg_token >= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR >= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <=, NEGATOR = <
);
/** @brief SQL operator agg_token = agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR = (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = =, NEGATOR = <>
);
/** @brief SQL operator agg_token <> agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <> (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <>, NEGATOR = =
);

/**
 * @brief Placeholder comparison of agg_token with text
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and text values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of text with agg_token
 *
 * Symmetric to agg_token_comp_text; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token = text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator text = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator text <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt>: a thin wrapper around a
 *  provenance gate UUID, used to expose continuous probabilistic
 *  c-tables in SQL.  The UUID indexes either a <tt>gate_rv</tt>
 *  (an actual distribution) or a <tt>gate_value</tt> (a
 *  zero-variance constant produced by <tt>provsql.as_random</tt>).
 *  Binary-coercible with <tt>uuid</tt> (same 16-byte layout), so an
 *  <tt>rv</tt>-typed expression flows directly into any function
 *  expecting a uuid at zero runtime cost.
 *
 *  Constructors live in this group: <tt>provsql.normal(μ, σ)</tt>,
 *  <tt>provsql.uniform(a, b)</tt>, <tt>provsql.exponential(λ)</tt>,
 *  <tt>provsql.erlang(k, λ)</tt>, and <tt>provsql.as_random(c)</tt>.
 *  Operator overloads
 *  (<tt>+ - * /</tt> and the six comparators) are defined further
 *  below, alongside direct <tt>rv_cmp_*</tt> UUID constructors for
 *  callers that want a <tt>gate_cmp</tt> token without going through
 *  the planner hook.
 *  @{
 */

CREATE TYPE random_variable;

/** @brief Input function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Output function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE random_variable (
  internallength = 16,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = char
);

/** @brief Build a random_variable from a UUID (internal). */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Binary-coercible cast random_variable -> uuid.
 *  A random_variable is byte-for-byte a pg_uuid_t (alignment char,
 *  length 16), so WITHOUT FUNCTION lets PostgreSQL reinterpret the
 *  bytes at zero runtime cost.  The cast is ASSIGNMENT (not IMPLICIT):
 *  an implicit cross-domain cast would silently reroute a comparison
 *  such as `v < w` to `uuid < uuid` (raw byte comparison) whenever
 *  `provsql` is not in search_path, since operators are resolved
 *  through search_path but casts are not.  Demoting to ASSIGNMENT
 *  turns that silent wrong result into a clean parse error.  Passing a
 *  random_variable to a uuid-taking function now needs an explicit
 *  `v::uuid` (function resolution never applies assignment casts). */
CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;

/**
 * @brief Internal: true iff @p x is a finite (non-NaN, non-±∞) float8.
 *
 * PostgreSQL's <tt>isnan</tt> is defined for <tt>numeric</tt> only,
 * not for <tt>double precision</tt>; we use the inequality form,
 * which works because PG defines <tt>NaN = NaN</tt> as <tt>TRUE</tt>
 * for floats (so <tt>NaN <> 'NaN'::float8</tt> is <tt>FALSE</tt>).
 */
CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_token_plus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) + b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token - numeric. */
CREATE OR REPLACE FUNCTION agg_token_minus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) - b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token * numeric. */
CREATE OR REPLACE FUNCTION agg_token_times_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) * b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token / numeric. */
CREATE OR REPLACE FUNCTION agg_token_div_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) / b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- numeric \<op\> agg_token ----------------------------------------------
/** @brief numeric + agg_token. */
CREATE OR REPLACE FUNCTION numeric_plus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a + provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric - agg_token. */
CREATE OR REPLACE FUNCTION numeric_minus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric * agg_token. */
CREATE OR REPLACE FUNCTION numeric_times_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric / agg_token. */
CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- Operator declarations -----------------------------------------------
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);

CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);

CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);

/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

/**
 * @brief Placeholder comparison of agg_token with numeric
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and numeric values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_numeric(a agg_token, b numeric)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-numeric not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of numeric with agg_token
 *
 * Symmetric to agg_token_comp_numeric; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION numeric_comp_agg_token(a numeric, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison numeric-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >,
  NEGATOR    = >=
);
/** @brief SQL operator numeric < agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >,
  NEGATOR    = >=
);

/** @brief SQL operator agg_token <= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >=,
  NEGATOR    = >
);
/** @brief SQL operator numeric <= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >=,
  NEGATOR    = >
);

/** @brief SQL operator agg_token = numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator numeric = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator numeric <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @brief SQL operator agg_token >= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <=,
  NEGATOR    = <
);
/** @brief SQL operator numeric >= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <=,
  NEGATOR    = <
);

/** @brief SQL operator agg_token > numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <,
  NEGATOR    = <=
);
/** @brief SQL operator numeric > agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Placeholder comparison of two agg_token values (the diagonal)
 *
 * Never actually called; lets the parser accept agg_token \<op\> agg_token
 * (e.g. sum(x) > sum(y) on materialised tokens), which the ProvSQL
 * rewriter lowers to a gate_cmp at plan time.  Declaring this diagonal
 * also disambiguates `s = s2` (previously "operator is not unique"
 * because both agg_token -> uuid and agg_token -> numeric casts applied).
 */
CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR < (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >, NEGATOR = >=
);
/** @brief SQL operator agg_token <= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >=, NEGATOR = >
);
/** @brief SQL operator agg_token > agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR > (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <, NEGATOR = <=
);
/** @brief SQL operator agg_token >= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR >= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <=, NEGATOR = <
);
/** @brief SQL operator agg_token = agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR = (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = =, NEGATOR = <>
);
/** @brief SQL operator agg_token <> agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <> (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <>, NEGATOR = =
);

/**
 * @brief Placeholder comparison of agg_token with text
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and text values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of text with agg_token
 *
 * Symmetric to agg_token_comp_text; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token = text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator text = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator text <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt>: a thin wrapper around a
 *  provenance gate UUID, used to expose continuous probabilistic
 *  c-tables in SQL.  The UUID indexes either a <tt>gate_rv</tt>
 *  (an actual distribution) or a <tt>gate_value</tt> (a
 *  zero-variance constant produced by <tt>provsql.as_random</tt>).
 *  Binary-coercible with <tt>uuid</tt> (same 16-byte layout), so an
 *  <tt>rv</tt>-typed expression flows directly into any function
 *  expecting a uuid at zero runtime cost.
 *
 *  Constructors live in this group: <tt>provsql.normal(μ, σ)</tt>,
 *  <tt>provsql.uniform(a, b)</tt>, <tt>provsql.exponential(λ)</tt>,
 *  <tt>provsql.erlang(k, λ)</tt>, and <tt>provsql.as_random(c)</tt>.
 *  Operator overloads
 *  (<tt>+ - * /</tt> and the six comparators) are defined further
 *  below, alongside direct <tt>rv_cmp_*</tt> UUID constructors for
 *  callers that want a <tt>gate_cmp</tt> token without going through
 *  the planner hook.
 *  @{
 */

CREATE TYPE random_variable;

/** @brief Input function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Output function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE random_variable (
  internallength = 16,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = char
);

/** @brief Build a random_variable from a UUID (internal). */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Binary-coercible cast random_variable -> uuid.
 *  A random_variable is byte-for-byte a pg_uuid_t (alignment char,
 *  length 16), so WITHOUT FUNCTION lets PostgreSQL reinterpret the
 *  bytes at zero runtime cost.  The cast is ASSIGNMENT (not IMPLICIT):
 *  an implicit cross-domain cast would silently reroute a comparison
 *  such as `v < w` to `uuid < uuid` (raw byte comparison) whenever
 *  `provsql` is not in search_path, since operators are resolved
 *  through search_path but casts are not.  Demoting to ASSIGNMENT
 *  turns that silent wrong result into a clean parse error.  Passing a
 *  random_variable to a uuid-taking function now needs an explicit
 *  `v::uuid` (function resolution never applies assignment casts). */
CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;

/**
 * @brief Internal: true iff @p x is a finite (non-NaN, non-±∞) float8.
 *
 * PostgreSQL's <tt>isnan</tt> is defined for <tt>numeric</tt> only,
 * not for <tt>double precision</tt>; we use the inequality form,
 * which works because PG defines <tt>NaN = NaN</tt> as <tt>TRUE</tt>
 * for floats (so <tt>NaN <> 'NaN'::float8</tt> is <tt>FALSE</tt>).
 */
CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_token_minus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) - b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token * numeric. */
CREATE OR REPLACE FUNCTION agg_token_times_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) * b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token / numeric. */
CREATE OR REPLACE FUNCTION agg_token_div_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) / b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- numeric \<op\> agg_token ----------------------------------------------
/** @brief numeric + agg_token. */
CREATE OR REPLACE FUNCTION numeric_plus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a + provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric - agg_token. */
CREATE OR REPLACE FUNCTION numeric_minus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric * agg_token. */
CREATE OR REPLACE FUNCTION numeric_times_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric / agg_token. */
CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- Operator declarations -----------------------------------------------
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);

CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);

CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);

/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

/**
 * @brief Placeholder comparison of agg_token with numeric
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and numeric values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_numeric(a agg_token, b numeric)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-numeric not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of numeric with agg_token
 *
 * Symmetric to agg_token_comp_numeric; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION numeric_comp_agg_token(a numeric, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison numeric-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >,
  NEGATOR    = >=
);
/** @brief SQL operator numeric < agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >,
  NEGATOR    = >=
);

/** @brief SQL operator agg_token <= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >=,
  NEGATOR    = >
);
/** @brief SQL operator numeric <= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >=,
  NEGATOR    = >
);

/** @brief SQL operator agg_token = numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator numeric = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator numeric <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @brief SQL operator agg_token >= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <=,
  NEGATOR    = <
);
/** @brief SQL operator numeric >= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <=,
  NEGATOR    = <
);

/** @brief SQL operator agg_token > numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <,
  NEGATOR    = <=
);
/** @brief SQL operator numeric > agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Placeholder comparison of two agg_token values (the diagonal)
 *
 * Never actually called; lets the parser accept agg_token \<op\> agg_token
 * (e.g. sum(x) > sum(y) on materialised tokens), which the ProvSQL
 * rewriter lowers to a gate_cmp at plan time.  Declaring this diagonal
 * also disambiguates `s = s2` (previously "operator is not unique"
 * because both agg_token -> uuid and agg_token -> numeric casts applied).
 */
CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR < (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >, NEGATOR = >=
);
/** @brief SQL operator agg_token <= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >=, NEGATOR = >
);
/** @brief SQL operator agg_token > agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR > (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <, NEGATOR = <=
);
/** @brief SQL operator agg_token >= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR >= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <=, NEGATOR = <
);
/** @brief SQL operator agg_token = agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR = (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = =, NEGATOR = <>
);
/** @brief SQL operator agg_token <> agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <> (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <>, NEGATOR = =
);

/**
 * @brief Placeholder comparison of agg_token with text
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and text values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of text with agg_token
 *
 * Symmetric to agg_token_comp_text; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token = text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator text = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator text <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt>: a thin wrapper around a
 *  provenance gate UUID, used to expose continuous probabilistic
 *  c-tables in SQL.  The UUID indexes either a <tt>gate_rv</tt>
 *  (an actual distribution) or a <tt>gate_value</tt> (a
 *  zero-variance constant produced by <tt>provsql.as_random</tt>).
 *  Binary-coercible with <tt>uuid</tt> (same 16-byte layout), so an
 *  <tt>rv</tt>-typed expression flows directly into any function
 *  expecting a uuid at zero runtime cost.
 *
 *  Constructors live in this group: <tt>provsql.normal(μ, σ)</tt>,
 *  <tt>provsql.uniform(a, b)</tt>, <tt>provsql.exponential(λ)</tt>,
 *  <tt>provsql.erlang(k, λ)</tt>, and <tt>provsql.as_random(c)</tt>.
 *  Operator overloads
 *  (<tt>+ - * /</tt> and the six comparators) are defined further
 *  below, alongside direct <tt>rv_cmp_*</tt> UUID constructors for
 *  callers that want a <tt>gate_cmp</tt> token without going through
 *  the planner hook.
 *  @{
 */

CREATE TYPE random_variable;

/** @brief Input function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Output function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE random_variable (
  internallength = 16,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = char
);

/** @brief Build a random_variable from a UUID (internal). */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Binary-coercible cast random_variable -> uuid.
 *  A random_variable is byte-for-byte a pg_uuid_t (alignment char,
 *  length 16), so WITHOUT FUNCTION lets PostgreSQL reinterpret the
 *  bytes at zero runtime cost.  The cast is ASSIGNMENT (not IMPLICIT):
 *  an implicit cross-domain cast would silently reroute a comparison
 *  such as `v < w` to `uuid < uuid` (raw byte comparison) whenever
 *  `provsql` is not in search_path, since operators are resolved
 *  through search_path but casts are not.  Demoting to ASSIGNMENT
 *  turns that silent wrong result into a clean parse error.  Passing a
 *  random_variable to a uuid-taking function now needs an explicit
 *  `v::uuid` (function resolution never applies assignment casts). */
CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;

/**
 * @brief Internal: true iff @p x is a finite (non-NaN, non-±∞) float8.
 *
 * PostgreSQL's <tt>isnan</tt> is defined for <tt>numeric</tt> only,
 * not for <tt>double precision</tt>; we use the inequality form,
 * which works because PG defines <tt>NaN = NaN</tt> as <tt>TRUE</tt>
 * for floats (so <tt>NaN <> 'NaN'::float8</tt> is <tt>FALSE</tt>).
 */
CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_token_times_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) * b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token / numeric. */
CREATE OR REPLACE FUNCTION agg_token_div_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) / b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- numeric \<op\> agg_token ----------------------------------------------
/** @brief numeric + agg_token. */
CREATE OR REPLACE FUNCTION numeric_plus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a + provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric - agg_token. */
CREATE OR REPLACE FUNCTION numeric_minus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric * agg_token. */
CREATE OR REPLACE FUNCTION numeric_times_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric / agg_token. */
CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- Operator declarations -----------------------------------------------
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);

CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);

CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);

/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

/**
 * @brief Placeholder comparison of agg_token with numeric
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and numeric values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_numeric(a agg_token, b numeric)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-numeric not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of numeric with agg_token
 *
 * Symmetric to agg_token_comp_numeric; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION numeric_comp_agg_token(a numeric, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison numeric-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >,
  NEGATOR    = >=
);
/** @brief SQL operator numeric < agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >,
  NEGATOR    = >=
);

/** @brief SQL operator agg_token <= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >=,
  NEGATOR    = >
);
/** @brief SQL operator numeric <= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >=,
  NEGATOR    = >
);

/** @brief SQL operator agg_token = numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator numeric = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator numeric <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @brief SQL operator agg_token >= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <=,
  NEGATOR    = <
);
/** @brief SQL operator numeric >= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <=,
  NEGATOR    = <
);

/** @brief SQL operator agg_token > numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <,
  NEGATOR    = <=
);
/** @brief SQL operator numeric > agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Placeholder comparison of two agg_token values (the diagonal)
 *
 * Never actually called; lets the parser accept agg_token \<op\> agg_token
 * (e.g. sum(x) > sum(y) on materialised tokens), which the ProvSQL
 * rewriter lowers to a gate_cmp at plan time.  Declaring this diagonal
 * also disambiguates `s = s2` (previously "operator is not unique"
 * because both agg_token -> uuid and agg_token -> numeric casts applied).
 */
CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR < (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >, NEGATOR = >=
);
/** @brief SQL operator agg_token <= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >=, NEGATOR = >
);
/** @brief SQL operator agg_token > agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR > (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <, NEGATOR = <=
);
/** @brief SQL operator agg_token >= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR >= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <=, NEGATOR = <
);
/** @brief SQL operator agg_token = agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR = (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = =, NEGATOR = <>
);
/** @brief SQL operator agg_token <> agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <> (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <>, NEGATOR = =
);

/**
 * @brief Placeholder comparison of agg_token with text
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and text values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of text with agg_token
 *
 * Symmetric to agg_token_comp_text; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token = text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator text = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator text <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt>: a thin wrapper around a
 *  provenance gate UUID, used to expose continuous probabilistic
 *  c-tables in SQL.  The UUID indexes either a <tt>gate_rv</tt>
 *  (an actual distribution) or a <tt>gate_value</tt> (a
 *  zero-variance constant produced by <tt>provsql.as_random</tt>).
 *  Binary-coercible with <tt>uuid</tt> (same 16-byte layout), so an
 *  <tt>rv</tt>-typed expression flows directly into any function
 *  expecting a uuid at zero runtime cost.
 *
 *  Constructors live in this group: <tt>provsql.normal(μ, σ)</tt>,
 *  <tt>provsql.uniform(a, b)</tt>, <tt>provsql.exponential(λ)</tt>,
 *  <tt>provsql.erlang(k, λ)</tt>, and <tt>provsql.as_random(c)</tt>.
 *  Operator overloads
 *  (<tt>+ - * /</tt> and the six comparators) are defined further
 *  below, alongside direct <tt>rv_cmp_*</tt> UUID constructors for
 *  callers that want a <tt>gate_cmp</tt> token without going through
 *  the planner hook.
 *  @{
 */

CREATE TYPE random_variable;

/** @brief Input function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Output function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE random_variable (
  internallength = 16,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = char
);

/** @brief Build a random_variable from a UUID (internal). */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Binary-coercible cast random_variable -> uuid.
 *  A random_variable is byte-for-byte a pg_uuid_t (alignment char,
 *  length 16), so WITHOUT FUNCTION lets PostgreSQL reinterpret the
 *  bytes at zero runtime cost.  The cast is ASSIGNMENT (not IMPLICIT):
 *  an implicit cross-domain cast would silently reroute a comparison
 *  such as `v < w` to `uuid < uuid` (raw byte comparison) whenever
 *  `provsql` is not in search_path, since operators are resolved
 *  through search_path but casts are not.  Demoting to ASSIGNMENT
 *  turns that silent wrong result into a clean parse error.  Passing a
 *  random_variable to a uuid-taking function now needs an explicit
 *  `v::uuid` (function resolution never applies assignment casts). */
CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;

/**
 * @brief Internal: true iff @p x is a finite (non-NaN, non-±∞) float8.
 *
 * PostgreSQL's <tt>isnan</tt> is defined for <tt>numeric</tt> only,
 * not for <tt>double precision</tt>; we use the inequality form,
 * which works because PG defines <tt>NaN = NaN</tt> as <tt>TRUE</tt>
 * for floats (so <tt>NaN <> 'NaN'::float8</tt> is <tt>FALSE</tt>).
 */
CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_token_div_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) / b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- numeric \<op\> agg_token ----------------------------------------------
/** @brief numeric + agg_token. */
CREATE OR REPLACE FUNCTION numeric_plus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a + provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric - agg_token. */
CREATE OR REPLACE FUNCTION numeric_minus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric * agg_token. */
CREATE OR REPLACE FUNCTION numeric_times_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric / agg_token. */
CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- Operator declarations -----------------------------------------------
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);

CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);

CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);

/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

/**
 * @brief Placeholder comparison of agg_token with numeric
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and numeric values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_numeric(a agg_token, b numeric)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-numeric not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of numeric with agg_token
 *
 * Symmetric to agg_token_comp_numeric; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION numeric_comp_agg_token(a numeric, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison numeric-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >,
  NEGATOR    = >=
);
/** @brief SQL operator numeric < agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >,
  NEGATOR    = >=
);

/** @brief SQL operator agg_token <= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >=,
  NEGATOR    = >
);
/** @brief SQL operator numeric <= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >=,
  NEGATOR    = >
);

/** @brief SQL operator agg_token = numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator numeric = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator numeric <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @brief SQL operator agg_token >= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <=,
  NEGATOR    = <
);
/** @brief SQL operator numeric >= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <=,
  NEGATOR    = <
);

/** @brief SQL operator agg_token > numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <,
  NEGATOR    = <=
);
/** @brief SQL operator numeric > agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Placeholder comparison of two agg_token values (the diagonal)
 *
 * Never actually called; lets the parser accept agg_token \<op\> agg_token
 * (e.g. sum(x) > sum(y) on materialised tokens), which the ProvSQL
 * rewriter lowers to a gate_cmp at plan time.  Declaring this diagonal
 * also disambiguates `s = s2` (previously "operator is not unique"
 * because both agg_token -> uuid and agg_token -> numeric casts applied).
 */
CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR < (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >, NEGATOR = >=
);
/** @brief SQL operator agg_token <= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >=, NEGATOR = >
);
/** @brief SQL operator agg_token > agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR > (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <, NEGATOR = <=
);
/** @brief SQL operator agg_token >= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR >= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <=, NEGATOR = <
);
/** @brief SQL operator agg_token = agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR = (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = =, NEGATOR = <>
);
/** @brief SQL operator agg_token <> agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <> (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <>, NEGATOR = =
);

/**
 * @brief Placeholder comparison of agg_token with text
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and text values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of text with agg_token
 *
 * Symmetric to agg_token_comp_text; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token = text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator text = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator text <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt>: a thin wrapper around a
 *  provenance gate UUID, used to expose continuous probabilistic
 *  c-tables in SQL.  The UUID indexes either a <tt>gate_rv</tt>
 *  (an actual distribution) or a <tt>gate_value</tt> (a
 *  zero-variance constant produced by <tt>provsql.as_random</tt>).
 *  Binary-coercible with <tt>uuid</tt> (same 16-byte layout), so an
 *  <tt>rv</tt>-typed expression flows directly into any function
 *  expecting a uuid at zero runtime cost.
 *
 *  Constructors live in this group: <tt>provsql.normal(μ, σ)</tt>,
 *  <tt>provsql.uniform(a, b)</tt>, <tt>provsql.exponential(λ)</tt>,
 *  <tt>provsql.erlang(k, λ)</tt>, and <tt>provsql.as_random(c)</tt>.
 *  Operator overloads
 *  (<tt>+ - * /</tt> and the six comparators) are defined further
 *  below, alongside direct <tt>rv_cmp_*</tt> UUID constructors for
 *  callers that want a <tt>gate_cmp</tt> token without going through
 *  the planner hook.
 *  @{
 */

CREATE TYPE random_variable;

/** @brief Input function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Output function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE random_variable (
  internallength = 16,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = char
);

/** @brief Build a random_variable from a UUID (internal). */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Binary-coercible cast random_variable -> uuid.
 *  A random_variable is byte-for-byte a pg_uuid_t (alignment char,
 *  length 16), so WITHOUT FUNCTION lets PostgreSQL reinterpret the
 *  bytes at zero runtime cost.  The cast is ASSIGNMENT (not IMPLICIT):
 *  an implicit cross-domain cast would silently reroute a comparison
 *  such as `v < w` to `uuid < uuid` (raw byte comparison) whenever
 *  `provsql` is not in search_path, since operators are resolved
 *  through search_path but casts are not.  Demoting to ASSIGNMENT
 *  turns that silent wrong result into a clean parse error.  Passing a
 *  random_variable to a uuid-taking function now needs an explicit
 *  `v::uuid` (function resolution never applies assignment casts). */
CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;

/**
 * @brief Internal: true iff @p x is a finite (non-NaN, non-±∞) float8.
 *
 * PostgreSQL's <tt>isnan</tt> is defined for <tt>numeric</tt> only,
 * not for <tt>double precision</tt>; we use the inequality form,
 * which works because PG defines <tt>NaN = NaN</tt> as <tt>TRUE</tt>
 * for floats (so <tt>NaN <> 'NaN'::float8</tt> is <tt>FALSE</tt>).
 */
CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION numeric_plus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a + provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric - agg_token. */
CREATE OR REPLACE FUNCTION numeric_minus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric * agg_token. */
CREATE OR REPLACE FUNCTION numeric_times_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric / agg_token. */
CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- Operator declarations -----------------------------------------------
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);

CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);

CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);

/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

/**
 * @brief Placeholder comparison of agg_token with numeric
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and numeric values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_numeric(a agg_token, b numeric)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-numeric not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of numeric with agg_token
 *
 * Symmetric to agg_token_comp_numeric; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION numeric_comp_agg_token(a numeric, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison numeric-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >,
  NEGATOR    = >=
);
/** @brief SQL operator numeric < agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >,
  NEGATOR    = >=
);

/** @brief SQL operator agg_token <= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >=,
  NEGATOR    = >
);
/** @brief SQL operator numeric <= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >=,
  NEGATOR    = >
);

/** @brief SQL operator agg_token = numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator numeric = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator numeric <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @brief SQL operator agg_token >= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <=,
  NEGATOR    = <
);
/** @brief SQL operator numeric >= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <=,
  NEGATOR    = <
);

/** @brief SQL operator agg_token > numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <,
  NEGATOR    = <=
);
/** @brief SQL operator numeric > agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Placeholder comparison of two agg_token values (the diagonal)
 *
 * Never actually called; lets the parser accept agg_token \<op\> agg_token
 * (e.g. sum(x) > sum(y) on materialised tokens), which the ProvSQL
 * rewriter lowers to a gate_cmp at plan time.  Declaring this diagonal
 * also disambiguates `s = s2` (previously "operator is not unique"
 * because both agg_token -> uuid and agg_token -> numeric casts applied).
 */
CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR < (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >, NEGATOR = >=
);
/** @brief SQL operator agg_token <= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >=, NEGATOR = >
);
/** @brief SQL operator agg_token > agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR > (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <, NEGATOR = <=
);
/** @brief SQL operator agg_token >= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR >= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <=, NEGATOR = <
);
/** @brief SQL operator agg_token = agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR = (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = =, NEGATOR = <>
);
/** @brief SQL operator agg_token <> agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <> (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <>, NEGATOR = =
);

/**
 * @brief Placeholder comparison of agg_token with text
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and text values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of text with agg_token
 *
 * Symmetric to agg_token_comp_text; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token = text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator text = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator text <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt>: a thin wrapper around a
 *  provenance gate UUID, used to expose continuous probabilistic
 *  c-tables in SQL.  The UUID indexes either a <tt>gate_rv</tt>
 *  (an actual distribution) or a <tt>gate_value</tt> (a
 *  zero-variance constant produced by <tt>provsql.as_random</tt>).
 *  Binary-coercible with <tt>uuid</tt> (same 16-byte layout), so an
 *  <tt>rv</tt>-typed expression flows directly into any function
 *  expecting a uuid at zero runtime cost.
 *
 *  Constructors live in this group: <tt>provsql.normal(μ, σ)</tt>,
 *  <tt>provsql.uniform(a, b)</tt>, <tt>provsql.exponential(λ)</tt>,
 *  <tt>provsql.erlang(k, λ)</tt>, and <tt>provsql.as_random(c)</tt>.
 *  Operator overloads
 *  (<tt>+ - * /</tt> and the six comparators) are defined further
 *  below, alongside direct <tt>rv_cmp_*</tt> UUID constructors for
 *  callers that want a <tt>gate_cmp</tt> token without going through
 *  the planner hook.
 *  @{
 */

CREATE TYPE random_variable;

/** @brief Input function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Output function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE random_variable (
  internallength = 16,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = char
);

/** @brief Build a random_variable from a UUID (internal). */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Binary-coercible cast random_variable -> uuid.
 *  A random_variable is byte-for-byte a pg_uuid_t (alignment char,
 *  length 16), so WITHOUT FUNCTION lets PostgreSQL reinterpret the
 *  bytes at zero runtime cost.  The cast is ASSIGNMENT (not IMPLICIT):
 *  an implicit cross-domain cast would silently reroute a comparison
 *  such as `v < w` to `uuid < uuid` (raw byte comparison) whenever
 *  `provsql` is not in search_path, since operators are resolved
 *  through search_path but casts are not.  Demoting to ASSIGNMENT
 *  turns that silent wrong result into a clean parse error.  Passing a
 *  random_variable to a uuid-taking function now needs an explicit
 *  `v::uuid` (function resolution never applies assignment casts). */
CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;

/**
 * @brief Internal: true iff @p x is a finite (non-NaN, non-±∞) float8.
 *
 * PostgreSQL's <tt>isnan</tt> is defined for <tt>numeric</tt> only,
 * not for <tt>double precision</tt>; we use the inequality form,
 * which works because PG defines <tt>NaN = NaN</tt> as <tt>TRUE</tt>
 * for floats (so <tt>NaN <> 'NaN'::float8</tt> is <tt>FALSE</tt>).
 */
CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION numeric_minus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric * agg_token. */
CREATE OR REPLACE FUNCTION numeric_times_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric / agg_token. */
CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- Operator declarations -----------------------------------------------
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);

CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);

CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);

/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

/**
 * @brief Placeholder comparison of agg_token with numeric
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and numeric values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_numeric(a agg_token, b numeric)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-numeric not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of numeric with agg_token
 *
 * Symmetric to agg_token_comp_numeric; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION numeric_comp_agg_token(a numeric, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison numeric-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >,
  NEGATOR    = >=
);
/** @brief SQL operator numeric < agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >,
  NEGATOR    = >=
);

/** @brief SQL operator agg_token <= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >=,
  NEGATOR    = >
);
/** @brief SQL operator numeric <= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >=,
  NEGATOR    = >
);

/** @brief SQL operator agg_token = numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator numeric = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator numeric <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @brief SQL operator agg_token >= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <=,
  NEGATOR    = <
);
/** @brief SQL operator numeric >= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <=,
  NEGATOR    = <
);

/** @brief SQL operator agg_token > numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <,
  NEGATOR    = <=
);
/** @brief SQL operator numeric > agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Placeholder comparison of two agg_token values (the diagonal)
 *
 * Never actually called; lets the parser accept agg_token \<op\> agg_token
 * (e.g. sum(x) > sum(y) on materialised tokens), which the ProvSQL
 * rewriter lowers to a gate_cmp at plan time.  Declaring this diagonal
 * also disambiguates `s = s2` (previously "operator is not unique"
 * because both agg_token -> uuid and agg_token -> numeric casts applied).
 */
CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR < (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >, NEGATOR = >=
);
/** @brief SQL operator agg_token <= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >=, NEGATOR = >
);
/** @brief SQL operator agg_token > agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR > (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <, NEGATOR = <=
);
/** @brief SQL operator agg_token >= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR >= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <=, NEGATOR = <
);
/** @brief SQL operator agg_token = agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR = (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = =, NEGATOR = <>
);
/** @brief SQL operator agg_token <> agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <> (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <>, NEGATOR = =
);

/**
 * @brief Placeholder comparison of agg_token with text
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and text values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of text with agg_token
 *
 * Symmetric to agg_token_comp_text; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token = text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator text = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator text <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt>: a thin wrapper around a
 *  provenance gate UUID, used to expose continuous probabilistic
 *  c-tables in SQL.  The UUID indexes either a <tt>gate_rv</tt>
 *  (an actual distribution) or a <tt>gate_value</tt> (a
 *  zero-variance constant produced by <tt>provsql.as_random</tt>).
 *  Binary-coercible with <tt>uuid</tt> (same 16-byte layout), so an
 *  <tt>rv</tt>-typed expression flows directly into any function
 *  expecting a uuid at zero runtime cost.
 *
 *  Constructors live in this group: <tt>provsql.normal(μ, σ)</tt>,
 *  <tt>provsql.uniform(a, b)</tt>, <tt>provsql.exponential(λ)</tt>,
 *  <tt>provsql.erlang(k, λ)</tt>, and <tt>provsql.as_random(c)</tt>.
 *  Operator overloads
 *  (<tt>+ - * /</tt> and the six comparators) are defined further
 *  below, alongside direct <tt>rv_cmp_*</tt> UUID constructors for
 *  callers that want a <tt>gate_cmp</tt> token without going through
 *  the planner hook.
 *  @{
 */

CREATE TYPE random_variable;

/** @brief Input function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Output function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE random_variable (
  internallength = 16,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = char
);

/** @brief Build a random_variable from a UUID (internal). */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Binary-coercible cast random_variable -> uuid.
 *  A random_variable is byte-for-byte a pg_uuid_t (alignment char,
 *  length 16), so WITHOUT FUNCTION lets PostgreSQL reinterpret the
 *  bytes at zero runtime cost.  The cast is ASSIGNMENT (not IMPLICIT):
 *  an implicit cross-domain cast would silently reroute a comparison
 *  such as `v < w` to `uuid < uuid` (raw byte comparison) whenever
 *  `provsql` is not in search_path, since operators are resolved
 *  through search_path but casts are not.  Demoting to ASSIGNMENT
 *  turns that silent wrong result into a clean parse error.  Passing a
 *  random_variable to a uuid-taking function now needs an explicit
 *  `v::uuid` (function resolution never applies assignment casts). */
CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;

/**
 * @brief Internal: true iff @p x is a finite (non-NaN, non-±∞) float8.
 *
 * PostgreSQL's <tt>isnan</tt> is defined for <tt>numeric</tt> only,
 * not for <tt>double precision</tt>; we use the inequality form,
 * which works because PG defines <tt>NaN = NaN</tt> as <tt>TRUE</tt>
 * for floats (so <tt>NaN <> 'NaN'::float8</tt> is <tt>FALSE</tt>).
 */
CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION numeric_times_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric / agg_token. */
CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- Operator declarations -----------------------------------------------
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);

CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);

CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);

/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

/**
 * @brief Placeholder comparison of agg_token with numeric
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and numeric values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_numeric(a agg_token, b numeric)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-numeric not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of numeric with agg_token
 *
 * Symmetric to agg_token_comp_numeric; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION numeric_comp_agg_token(a numeric, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison numeric-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >,
  NEGATOR    = >=
);
/** @brief SQL operator numeric < agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >,
  NEGATOR    = >=
);

/** @brief SQL operator agg_token <= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >=,
  NEGATOR    = >
);
/** @brief SQL operator numeric <= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >=,
  NEGATOR    = >
);

/** @brief SQL operator agg_token = numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator numeric = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator numeric <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @brief SQL operator agg_token >= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <=,
  NEGATOR    = <
);
/** @brief SQL operator numeric >= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <=,
  NEGATOR    = <
);

/** @brief SQL operator agg_token > numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <,
  NEGATOR    = <=
);
/** @brief SQL operator numeric > agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Placeholder comparison of two agg_token values (the diagonal)
 *
 * Never actually called; lets the parser accept agg_token \<op\> agg_token
 * (e.g. sum(x) > sum(y) on materialised tokens), which the ProvSQL
 * rewriter lowers to a gate_cmp at plan time.  Declaring this diagonal
 * also disambiguates `s = s2` (previously "operator is not unique"
 * because both agg_token -> uuid and agg_token -> numeric casts applied).
 */
CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR < (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >, NEGATOR = >=
);
/** @brief SQL operator agg_token <= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >=, NEGATOR = >
);
/** @brief SQL operator agg_token > agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR > (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <, NEGATOR = <=
);
/** @brief SQL operator agg_token >= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR >= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <=, NEGATOR = <
);
/** @brief SQL operator agg_token = agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR = (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = =, NEGATOR = <>
);
/** @brief SQL operator agg_token <> agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <> (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <>, NEGATOR = =
);

/**
 * @brief Placeholder comparison of agg_token with text
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and text values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of text with agg_token
 *
 * Symmetric to agg_token_comp_text; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token = text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator text = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator text <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt>: a thin wrapper around a
 *  provenance gate UUID, used to expose continuous probabilistic
 *  c-tables in SQL.  The UUID indexes either a <tt>gate_rv</tt>
 *  (an actual distribution) or a <tt>gate_value</tt> (a
 *  zero-variance constant produced by <tt>provsql.as_random</tt>).
 *  Binary-coercible with <tt>uuid</tt> (same 16-byte layout), so an
 *  <tt>rv</tt>-typed expression flows directly into any function
 *  expecting a uuid at zero runtime cost.
 *
 *  Constructors live in this group: <tt>provsql.normal(μ, σ)</tt>,
 *  <tt>provsql.uniform(a, b)</tt>, <tt>provsql.exponential(λ)</tt>,
 *  <tt>provsql.erlang(k, λ)</tt>, and <tt>provsql.as_random(c)</tt>.
 *  Operator overloads
 *  (<tt>+ - * /</tt> and the six comparators) are defined further
 *  below, alongside direct <tt>rv_cmp_*</tt> UUID constructors for
 *  callers that want a <tt>gate_cmp</tt> token without going through
 *  the planner hook.
 *  @{
 */

CREATE TYPE random_variable;

/** @brief Input function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Output function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE random_variable (
  internallength = 16,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = char
);

/** @brief Build a random_variable from a UUID (internal). */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Binary-coercible cast random_variable -> uuid.
 *  A random_variable is byte-for-byte a pg_uuid_t (alignment char,
 *  length 16), so WITHOUT FUNCTION lets PostgreSQL reinterpret the
 *  bytes at zero runtime cost.  The cast is ASSIGNMENT (not IMPLICIT):
 *  an implicit cross-domain cast would silently reroute a comparison
 *  such as `v < w` to `uuid < uuid` (raw byte comparison) whenever
 *  `provsql` is not in search_path, since operators are resolved
 *  through search_path but casts are not.  Demoting to ASSIGNMENT
 *  turns that silent wrong result into a clean parse error.  Passing a
 *  random_variable to a uuid-taking function now needs an explicit
 *  `v::uuid` (function resolution never applies assignment casts). */
CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;

/**
 * @brief Internal: true iff @p x is a finite (non-NaN, non-±∞) float8.
 *
 * PostgreSQL's <tt>isnan</tt> is defined for <tt>numeric</tt> only,
 * not for <tt>double precision</tt>; we use the inequality form,
 * which works because PG defines <tt>NaN = NaN</tt> as <tt>TRUE</tt>
 * for floats (so <tt>NaN <> 'NaN'::float8</tt> is <tt>FALSE</tt>).
 */
CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- Operator declarations -----------------------------------------------
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);

CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);

CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);

/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

/**
 * @brief Placeholder comparison of agg_token with numeric
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and numeric values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_numeric(a agg_token, b numeric)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-numeric not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of numeric with agg_token
 *
 * Symmetric to agg_token_comp_numeric; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION numeric_comp_agg_token(a numeric, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison numeric-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >,
  NEGATOR    = >=
);
/** @brief SQL operator numeric < agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >,
  NEGATOR    = >=
);

/** @brief SQL operator agg_token <= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >=,
  NEGATOR    = >
);
/** @brief SQL operator numeric <= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >=,
  NEGATOR    = >
);

/** @brief SQL operator agg_token = numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator numeric = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator numeric <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @brief SQL operator agg_token >= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <=,
  NEGATOR    = <
);
/** @brief SQL operator numeric >= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <=,
  NEGATOR    = <
);

/** @brief SQL operator agg_token > numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <,
  NEGATOR    = <=
);
/** @brief SQL operator numeric > agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Placeholder comparison of two agg_token values (the diagonal)
 *
 * Never actually called; lets the parser accept agg_token \<op\> agg_token
 * (e.g. sum(x) > sum(y) on materialised tokens), which the ProvSQL
 * rewriter lowers to a gate_cmp at plan time.  Declaring this diagonal
 * also disambiguates `s = s2` (previously "operator is not unique"
 * because both agg_token -> uuid and agg_token -> numeric casts applied).
 */
CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR < (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >, NEGATOR = >=
);
/** @brief SQL operator agg_token <= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >=, NEGATOR = >
);
/** @brief SQL operator agg_token > agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR > (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <, NEGATOR = <=
);
/** @brief SQL operator agg_token >= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR >= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <=, NEGATOR = <
);
/** @brief SQL operator agg_token = agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR = (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = =, NEGATOR = <>
);
/** @brief SQL operator agg_token <> agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <> (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <>, NEGATOR = =
);

/**
 * @brief Placeholder comparison of agg_token with text
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and text values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of text with agg_token
 *
 * Symmetric to agg_token_comp_text; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token = text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator text = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator text <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt>: a thin wrapper around a
 *  provenance gate UUID, used to expose continuous probabilistic
 *  c-tables in SQL.  The UUID indexes either a <tt>gate_rv</tt>
 *  (an actual distribution) or a <tt>gate_value</tt> (a
 *  zero-variance constant produced by <tt>provsql.as_random</tt>).
 *  Binary-coercible with <tt>uuid</tt> (same 16-byte layout), so an
 *  <tt>rv</tt>-typed expression flows directly into any function
 *  expecting a uuid at zero runtime cost.
 *
 *  Constructors live in this group: <tt>provsql.normal(μ, σ)</tt>,
 *  <tt>provsql.uniform(a, b)</tt>, <tt>provsql.exponential(λ)</tt>,
 *  <tt>provsql.erlang(k, λ)</tt>, and <tt>provsql.as_random(c)</tt>.
 *  Operator overloads
 *  (<tt>+ - * /</tt> and the six comparators) are defined further
 *  below, alongside direct <tt>rv_cmp_*</tt> UUID constructors for
 *  callers that want a <tt>gate_cmp</tt> token without going through
 *  the planner hook.
 *  @{
 */

CREATE TYPE random_variable;

/** @brief Input function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Output function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE random_variable (
  internallength = 16,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = char
);

/** @brief Build a random_variable from a UUID (internal). */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Binary-coercible cast random_variable -> uuid.
 *  A random_variable is byte-for-byte a pg_uuid_t (alignment char,
 *  length 16), so WITHOUT FUNCTION lets PostgreSQL reinterpret the
 *  bytes at zero runtime cost.  The cast is ASSIGNMENT (not IMPLICIT):
 *  an implicit cross-domain cast would silently reroute a comparison
 *  such as `v < w` to `uuid < uuid` (raw byte comparison) whenever
 *  `provsql` is not in search_path, since operators are resolved
 *  through search_path but casts are not.  Demoting to ASSIGNMENT
 *  turns that silent wrong result into a clean parse error.  Passing a
 *  random_variable to a uuid-taking function now needs an explicit
 *  `v::uuid` (function resolution never applies assignment casts). */
CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;

/**
 * @brief Internal: true iff @p x is a finite (non-NaN, non-±∞) float8.
 *
 * PostgreSQL's <tt>isnan</tt> is defined for <tt>numeric</tt> only,
 * not for <tt>double precision</tt>; we use the inequality form,
 * which works because PG defines <tt>NaN = NaN</tt> as <tt>TRUE</tt>
 * for floats (so <tt>NaN <> 'NaN'::float8</tt> is <tt>FALSE</tt>).
 */
CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

-- 10. Refresh the cached OID lookups so a backend warmed under 1.8.0 picks
--    up the new surface on its next get_constants() call.
SELECT reset_constants_cache();
