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

-- 7. Refresh the cached OID lookups so a backend warmed under 1.8.0 picks
--    up the new surface on its next get_constants() call.
SELECT reset_constants_cache();
