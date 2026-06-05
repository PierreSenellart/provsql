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

-- 10. Idempotent add_provenance / create_provenance_mapping: re-running
--    them on an already-tracked table / existing mapping is a NOTICE-and-
--    return no-op, so setup scripts and notebook cells can be replayed.
--    add_provenance has two instantiations: the common one, and a
--    PostgreSQL-14+ override wired to update-provenance tracking.
CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$$
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
$$ LANGUAGE plpgsql SECURITY DEFINER;

DO $do14$
BEGIN
  IF current_setting('server_version_num')::int >= 140000 THEN
    EXECUTE $prov14$
CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$$
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
$$ LANGUAGE plpgsql SECURITY DEFINER;
    $prov14$;
  END IF;
END
$do14$;

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

-- 11. agg_token arithmetic records its computed scalar in the
--    gate_arith's extra (agg_arith_make, now behind all 13 operator
--    functions), and agg_token_value_text recovers the "value (*)"
--    display for arith tokens like agg ones.
/** @brief Mint (or reuse) the gate_arith for an agg_token arithmetic
 *  result and return the agg_token carrying it.
 *
 * Also records the computed scalar in the gate's @c extra -- exactly
 * what aggregate evaluation does for @c agg gates -- so
 * @c agg_token_value_text can recover the @c "value (*)" display from
 * the bare UUID (as ProvSQL Studio does for result cells under
 * @c provsql.aggtoken_text_as_uuid). The gate UUID is deterministic in
 * (op, children), so re-recording the (identical) value is idempotent. */
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

-- agg_token \<op\> agg_token --------------------------------------------

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


