/**
 * @file
 * @brief ProvSQL upgrade script: 1.3.1 → 1.4.0
 *
 * 1.4.0 introduces a substantial new SQL surface: nine new compiled
 * semiring evaluators (`sr_how`, `sr_which`, `sr_tropical`,
 * `sr_viterbi`, `sr_lukasiewicz`, `sr_minmax`, `sr_maxmin`, plus
 * the PG14+ `sr_temporal` / `sr_interval_num` / `sr_interval_int`
 * family), two circuit-introspection helpers (`circuit_subgraph`,
 * `resolve_input`), a renderer for `agg_token` cells
 * (`agg_token_value_text`), and a signature change to `sr_boolexpr`
 * (the provenance-mapping argument is now optional).
 *
 * Two existing function bodies also change.  `provenance_aggregate`
 * folds the aggregate function OID into the agg gate UUID, so
 * `SUM(id)` and `AVG(id)` over the same children become distinct
 * gates instead of racing on `set_infos`.  `agg_token_out` is now
 * `STABLE` (was `IMMUTABLE`) so the per-row cell text reflects
 * in-session flips of the new `provsql.aggtoken_text_as_uuid` GUC.
 *
 * The two new GUCs (`provsql.tool_search_path` and
 * `provsql.aggtoken_text_as_uuid`) are registered C-side by
 * `_PG_init` and need no SQL.
 *
 * The mmap circuit format is unchanged from 1.3.0 — no migration
 * required.  The PG14-only block at the bottom replaces the legacy
 * `union_tstzintervals_{plus,times,monus,plus_state,times_state}`
 * helpers (subsumed by `sr_temporal`) and updates `timetravel`,
 * `timeslice`, `history`, `get_valid_time`, and
 * `union_tstzintervals` itself to call `sr_temporal` directly.
 */
SET search_path TO provsql;

-- 1. provenance_aggregate: fold aggfnoid into the agg gate UUID so
--    SUM(id) and AVG(id) over the same children are distinct gates.

CREATE OR REPLACE FUNCTION provenance_aggregate(
    aggfnoid integer,
    aggtype integer,
    val anyelement,
    tokens uuid[])
  RETURNS agg_token AS
$$
DECLARE
  c INTEGER;
  agg_tok uuid;
  agg_val varchar;
BEGIN
  c:=COALESCE(array_length(tokens, 1), 0);

  agg_val = CAST(val as VARCHAR);

  IF c = 0 THEN
    agg_tok := gate_zero();
  ELSE
    agg_tok := uuid_generate_v5(
      uuid_ns_provsql(),
      concat('agg',aggfnoid,tokens));
    PERFORM create_gate(agg_tok, 'agg', tokens);
    PERFORM set_infos(agg_tok, aggfnoid, aggtype);
    PERFORM set_extra(agg_tok, agg_val);
  END IF;

  RETURN '( '||agg_tok||' , '||agg_val||' )';
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql,pg_temp,public SECURITY DEFINER;

-- 2. agg_token_out: STABLE (was IMMUTABLE) — output now depends on
--    the provsql.aggtoken_text_as_uuid GUC.

CREATE OR REPLACE FUNCTION agg_token_out(agg_token)
  RETURNS cstring
  AS 'provsql','agg_token_out' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- 3. circuit_subgraph: new BFS introspection helper used by Studio
--    and any downstream tool that needs to walk a bounded slice of
--    the circuit DAG.

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
  node_depth AS (
    SELECT node, MIN(depth) AS depth FROM bfs GROUP BY node
  ),
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

-- 4. resolve_input: walk every provenance-tracked relation and
--    return the row whose provsql column matches a given UUID.

CREATE OR REPLACE FUNCTION resolve_input(uuid UUID)
  RETURNS TABLE(relation regclass, row_data JSONB) AS
$$
DECLARE
  t RECORD;
  rel regclass;
  rd  JSONB;
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