-- 12. Corrective fixes for damage that older shipped upgrade scripts left
--    on upgraded databases (a database freshly installed at any version is
--    unaffected; every block is conditional and idempotent).
--
-- 12a. The 1.4.0 -> 1.5.0 script's operator-existence guards did not
--    distinguish SHELL operators (auto-created by COMMUTATOR / NEGATOR
--    forward references) from real ones, so <>, >= and > on
--    (random_variable, random_variable) stayed shells.  CREATE OPERATOR
--    over a shell fills it in.
DO $$ BEGIN
  IF EXISTS (
    SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '<>'
      AND o.oprleft = 'provsql.random_variable'::regtype
      AND o.oprright = 'provsql.random_variable'::regtype
      AND o.oprcode = 0
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
  IF EXISTS (
    SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '>='
      AND o.oprleft = 'provsql.random_variable'::regtype
      AND o.oprright = 'provsql.random_variable'::regtype
      AND o.oprcode = 0
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
  IF EXISTS (
    SELECT 1 FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '>'
      AND o.oprleft = 'provsql.random_variable'::regtype
      AND o.oprright = 'provsql.random_variable'::regtype
      AND o.oprcode = 0
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

-- 12b. The 1.6.0 -> 1.7.0 script shipped without SET search_path, so the
--    functions it created landed in the session default schema (typically
--    public).  Move each stray into provsql; if a provsql copy already
--    exists (it should not, but be safe), drop the stray instead.  The
--    removed probability_benchmark helpers get the same treatment as
--    section 6, public-schema edition.
DO $do$
DECLARE
  sig text;
BEGIN
  FOREACH sig IN ARRAY ARRAY[
    'eval_recursive(text, text, text, text, integer)',
    'compile_to_ddnnf(uuid, text)',
    'compile_to_ddnnf_dot(uuid, text)',
    'ddnnf_stats(uuid, text)',
    'tree_decomposition_dot(uuid)',
    'tool_available(text)',
    'tseytin_cnf(uuid, boolean, boolean)',
    'tseytin_cnf_mapping(uuid)',
    'tseytin_cnf_mapping_json(uuid)'
  ] LOOP
    IF to_regprocedure('public.' || sig) IS NOT NULL THEN
      IF to_regprocedure('provsql.' || sig) IS NULL THEN
        EXECUTE format('ALTER FUNCTION public.%s SET SCHEMA provsql', sig);
      ELSE
        EXECUTE format('DROP FUNCTION public.%s', sig);
      END IF;
    END IF;
  END LOOP;
  FOREACH sig IN ARRAY ARRAY[
    'probability_benchmark(uuid, integer, text)',
    '_probability_benchmark_one(uuid, text, text)'
  ] LOOP
    IF to_regprocedure('public.' || sig) IS NOT NULL THEN
      EXECUTE format('DROP FUNCTION public.%s', sig);
    END IF;
  END LOOP;
END
$do$;

-- 13. Function-body parity with a fresh 1.9.0 install: the definitions
--    below evolved after the upgrade script (or an older script) last
--    captured them; re-create each with its current body.  Verified by
--    diffing the catalog of an upgraded database against a fresh install
--    (functions, aggregates, operators, casts, enum values).
CREATE OR REPLACE FUNCTION agg_token_make(tok uuid, val numeric)
  RETURNS agg_token AS
$$
  SELECT format('( %s , %s )', tok::text, val::text)::provsql.agg_token;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
  SET search_path=provsql,pg_temp,public;

CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

CREATE OR REPLACE FUNCTION as_random(c double precision)
  RETURNS random_variable AS
$$
DECLARE
  -- Canonicalise -0.0 to +0.0: IEEE 754 defines x + 0.0 = +0.0 for
  -- both signed zeros, and is identity for finite, NaN, and ±Infinity.
  -- Without this, as_random(-0.0) and as_random(+0.0) would produce
  -- different gate UUIDs (their CAST AS VARCHAR text representations
  -- differ: '-0' vs '0') even though they denote the same constant.
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
    -- PostgreSQL diverges from IEEE 754: NaN = NaN is TRUE there, so
    -- the canonical x <> x NaN test doesn't fire.  Compare against the
    -- literal 'NaN'::float8 instead, and reject ±Infinity for outcomes
    -- explicitly.
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

  -- Degenerate case: exactly one positive-mass outcome (the rest are
  -- zero).  The "categorical" is then a Dirac point mass; skip the
  -- block-allocation entirely and return @c as_random(v), which yields
  -- a shared, v5-keyed gate_value -- exactly what downstream
  -- evaluators (rv_moment, AnalyticEvaluator, rv_support) treat
  -- specially.  Saves a key gate and a mulinput per call, and lets
  -- two calls to @c categorical({1.0}, {v}) collide on the same
  -- gate_value UUID instead of producing distinct anonymous blocks.
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

  -- Mint the block's key anchor.  Probability 1.0 matches the
  -- joint-table convention: the categorical mass lives on the
  -- mulinputs, the key just identifies the block.
  key_token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(key_token, 'input');
  PERFORM provsql.set_prob(key_token, 1.0);

  -- One mulinput per positive-probability outcome.  Zero-probability
  -- entries contribute no mass and are skipped: the gate_mixture's
  -- wire vector is otherwise polluted with no-op leaves.
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
  -- iterative binomial coefficient C(k, i)
  k_double float8;
BEGIN
  IF pg_typeof(input) = 'random_variable'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    -- See variance() above: rv_moment handles the conditional/unconditional
    -- dispatch internally based on the resolved prov gate type.
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
    -- mu may be ±Infinity for empty MIN / MAX with positive empty
    -- probability; central_moment is undefined in that case.
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
      -- C(k, i+1) = C(k, i) * (k - i) / (i + 1)
      IF i < k THEN
        binom := binom * (k_double - i) / (i + 1);
      END IF;
    END LOOP;
    RETURN total;
  END IF;

  RAISE EXCEPTION 'central_moment() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

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
    -- See variance() above: rv_moment handles the conditional/unconditional
    -- dispatch internally based on the resolved prov gate type.
    RETURN provsql.rv_moment(
      (input::random_variable)::uuid, k, false, prov);
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    RETURN agg_raw_moment(input::agg_token, k, prov, method, arguments);
  END IF;

  RAISE EXCEPTION 'moment() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

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
    -- Conditioning on prov is handled inside rv_moment: when prov
    -- resolves to gate_one() (the default, or load-time
    -- simplification of any always-true sub-circuit) the
    -- unconditional analytical path runs unchanged; otherwise the
    -- joint-circuit loader unifies shared gate_rv leaves between
    -- input and prov, and the conditional path runs either
    -- truncated-distribution closed form or MC rejection.
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

  -- random_variable is binary-coercible to uuid (explicit cast
  -- below), so a single rv_support call covers both shapes.
  -- rv_support handles
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

CREATE OR REPLACE FUNCTION circuit_subgraph(root UUID, max_depth INT DEFAULT 8)
  RETURNS TABLE(node UUID, parent UUID, child_pos INT, gate_type TEXT, info1 TEXT, info2 TEXT, depth INT) AS
$$
  WITH RECURSIVE bfs(node, parent, child_pos, depth) AS (
    SELECT root, NULL::UUID, NULL::INT, 0
      UNION ALL
    SELECT c.t, b.node, c.idx::INT, b.depth + 1
    FROM bfs b
    CROSS JOIN LATERAL unnest(provsql.get_children(b.node))
      WITH ORDINALITY AS c(t, idx)
    WHERE b.depth < max_depth
  ),
  -- Each node's canonical depth is its longest-path distance from the
  -- root (the standard circuit-depth notion: the longest chain of
  -- gates separating the node from the output). The recursive CTE
  -- enumerates paths up to @c max_depth, so MAX over those is the
  -- longest path of length at most @c max_depth.
  node_depth AS (
    SELECT node, MAX(depth) AS depth FROM bfs GROUP BY node
  ),
  -- All distinct (parent, child, child_pos) triples seen during the BFS.
  -- A child reached from k parents within the bound contributes k rows.
  -- Self-joins (times(x, x)) contribute one row per child position.
  edges AS (
    SELECT DISTINCT parent, node AS child, child_pos
    FROM bfs WHERE parent IS NOT NULL
  )
  SELECT
    d.node,
    e.parent,
    e.child_pos,
    provsql.get_gate_type(d.node)::TEXT,
    i.info1::TEXT,
    i.info2::TEXT,
    d.depth
  FROM node_depth d
  LEFT JOIN edges e ON e.child = d.node
  LEFT JOIN LATERAL provsql.get_infos(d.node) i ON TRUE
  ORDER BY d.depth, d.node, e.parent;
$$ LANGUAGE sql STABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION identify_token(
  token UUID, OUT table_name regclass, OUT nb_columns integer) AS
$$
DECLARE
  t RECORD;
  result RECORD;
BEGIN
  table_name:=NULL;
  nb_columns:=-1;
  FOR t IN
    SELECT relname,
      (SELECT count(*) FROM pg_attribute a2 WHERE a2.attrelid=a1.attrelid AND attnum>0 AND atttypid<>0)-1 c
    FROM pg_attribute a1 JOIN pg_type ON atttypid=pg_type.oid
                        JOIN pg_class ON attrelid=pg_class.oid
                        JOIN pg_namespace ON relnamespace=pg_namespace.oid
    WHERE typname='uuid' AND relkind='r'
                                     AND nspname<>'provsql'
                                     AND attname='provsql'
  LOOP
    EXECUTE format('SELECT * FROM %I WHERE provsql=%L',t.relname,token) INTO result;
    -- Test result.provsql rather than the whole record: "RECORD IS NOT NULL"
    -- is true only when every field is non-null, so a matched row that has any
    -- NULL data column would be wrongly skipped. The provsql column is the
    -- (non-null) token we matched on, so it is set iff a row was found.
    IF result.provsql IS NOT NULL THEN
      table_name:=t.relname;
      nb_columns:=t.c;
      EXIT;
    END IF;
  END LOOP;
END
$$ LANGUAGE plpgsql STRICT;

CREATE OR REPLACE FUNCTION provenance_evaluate(
  token UUID,
  token2value regclass,
  element_one anyelement,
  value_type regtype,
  plus_function regproc,
  times_function regproc,
  monus_function regproc,
  delta_function regproc)
  RETURNS anyelement AS
$$
DECLARE
  gate_type provenance_gate;
  result ALIAS FOR $0;
  children UUID[];
--  cmp_value anyelement;
--  temp_result anyelement;
  value_text TEXT;
BEGIN
  SELECT get_gate_type(token) INTO gate_type;

  IF gate_type IS NULL THEN
    RETURN NULL;

  ELSIF gate_type = 'input' THEN
    EXECUTE format('SELECT value FROM %s WHERE provenance=%L', token2value, token)
      INTO result;
    IF result IS NULL THEN
      result := element_one;
    END IF;
  ELSIF gate_type = 'mulinput' THEN
    SELECT concat('{',(get_children(token))[1]::text,'=',(get_infos(token)).info1,'}')
      INTO result;
  ELSIF gate_type='update' THEN
    EXECUTE format('SELECT value FROM %s WHERE provenance=%L',token2value,token) INTO result;
    IF result IS NULL THEN
      result:=element_one;
    END IF;
  ELSIF gate_type = 'plus' THEN
    EXECUTE format('SELECT %s(provsql.provenance_evaluate(t,%L,%L::%s,%L,%L,%L,%L,%L)) FROM unnest(get_children(%L)) AS t',
      plus_function, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function, token)
      INTO result;

  ELSIF gate_type = 'times' THEN
    EXECUTE format('SELECT %s(provsql.provenance_evaluate(t,%L,%L::%s,%L,%L,%L,%L,%L)) FROM unnest(get_children(%L)) AS t',
      times_function, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function, token)
      INTO result;

  ELSIF gate_type = 'monus' THEN
    IF monus_function IS NULL THEN
      RAISE EXCEPTION USING MESSAGE='Provenance with negation evaluated over a semiring without monus function';
    ELSE
      EXECUTE format('SELECT %s(a1,a2) FROM (SELECT provsql.provenance_evaluate(c[1],%L,%L::%s,%L,%L,%L,%L,%L) AS a1, ' ||
                     'provsql.provenance_evaluate(c[2],%L,%L::%s,%L,%L,%L,%L,%L) AS a2 FROM get_children(%L) c) tmp',
        monus_function, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function,
        token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function, token)
      INTO result;
    END IF;

  ELSIF gate_type = 'eq' THEN
    EXECUTE format('SELECT provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L)',
      token, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function)
      INTO result;

/*  elsif gate_type = 'cmp' then

    EXECUTE format('SELECT provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L)',
      token, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function)
      INTO temp_result;

    EXECUTE format('SELECT get_extra((get_children(%L))[2])', token)
      INTO cmp_value;

    IF temp_result::text = cmp_value::text THEN
      SELECT concat('{',temp_result::text,'=',cmp_value::text,'}')
      INTO result;
    ELSE
      RETURN gate_zero()
      */



  ELSIF gate_type = 'delta' THEN
    IF delta_function IS NULL THEN
      RAISE EXCEPTION USING MESSAGE='Provenance with aggregation evaluated over a semiring without delta function';
    ELSE
      EXECUTE format('SELECT %I(a) FROM (SELECT provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L) AS a) tmp',
        delta_function, token, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function)
      INTO result;
    END IF;

  ELSIF gate_type = 'zero' THEN
    EXECUTE format('SELECT %I(a) FROM (SELECT %L::%I AS a WHERE FALSE) temp', plus_function, element_one, value_type)
      INTO result;

  ELSIF gate_type = 'one' THEN
    EXECUTE format('SELECT %L::%I', element_one, value_type)
      INTO result;

  ELSIF gate_type = 'project' THEN
    EXECUTE format('SELECT provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L)',
      token, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function)
      INTO result;

  ELSIF gate_type = 'annotation' THEN
    -- Transparent single-child wrapper (carries the inversion-free certificate
    -- / per-input order keys in extra, inert for every semiring): evaluate
    -- through to the child, like 'project'.
    EXECUTE format('SELECT provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L)',
      token, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function)
      INTO result;

  ELSE
    RAISE EXCEPTION USING MESSAGE='provenance_evaluate cannot be called on formulas using ' || gate_type || ' gates; use compiled semirings instead';
  END IF;

  RETURN result;
END
$$ LANGUAGE plpgsql PARALLEL SAFE STABLE;

CREATE OR REPLACE FUNCTION provenance_times(VARIADIC tokens uuid[])
  RETURNS UUID AS
$$
DECLARE
  times_token uuid;
  filtered_tokens uuid[];
BEGIN
  SELECT array_agg(t) FROM unnest(tokens) t WHERE t IS NOT NULL AND t <> gate_one() INTO filtered_tokens;

  -- Dispatch on the FILTERED count: a single survivor short-circuits
  -- to that token directly (no useless single-child times gate); zero
  -- survivors collapse to the identity. Using array_length(tokens, 1)
  -- here would miss the [one, cmp] → [cmp] case, leaving the cmp wrapped
  -- in a one-child times when its only sibling was gate_one().
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

CREATE OR REPLACE FUNCTION remove_provenance(_tbl regclass)
  RETURNS void AS
$$
DECLARE
BEGIN
  PERFORM provsql.remove_table_info(_tbl::oid);
  -- Drop the BEFORE INSERT/UPDATE guard first: it has a column
  -- dependency on provsql (via the OF provsql clause), so the
  -- subsequent DROP COLUMN would otherwise raise.
  BEGIN
    EXECUTE format('DROP TRIGGER provenance_guard on %s', _tbl);
  EXCEPTION WHEN undefined_object THEN
  END;
  EXECUTE format('ALTER TABLE %s DROP COLUMN provsql', _tbl);
  BEGIN
    EXECUTE format('DROP TRIGGER add_gate on %s', _tbl);
  EXCEPTION WHEN undefined_object THEN
  END;
  BEGIN
    EXECUTE format('DROP TRIGGER insert_statement on %s', _tbl);
    EXECUTE format('DROP TRIGGER update_statement on %s', _tbl);
    EXECUTE format('DROP TRIGGER delete_statement on %s', _tbl);
  EXCEPTION WHEN undefined_object THEN
  END;
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION repair_key(_tbl regclass, key_att text)
  RETURNS void AS
$$
DECLARE
  r RECORD;
  rows_query TEXT;
  block_key_cols INT2[];
BEGIN
  -- Resolve the (possibly comma-separated) key_att text into the
  -- corresponding pg_attribute.attnum values for the safe-query
  -- metadata.  Names are trimmed; quoting is not supported because
  -- repair_key has never accepted quoted identifiers in key_att.
  IF key_att = '' THEN
    block_key_cols := ARRAY[]::INT2[];
  ELSE
    SELECT array_agg(a.attnum ORDER BY t.ord)::INT2[]
      INTO block_key_cols
      FROM unnest(string_to_array(key_att, ',')) WITH ORDINALITY AS t(name, ord)
      JOIN pg_attribute a
        ON a.attrelid = _tbl
       AND a.attname  = trim(t.name)
       AND a.attnum   > 0
       AND NOT a.attisdropped;
    IF block_key_cols IS NULL OR array_length(block_key_cols, 1) IS NULL THEN
      RAISE EXCEPTION 'repair_key: could not resolve key columns from "%"', key_att;
    END IF;
    IF array_length(block_key_cols, 1) > 16 THEN
      RAISE EXCEPTION 'repair_key: block key wider than 16 columns is not supported';
    END IF;
  END IF;

  -- Same column shape as add_provenance: no UNIQUE, no DEFAULT past
  -- the initial backfill (the guard trigger added after the rename
  -- takes over both jobs once the column has been renamed to its
  -- final name).  The DEFAULT is kept here only so the second pass
  -- below can read provsql_temp from the user-visible rows
  -- without a separate UPDATE.
  EXECUTE format('ALTER TABLE %s ADD COLUMN provsql_temp UUID DEFAULT public.uuid_generate_v4()', _tbl);

  -- Build a per-group mapping (key columns + a fresh key_token + the
  -- group size) once, then use it for both the create_gate(key_token,
  -- 'input') first pass and the per-row mulinput second pass.  Going
  -- through a temp table avoids re-running uuid_generate_v4() (which
  -- would produce different UUIDs the second time).  USING (%1$s) on
  -- the second pass handles the multi-column case uniformly.
  -- ON COMMIT DROP plus the explicit DROP TABLE at the end of this
  -- function leave the temp table cleaned up across transactions and
  -- across repeated calls in the same transaction.
  IF key_att = '' THEN
    EXECUTE format(
      'CREATE TEMP TABLE provsql_repair_key_tmp ON COMMIT DROP AS
         SELECT public.uuid_generate_v4() AS provsql_key_token,
                COUNT(*) AS provsql_group_size
           FROM %s', _tbl);
    rows_query := format(
      'SELECT t.provsql_temp,
              k.provsql_key_token AS key_token,
              ROW_NUMBER() OVER (ORDER BY t.ctid) AS within_group,
              k.provsql_group_size AS group_size
         FROM %s t CROSS JOIN provsql_repair_key_tmp k', _tbl);
  ELSE
    EXECUTE format(
      'CREATE TEMP TABLE provsql_repair_key_tmp ON COMMIT DROP AS
         SELECT %1$s,
                public.uuid_generate_v4() AS provsql_key_token,
                COUNT(*) AS provsql_group_size
           FROM %2$s
       GROUP BY %1$s', key_att, _tbl);
    rows_query := format(
      'SELECT t.provsql_temp,
              k.provsql_key_token AS key_token,
              ROW_NUMBER() OVER (PARTITION BY k.provsql_key_token
                                 ORDER BY t.ctid) AS within_group,
              k.provsql_group_size AS group_size
         FROM %2$s t
         JOIN provsql_repair_key_tmp k USING (%1$s)', key_att, _tbl);
  END IF;

  -- Pass 1: one input gate per group key.
  FOR r IN SELECT provsql_key_token FROM provsql_repair_key_tmp LOOP
    PERFORM provsql.create_gate(r.provsql_key_token, 'input');
  END LOOP;

  -- Pass 2: per row, attach a mulinput gate to its group's key token.
  FOR r IN EXECUTE rows_query LOOP
    PERFORM provsql.create_gate(r.provsql_temp, 'mulinput', ARRAY[r.key_token]);
    PERFORM provsql.set_prob(r.provsql_temp, 1./r.group_size);
    PERFORM provsql.set_infos(r.provsql_temp, r.within_group::int);
  END LOOP;

  DROP TABLE provsql_repair_key_tmp;

  EXECUTE format('ALTER TABLE %s ALTER COLUMN provsql_temp DROP DEFAULT', _tbl);
  EXECUTE format('ALTER TABLE %s RENAME COLUMN provsql_temp TO provsql', _tbl);
  EXECUTE format('CREATE INDEX ON %s(provsql)', _tbl);
  EXECUTE format(
    'CREATE TRIGGER provenance_guard BEFORE INSERT OR UPDATE OF provsql '
    'ON %s FOR EACH ROW EXECUTE PROCEDURE provsql.provenance_guard()',
    _tbl);
  PERFORM provsql.set_table_info(_tbl::oid, 'bid', block_key_cols);
  -- Base BID tables also have themselves as their sole ancestor.  Same
  -- rationale as the @c add_provenance branch above.
  PERFORM provsql.set_ancestors(_tbl::oid, ARRAY[_tbl::oid]);
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION resolve_input(uuid UUID)
  RETURNS TABLE(relation regclass, row_data JSONB) AS
$$
DECLARE
  t RECORD;
  rel regclass;
  rd  JSONB;
  -- ProvSQL's rewriter unconditionally appends a provsql column to the
  -- targetlist of any SELECT reading from a tracked relation; capture and
  -- discard it here rather than disabling the rewriter for the whole call.
  ign UUID;
BEGIN
  FOR t IN
    SELECT c.oid::regclass AS regc
    FROM pg_attribute a
         JOIN pg_class c ON a.attrelid = c.oid
         JOIN pg_namespace ns ON c.relnamespace = ns.oid
         JOIN pg_type ty ON a.atttypid = ty.oid
    WHERE a.attname = 'provsql'
      AND ty.typname = 'uuid'
      AND c.relkind = 'r'
      AND ns.nspname <> 'provsql'
      AND a.attnum > 0
  LOOP
    FOR rel, rd, ign IN
      EXECUTE format(
        'SELECT %L::regclass, to_jsonb(t) - ''provsql'', t.provsql FROM %s AS t WHERE provsql = $1',
        t.regc, t.regc)
      USING uuid
    LOOP
      relation := rel;
      row_data := rd;
      RETURN NEXT;
    END LOOP;
  END LOOP;
END
$$ LANGUAGE plpgsql STABLE;

CREATE OR REPLACE FUNCTION sub_circuit_for_where(token UUID)
  RETURNS TABLE(f UUID, t UUID, gate_type provenance_gate, table_name REGCLASS, nb_columns INTEGER, infos INTEGER[], extra TEXT) AS
$$
    WITH RECURSIVE transitive_closure(f,t,idx,gate_type) AS (
      SELECT $1,t,id,provsql.get_gate_type($1) FROM unnest(provsql.get_children($1)) WITH ORDINALITY AS a(t,id)
        UNION ALL
      SELECT p1.t,u,id,provsql.get_gate_type(p1.t) FROM transitive_closure p1, unnest(provsql.get_children(p1.t)) WITH ORDINALITY AS a(u, id)
    ) SELECT f, t, gate_type, table_name, nb_columns, ARRAY[(get_infos(f)).info1, (get_infos(f)).info2], get_extra(f) FROM (
      -- One row per distinct (parent, child, child-position) edge.  The
      -- recursive closure (UNION ALL) re-emits a gate's outgoing edges once per
      -- path that reaches it, so a *shared* non-input gate would otherwise be
      -- reported with duplicate edges; DISTINCT on the (f,t,idx) triple
      -- collapses those while keeping genuine repeated children (same f,t,
      -- different idx, e.g. a self-product).  Without this, a shared
      -- single-child gate (notably an inversion-free order-marker annotation)
      -- gets its child wired k times in the where-circuit -> the locator sets
      -- are duplicated k-fold.
      SELECT DISTINCT f, t::uuid, idx, gate_type, NULL::regclass AS table_name, NULL::integer AS nb_columns FROM transitive_closure
      UNION ALL
        SELECT DISTINCT t, NULL::uuid, NULL::int, 'input'::provenance_gate, (id).table_name, (id).nb_columns FROM transitive_closure JOIN (SELECT t AS prov, provsql.identify_token(t) as id FROM transitive_closure WHERE t NOT IN (SELECT f FROM transitive_closure)) temp ON t=prov
      UNION ALL
        SELECT DISTINCT $1, NULL::uuid, NULL::int, 'input'::provenance_gate, (id).table_name, (id).nb_columns FROM (SELECT provsql.identify_token($1) AS id WHERE $1 NOT IN (SELECT f FROM transitive_closure)) temp
      ) t
    -- order each parent's edges by child position so the where-circuit's TIMES
    -- concatenation reproduces the column order (input rows have idx NULL).
    ORDER BY f, idx
$$
LANGUAGE sql;

CREATE OR REPLACE FUNCTION setup_search_path()
  RETURNS text
  LANGUAGE plpgsql AS $$
DECLARE
  db        text := current_database();
  cfg       text[];
  cur       text;        -- existing database-level search_path value
  new_path  text;
BEGIN
  -- setrole = 0 selects the database-wide default, not a per-role override.
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
    -- No database-level search_path at all: install the documented
    -- default with provsql appended.
    new_path := '"$user", public, provsql';
    EXECUTE format('ALTER DATABASE %I SET search_path = %s', db, new_path);
    RAISE NOTICE 'ProvSQL: set search_path = % for database "%" (no previous database-level setting). Only new sessions are affected.',
      new_path, db;
    RETURN new_path;
  END IF;

  -- Already contains provsql as a path element?  Idempotent no-op.
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
$$;


-- 14. Refresh the cached OID lookups so a backend warmed under 1.8.0 picks
--    up the new surface on its next get_constants() call.
SELECT reset_constants_cache();