-- 5. agg_token_value_text: recover "value (*)" from an agg gate UUID
--    when aggtoken_text_as_uuid is on and the cell rendered as a UUID.

CREATE OR REPLACE FUNCTION agg_token_value_text(token UUID)
  RETURNS text AS
$$
  SELECT CASE
    WHEN provsql.get_gate_type(token) = 'agg'
      THEN provsql.get_extra(token) || ' (*)'
    ELSE NULL
  END;
$$ LANGUAGE sql STABLE STRICT PARALLEL SAFE;

-- 6. sr_boolexpr: signature change.  Drop the old 1-arg form first so
--    PostgreSQL resolves to the new 2-arg form on the next call.
--    Existing callers that passed only the token continue to work
--    because the second argument now defaults to NULL.

DROP FUNCTION IF EXISTS sr_boolexpr(ANYELEMENT);

CREATE FUNCTION sr_boolexpr(token ANYELEMENT, token2value regclass = NULL)
  RETURNS VARCHAR AS
$$
BEGIN
  IF token IS NULL THEN
    RETURN NULL;
  END IF;
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'boolexpr',
    '⊤'::VARCHAR
  );
END
$$ LANGUAGE plpgsql PARALLEL SAFE STABLE;

-- 7. New compiled semirings (PG-version agnostic).

CREATE FUNCTION sr_how(token ANYELEMENT, token2value regclass)
  RETURNS VARCHAR AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'how',
    '{}'::VARCHAR
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

CREATE FUNCTION sr_which(token ANYELEMENT, token2value regclass)
  RETURNS VARCHAR AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'which',
    '{}'::VARCHAR
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

CREATE FUNCTION sr_tropical(token ANYELEMENT, token2value regclass)
  RETURNS FLOAT AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'tropical',
    0::FLOAT
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

CREATE FUNCTION sr_viterbi(token ANYELEMENT, token2value regclass)
  RETURNS FLOAT AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'viterbi',
    1::FLOAT
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

CREATE FUNCTION sr_lukasiewicz(token ANYELEMENT, token2value regclass)
  RETURNS FLOAT AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'lukasiewicz',
    1::FLOAT
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

CREATE FUNCTION sr_minmax(token UUID, token2value regclass, element_one ANYENUM)
  RETURNS ANYENUM AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'minmax',
    element_one
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

CREATE FUNCTION sr_maxmin(token UUID, token2value regclass, element_one ANYENUM)
  RETURNS ANYENUM AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'maxmin',
    element_one
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

-- 8. PG14+ block: drop the legacy union_tstzintervals_* helpers,
--    add sr_temporal / sr_interval_num / sr_interval_int, and
--    rewrite the temporal-DB helpers to call sr_temporal directly.
--    The existing union_tstzintervals(UUID, regclass) wrapper is
--    rewritten as a thin SQL call into sr_temporal so user code
--    that imports it keeps working unchanged.

DO $$
BEGIN
  IF current_setting('server_version_num')::int >= 140000 THEN

    EXECUTE $sql$
      DROP AGGREGATE IF EXISTS provsql.union_tstzintervals_plus(tstzmultirange);
      DROP AGGREGATE IF EXISTS provsql.union_tstzintervals_times(tstzmultirange);
      DROP FUNCTION IF EXISTS provsql.union_tstzintervals_plus_state(tstzmultirange, tstzmultirange);
      DROP FUNCTION IF EXISTS provsql.union_tstzintervals_times_state(tstzmultirange, tstzmultirange);
      DROP FUNCTION IF EXISTS provsql.union_tstzintervals_monus(tstzmultirange, tstzmultirange);
    $sql$;

    EXECUTE $sql$
CREATE FUNCTION sr_temporal(token ANYELEMENT, token2value regclass)
  RETURNS tstzmultirange AS
$func$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'interval_union',
    '{(,)}'::tstzmultirange
  );
END
$func$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;
    $sql$;

    EXECUTE $sql$
CREATE FUNCTION sr_interval_num(token ANYELEMENT, token2value regclass)
  RETURNS nummultirange AS
$func$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'interval_union',
    '{(,)}'::nummultirange
  );
END
$func$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;
    $sql$;

    EXECUTE $sql$
CREATE FUNCTION sr_interval_int(token ANYELEMENT, token2value regclass)
  RETURNS int4multirange AS
$func$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'interval_union',
    '{(,)}'::int4multirange
  );
END
$func$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;
    $sql$;

    EXECUTE $sql$
CREATE OR REPLACE FUNCTION union_tstzintervals(
    token UUID,
    token2value regclass
)
RETURNS tstzmultirange AS
$func$
  SELECT sr_temporal(token, token2value)
$func$ LANGUAGE SQL PARALLEL SAFE STABLE;
    $sql$;

    EXECUTE $sql$
CREATE OR REPLACE FUNCTION timetravel(
  tablename text,
  at_time timestamptz
)
RETURNS SETOF record
LANGUAGE plpgsql
AS
$func$
BEGIN
    RETURN QUERY EXECUTE format(
        '
          SELECT
            %1$I.*,
            sr_temporal(provenance(), %2$L)
          FROM
            %1$I
          WHERE
            sr_temporal(provenance(), %2$L) @> %3$L::timestamptz
        ',
        tablename,
        'provsql.time_validity_view',
        at_time::text
    );
END;
$func$;
    $sql$;

    EXECUTE $sql$
CREATE OR REPLACE FUNCTION timeslice(
  tablename text,
  from_time timestamptz,
  to_time timestamptz
)
RETURNS SETOF record
LANGUAGE plpgsql
AS
$func$
BEGIN
  RETURN QUERY EXECUTE format(
    '
      SELECT
        %1$I.*,
        sr_temporal(provenance(), %2$L)
      FROM
        %1$I
      WHERE
        sr_temporal(provenance(), %2$L)
        && tstzrange(%3$L::timestamptz, %4$L::timestamptz)
    ',
    tablename,
    'provsql.time_validity_view',
    from_time::text,
    to_time::text
  );
END;
$func$;
    $sql$;

    EXECUTE $sql$
CREATE OR REPLACE FUNCTION history(
  tablename text,
  col_names text[],
  col_values text[]
)
RETURNS SETOF record
LANGUAGE plpgsql
AS
$func$
DECLARE
    condition text := '';
    i         int;
BEGIN
    IF array_length(col_names, 1) IS NULL
       OR array_length(col_values, 1) IS NULL
       OR array_length(col_names, 1) != array_length(col_values, 1)
    THEN
        RAISE EXCEPTION 'col_names and col_values must have the same (non-null) length';
    END IF;

    FOR i IN 1..array_length(col_names, 1)
    LOOP
        IF i > 1 THEN
            condition := condition || ' AND ';
        END IF;
        condition := condition || format('%I = %L', col_names[i], col_values[i]);
    END LOOP;

    RETURN QUERY EXECUTE format(
      '
        SELECT
          %I.*,
          sr_temporal(provenance(), %L)
        FROM
          %I
        WHERE
          %s
      ',
      tablename,
      'provsql.time_validity_view',
      tablename,
      condition
    );
END;
$func$;
    $sql$;

    EXECUTE $sql$
CREATE OR REPLACE FUNCTION get_valid_time(
  token uuid,
  tablename text
)
RETURNS tstzmultirange
LANGUAGE plpgsql
AS $func$
DECLARE
  result tstzmultirange;
BEGIN
  EXECUTE format(
    '
      SELECT
        sr_temporal(provenance(), %L)
      FROM
        %I
      WHERE
        provsql = %L
    ',
    'provsql.time_validity_view',
    tablename,
    token
  )
  INTO result;

  RETURN result;
END;
$func$;
    $sql$;

  END IF;
END
$$;
