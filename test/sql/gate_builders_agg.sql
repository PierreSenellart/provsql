\set ECHO none
\pset format unaligned

-- provenance_semimod, provenance_semimod_nullable and provenance_aggregate are
-- written in C; they must build the gates the former PL/pgSQL versions built,
-- at the same addresses.  The former bodies are kept here, in schema gba_ref.
--
-- The C function runs first, on values no other test uses, so that it is the
-- one that writes the gates; the reference then runs on the same input.  A
-- value gate and an agg gate record their text write-once: had the C function
-- written another text, the reference would fail with "already records".

CREATE SCHEMA gba_ref;
-- The former bodies annotated the gate after creating it, through set_infos
-- and set_extra, which no longer exist: what a gate records is given when it
-- is created.  Here they are no-ops; the address is what the reference is
-- for, and what the C function recorded is checked directly.
CREATE FUNCTION gba_ref.set_infos(token uuid, info1 int, info2 int DEFAULT NULL) RETURNS void
  LANGUAGE plpgsql AS $$ BEGIN END $$;
CREATE FUNCTION gba_ref.set_extra(token uuid, data text) RETURNS void
  LANGUAGE plpgsql AS $$ BEGIN END $$;

CREATE FUNCTION gba_ref.provenance_aggregate(
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
    PERFORM gba_ref.set_infos(agg_tok, aggfnoid,
                      CASE WHEN is_scalar THEN aggtype | (-2147483648) ELSE aggtype END);
    PERFORM gba_ref.set_extra(agg_tok, agg_val);
  END IF;

  RETURN '( '||agg_tok||' , '||agg_val||' )';
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql,pg_temp,public SECURITY DEFINER IMMUTABLE;

CREATE FUNCTION gba_ref.provenance_semimod(val anyelement, token UUID)
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
  PERFORM gba_ref.set_extra(value_token, CAST(val AS VARCHAR));

  --create semimod gate
  PERFORM create_gate(semimod_token,'semimod',ARRAY[token::uuid,value_token]);

  RETURN semimod_token;
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql,pg_temp,public SECURITY DEFINER IMMUTABLE;

CREATE FUNCTION gba_ref.provenance_semimod_nullable(val anyelement, token UUID)
  RETURNS UUID AS
$$
DECLARE
  semimod_token uuid;
  value_token uuid;
BEGIN
  IF val IS NOT NULL THEN
    RETURN gba_ref.provenance_semimod(val, token);
  END IF;

  value_token := gate_null();
  SELECT uuid_generate_v5(uuid_ns_provsql(),concat('semimod',value_token,token))
    INTO semimod_token;

  PERFORM create_gate(value_token,'value');
  PERFORM gba_ref.set_extra(value_token, 'NULL');

  PERFORM create_gate(semimod_token,'semimod',ARRAY[token::uuid,value_token]);

  RETURN semimod_token;
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql,pg_temp,public SECURITY DEFINER IMMUTABLE;

CREATE DOMAIN gba_dom AS numeric(6,2);
CREATE TYPE gba_enum AS ENUM ('gba small', 'gba large');

CREATE FUNCTION gba_leaf(i int) RETURNS uuid LANGUAGE sql IMMUTABLE AS $$
  SELECT public.uuid_generate_v5(provsql.uuid_ns_provsql(), 'gba-leaf-' || i) $$;
CREATE FUNCTION gba_leaves(i int, n int) RETURNS uuid[] LANGUAGE sql IMMUTABLE AS $$
  SELECT array_agg(gba_leaf(1000 * i + j) ORDER BY j) FROM generate_series(1, n) j $$;

-- What a semimod token designates: its type, its first child, and the type
-- and text of the value gate that is its second child.
CREATE FUNCTION gba_semimod(s uuid, leaf uuid) RETURNS text LANGUAGE sql AS $$
  SELECT CASE WHEN s IS NULL THEN 'no gate' ELSE
    provsql.get_gate_type(s) || ' of '
    || CASE WHEN (provsql.get_children(s))[1] = leaf THEN 'the row token' ELSE 'SOMETHING ELSE' END
    || ' and ' || provsql.get_gate_type((provsql.get_children(s))[2]) || ' "'
    || left(provsql.get_extra((provsql.get_children(s))[2]), 40) || '"' END $$;

-- The cast to varchar is a cast function for some types (boolean, char(n),
-- numbers), the output function for others (date, uuid, arrays, enum, jsonb),
-- nothing for text and varchar.
SELECT label, c IS NOT DISTINCT FROM r AS same_token, gba_semimod(c, l) AS gate
FROM (
  SELECT 0 AS k, 'integer' AS label, gba_leaf(0) AS l,
         provsql.provenance_semimod(918273, gba_leaf(0)) AS c,
         gba_ref.provenance_semimod(918273, gba_leaf(0)) AS r
  UNION ALL
  SELECT 1 AS k, 'bigint' AS label, gba_leaf(1) AS l,
         provsql.provenance_semimod(9182736455463::bigint, gba_leaf(1)) AS c,
         gba_ref.provenance_semimod(9182736455463::bigint, gba_leaf(1)) AS r
  UNION ALL
  SELECT 2 AS k, 'smallint' AS label, gba_leaf(2) AS l,
         provsql.provenance_semimod(9182::smallint, gba_leaf(2)) AS c,
         gba_ref.provenance_semimod(9182::smallint, gba_leaf(2)) AS r
  UNION ALL
  SELECT 3 AS k, 'numeric' AS label, gba_leaf(3) AS l,
         provsql.provenance_semimod(918273.6450, gba_leaf(3)) AS c,
         gba_ref.provenance_semimod(918273.6450, gba_leaf(3)) AS r
  UNION ALL
  SELECT 4 AS k, 'numeric, trailing zeros' AS label, gba_leaf(4) AS l,
         provsql.provenance_semimod(918273.000::numeric(12,3), gba_leaf(4)) AS c,
         gba_ref.provenance_semimod(918273.000::numeric(12,3), gba_leaf(4)) AS r
  UNION ALL
  SELECT 5 AS k, 'double precision' AS label, gba_leaf(5) AS l,
         provsql.provenance_semimod(918273.25e10::float8, gba_leaf(5)) AS c,
         gba_ref.provenance_semimod(918273.25e10::float8, gba_leaf(5)) AS r
  UNION ALL
  SELECT 6 AS k, 'real' AS label, gba_leaf(6) AS l,
         provsql.provenance_semimod(9182.5::real, gba_leaf(6)) AS c,
         gba_ref.provenance_semimod(9182.5::real, gba_leaf(6)) AS r
  UNION ALL
  SELECT 7 AS k, 'boolean' AS label, gba_leaf(7) AS l,
         provsql.provenance_semimod((918273 > 0), gba_leaf(7)) AS c,
         gba_ref.provenance_semimod((918273 > 0), gba_leaf(7)) AS r
  UNION ALL
  SELECT 8 AS k, 'text' AS label, gba_leaf(8) AS l,
         provsql.provenance_semimod('gba text 918273'::text, gba_leaf(8)) AS c,
         gba_ref.provenance_semimod('gba text 918273'::text, gba_leaf(8)) AS r
  UNION ALL
  SELECT 9 AS k, 'varchar' AS label, gba_leaf(9) AS l,
         provsql.provenance_semimod('gba varchar 918273'::varchar, gba_leaf(9)) AS c,
         gba_ref.provenance_semimod('gba varchar 918273'::varchar, gba_leaf(9)) AS r
  UNION ALL
  SELECT 10 AS k, 'varchar(n)' AS label, gba_leaf(10) AS l,
         provsql.provenance_semimod('gba vc'::varchar(10), gba_leaf(10)) AS c,
         gba_ref.provenance_semimod('gba vc'::varchar(10), gba_leaf(10)) AS r
  UNION ALL
  SELECT 11 AS k, 'char(n), padded' AS label, gba_leaf(11) AS l,
         provsql.provenance_semimod('gba ch'::char(12), gba_leaf(11)) AS c,
         gba_ref.provenance_semimod('gba ch'::char(12), gba_leaf(11)) AS r
  UNION ALL
  SELECT 12 AS k, 'name' AS label, gba_leaf(12) AS l,
         provsql.provenance_semimod('gba_name'::name, gba_leaf(12)) AS c,
         gba_ref.provenance_semimod('gba_name'::name, gba_leaf(12)) AS r
  UNION ALL
  SELECT 13 AS k, 'date' AS label, gba_leaf(13) AS l,
         provsql.provenance_semimod(DATE '2026-09-18', gba_leaf(13)) AS c,
         gba_ref.provenance_semimod(DATE '2026-09-18', gba_leaf(13)) AS r
  UNION ALL
  SELECT 14 AS k, 'timestamp' AS label, gba_leaf(14) AS l,
         provsql.provenance_semimod(TIMESTAMP '2026-09-18 01:02:03.5', gba_leaf(14)) AS c,
         gba_ref.provenance_semimod(TIMESTAMP '2026-09-18 01:02:03.5', gba_leaf(14)) AS r
  UNION ALL
  SELECT 15 AS k, 'interval' AS label, gba_leaf(15) AS l,
         provsql.provenance_semimod(INTERVAL '918 days 2 hours', gba_leaf(15)) AS c,
         gba_ref.provenance_semimod(INTERVAL '918 days 2 hours', gba_leaf(15)) AS r
  UNION ALL
  SELECT 16 AS k, 'uuid' AS label, gba_leaf(16) AS l,
         provsql.provenance_semimod('00000000-0000-4000-8000-000000918273'::uuid, gba_leaf(16)) AS c,
         gba_ref.provenance_semimod('00000000-0000-4000-8000-000000918273'::uuid, gba_leaf(16)) AS r
  UNION ALL
  SELECT 17 AS k, 'integer[]' AS label, gba_leaf(17) AS l,
         provsql.provenance_semimod(ARRAY[9,1,8,2,7,3], gba_leaf(17)) AS c,
         gba_ref.provenance_semimod(ARRAY[9,1,8,2,7,3], gba_leaf(17)) AS r
  UNION ALL
  SELECT 18 AS k, 'text[] with quotes' AS label, gba_leaf(18) AS l,
         provsql.provenance_semimod(ARRAY['gba a','gba "b"',NULL], gba_leaf(18)) AS c,
         gba_ref.provenance_semimod(ARRAY['gba a','gba "b"',NULL], gba_leaf(18)) AS r
  UNION ALL
  SELECT 19 AS k, 'enum' AS label, gba_leaf(19) AS l,
         provsql.provenance_semimod('gba large'::gba_enum, gba_leaf(19)) AS c,
         gba_ref.provenance_semimod('gba large'::gba_enum, gba_leaf(19)) AS r
  UNION ALL
  SELECT 20 AS k, 'domain' AS label, gba_leaf(20) AS l,
         provsql.provenance_semimod(918.27::gba_dom, gba_leaf(20)) AS c,
         gba_ref.provenance_semimod(918.27::gba_dom, gba_leaf(20)) AS r
  UNION ALL
  SELECT 21 AS k, 'jsonb' AS label, gba_leaf(21) AS l,
         provsql.provenance_semimod('{"gba": [918, 273]}'::jsonb, gba_leaf(21)) AS c,
         gba_ref.provenance_semimod('{"gba": [918, 273]}'::jsonb, gba_leaf(21)) AS r
  UNION ALL
  SELECT 22 AS k, 'text reading NULL' AS label, gba_leaf(22) AS l,
         provsql.provenance_semimod('NULL'::text, gba_leaf(22)) AS c,
         gba_ref.provenance_semimod('NULL'::text, gba_leaf(22)) AS r
  UNION ALL
  SELECT 23 AS k, 'empty text' AS label, gba_leaf(23) AS l,
         provsql.provenance_semimod(''::text, gba_leaf(23)) AS c,
         gba_ref.provenance_semimod(''::text, gba_leaf(23)) AS r
  UNION ALL
  SELECT 24 AS k, 'long text' AS label, gba_leaf(24) AS l,
         provsql.provenance_semimod(repeat('gba long 918273 ', 12), gba_leaf(24)) AS c,
         gba_ref.provenance_semimod(repeat('gba long 918273 ', 12), gba_leaf(24)) AS r
) t ORDER BY k;

-- A NULL value: no gate for provenance_semimod, a gate over gate_null() for
-- the nullable variant, which is provenance_semimod on anything else.
SELECT provsql.provenance_semimod(NULL::int, gba_leaf(50)) IS NULL AS c_null,
       gba_ref.provenance_semimod(NULL::int, gba_leaf(50)) IS NULL AS ref_null;
SELECT label, c IS NOT DISTINCT FROM r AS same_token, gba_semimod(c, l) AS gate,
       (provsql.get_children(c))[2] = provsql.gate_null() AS over_gate_null
FROM (
  SELECT 'nullable, NULL' AS label, gba_leaf(51) AS l,
         provsql.provenance_semimod_nullable(NULL::text, gba_leaf(51)) AS c,
         gba_ref.provenance_semimod_nullable(NULL::text, gba_leaf(51)) AS r
  UNION ALL
  SELECT 'nullable, text reading NULL', gba_leaf(51),
         provsql.provenance_semimod_nullable('NULL'::text, gba_leaf(51)),
         gba_ref.provenance_semimod_nullable('NULL'::text, gba_leaf(51))
  UNION ALL
  SELECT 'nullable, a number', gba_leaf(52),
         provsql.provenance_semimod_nullable(564738, gba_leaf(52)),
         gba_ref.provenance_semimod_nullable(564738, gba_leaf(52))
) t ORDER BY label;
-- Same value under the two functions, same gate.
SELECT provsql.provenance_semimod(564738, gba_leaf(52)) = provsql.provenance_semimod_nullable(564738, gba_leaf(52)) AS same;

-- A NULL row token is refused, as it was (by create_gate).
SELECT provsql.provenance_semimod(1, NULL);

-- The value gates written by this session are remembered; a value seen again,
-- under another row token, gives another semimod gate over the same value gate.
SELECT a <> b AS different_semimod,
       (provsql.get_children(a))[2] = (provsql.get_children(b))[2] AS same_value_gate
FROM (SELECT provsql.provenance_semimod(918273, gba_leaf(60)) AS a,
             provsql.provenance_semimod(918273, gba_leaf(61)) AS b) t;

-- provenance_aggregate: the agg_token (gate and value, the value cut to what
-- an agg_token holds), and the gate: children, infos, text.  The address now
-- hashes the result type and the value too, so the token differs from the
-- reference's; the reference is still run, on the new gate, to check that
-- what it would write is what the C function wrote.
SELECT label, c::uuid <> r::uuid AS token_differs,
       provsql.agg_token_out(c)::text = provsql.agg_token_out(r)::text AS same_value,
       provsql.get_gate_type(c::uuid) AS type,
       array_length(provsql.get_children(c::uuid), 1) AS children,
       (provsql.get_infos(c::uuid)).info1 AS info1, (provsql.get_infos(c::uuid)).info2 AS info2,
       left(provsql.get_extra(c::uuid), 40) AS extra,
       provsql.agg_token_out(c)::text = left(provsql.get_extra(c::uuid), 79) || ' (*)' AS value_is_cut_extra
FROM (
  SELECT 0 AS k, 'integer' AS label,
         provsql.provenance_aggregate(2108, 23, 918273, gba_leaves(100, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 918273, gba_leaves(100, 3)) AS r
  UNION ALL
  SELECT 1 AS k, 'bigint' AS label,
         provsql.provenance_aggregate(2108, 23, 9182736455463::bigint, gba_leaves(101, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 9182736455463::bigint, gba_leaves(101, 3)) AS r
  UNION ALL
  SELECT 2 AS k, 'smallint' AS label,
         provsql.provenance_aggregate(2108, 23, 9182::smallint, gba_leaves(102, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 9182::smallint, gba_leaves(102, 3)) AS r
  UNION ALL
  SELECT 3 AS k, 'numeric' AS label,
         provsql.provenance_aggregate(2108, 23, 918273.6450, gba_leaves(103, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 918273.6450, gba_leaves(103, 3)) AS r
  UNION ALL
  SELECT 4 AS k, 'numeric, trailing zeros' AS label,
         provsql.provenance_aggregate(2108, 23, 918273.000::numeric(12,3), gba_leaves(104, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 918273.000::numeric(12,3), gba_leaves(104, 3)) AS r
  UNION ALL
  SELECT 5 AS k, 'double precision' AS label,
         provsql.provenance_aggregate(2108, 23, 918273.25e10::float8, gba_leaves(105, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 918273.25e10::float8, gba_leaves(105, 3)) AS r
  UNION ALL
  SELECT 6 AS k, 'real' AS label,
         provsql.provenance_aggregate(2108, 23, 9182.5::real, gba_leaves(106, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 9182.5::real, gba_leaves(106, 3)) AS r
  UNION ALL
  SELECT 7 AS k, 'boolean' AS label,
         provsql.provenance_aggregate(2108, 23, (918273 > 0), gba_leaves(107, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, (918273 > 0), gba_leaves(107, 3)) AS r
  UNION ALL
  SELECT 8 AS k, 'text' AS label,
         provsql.provenance_aggregate(2108, 23, 'gba text 918273'::text, gba_leaves(108, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 'gba text 918273'::text, gba_leaves(108, 3)) AS r
  UNION ALL
  SELECT 9 AS k, 'varchar' AS label,
         provsql.provenance_aggregate(2108, 23, 'gba varchar 918273'::varchar, gba_leaves(109, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 'gba varchar 918273'::varchar, gba_leaves(109, 3)) AS r
  UNION ALL
  SELECT 10 AS k, 'varchar(n)' AS label,
         provsql.provenance_aggregate(2108, 23, 'gba vc'::varchar(10), gba_leaves(110, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 'gba vc'::varchar(10), gba_leaves(110, 3)) AS r
  UNION ALL
  SELECT 11 AS k, 'char(n), padded' AS label,
         provsql.provenance_aggregate(2108, 23, 'gba ch'::char(12), gba_leaves(111, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 'gba ch'::char(12), gba_leaves(111, 3)) AS r
  UNION ALL
  SELECT 12 AS k, 'name' AS label,
         provsql.provenance_aggregate(2108, 23, 'gba_name'::name, gba_leaves(112, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 'gba_name'::name, gba_leaves(112, 3)) AS r
  UNION ALL
  SELECT 13 AS k, 'date' AS label,
         provsql.provenance_aggregate(2108, 23, DATE '2026-09-18', gba_leaves(113, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, DATE '2026-09-18', gba_leaves(113, 3)) AS r
  UNION ALL
  SELECT 14 AS k, 'timestamp' AS label,
         provsql.provenance_aggregate(2108, 23, TIMESTAMP '2026-09-18 01:02:03.5', gba_leaves(114, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, TIMESTAMP '2026-09-18 01:02:03.5', gba_leaves(114, 3)) AS r
  UNION ALL
  SELECT 15 AS k, 'interval' AS label,
         provsql.provenance_aggregate(2108, 23, INTERVAL '918 days 2 hours', gba_leaves(115, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, INTERVAL '918 days 2 hours', gba_leaves(115, 3)) AS r
  UNION ALL
  SELECT 16 AS k, 'uuid' AS label,
         provsql.provenance_aggregate(2108, 23, '00000000-0000-4000-8000-000000918273'::uuid, gba_leaves(116, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, '00000000-0000-4000-8000-000000918273'::uuid, gba_leaves(116, 3)) AS r
  UNION ALL
  SELECT 17 AS k, 'integer[]' AS label,
         provsql.provenance_aggregate(2108, 23, ARRAY[9,1,8,2,7,3], gba_leaves(117, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, ARRAY[9,1,8,2,7,3], gba_leaves(117, 3)) AS r
  UNION ALL
  SELECT 18 AS k, 'text[] with quotes' AS label,
         provsql.provenance_aggregate(2108, 23, ARRAY['gba a','gba "b"',NULL], gba_leaves(118, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, ARRAY['gba a','gba "b"',NULL], gba_leaves(118, 3)) AS r
  UNION ALL
  SELECT 19 AS k, 'enum' AS label,
         provsql.provenance_aggregate(2108, 23, 'gba large'::gba_enum, gba_leaves(119, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 'gba large'::gba_enum, gba_leaves(119, 3)) AS r
  UNION ALL
  SELECT 20 AS k, 'domain' AS label,
         provsql.provenance_aggregate(2108, 23, 918.27::gba_dom, gba_leaves(120, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 918.27::gba_dom, gba_leaves(120, 3)) AS r
  UNION ALL
  SELECT 21 AS k, 'jsonb' AS label,
         provsql.provenance_aggregate(2108, 23, '{"gba": [918, 273]}'::jsonb, gba_leaves(121, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, '{"gba": [918, 273]}'::jsonb, gba_leaves(121, 3)) AS r
  UNION ALL
  SELECT 22 AS k, 'text reading NULL' AS label,
         provsql.provenance_aggregate(2108, 23, 'NULL'::text, gba_leaves(122, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, 'NULL'::text, gba_leaves(122, 3)) AS r
  UNION ALL
  SELECT 23 AS k, 'empty text' AS label,
         provsql.provenance_aggregate(2108, 23, ''::text, gba_leaves(123, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, ''::text, gba_leaves(123, 3)) AS r
  UNION ALL
  SELECT 24 AS k, 'long text' AS label,
         provsql.provenance_aggregate(2108, 23, repeat('gba long 918273 ', 12), gba_leaves(124, 3)) AS c,
         gba_ref.provenance_aggregate(2108, 23, repeat('gba long 918273 ', 12), gba_leaves(124, 3)) AS r
) t ORDER BY k;

-- NULL children are rows whose value was NULL; the order of the others is
-- kept; no child is gate_zero; the scalar flag and the aggregate function
-- change the address, and the flag is the high bit of info2.
SELECT label, provsql.get_gate_type(c::uuid) AS type,
       provsql.get_children(c::uuid) = kept AS children_as_given,
       (provsql.get_infos(c::uuid)).info2 AS info2
FROM (
  SELECT 1 AS k, 'NULL children dropped' AS label, ARRAY[gba_leaf(71), gba_leaf(70)] AS kept,
         provsql.provenance_aggregate(2108, 23, 717071, ARRAY[NULL, gba_leaf(71), NULL, gba_leaf(70)]) AS c,
         gba_ref.provenance_aggregate(2108, 23, 717071, ARRAY[NULL, gba_leaf(71), NULL, gba_leaf(70)]) AS r
  UNION ALL
  SELECT 2, 'scalar', ARRAY[gba_leaf(71), gba_leaf(70)],
         provsql.provenance_aggregate(2108, 23, 717071, ARRAY[gba_leaf(71), gba_leaf(70)], true),
         gba_ref.provenance_aggregate(2108, 23, 717071, ARRAY[gba_leaf(71), gba_leaf(70)], true)
  UNION ALL
  SELECT 3, 'other aggregate function', ARRAY[gba_leaf(71), gba_leaf(70)],
         provsql.provenance_aggregate(2101, 1700, 717071, ARRAY[gba_leaf(71), gba_leaf(70)]),
         gba_ref.provenance_aggregate(2101, 1700, 717071, ARRAY[gba_leaf(71), gba_leaf(70)])
) t ORDER BY k;
SELECT count(DISTINCT c::uuid) AS distinct_gates FROM (
  SELECT provsql.provenance_aggregate(2108, 23, 717071, ARRAY[gba_leaf(71), gba_leaf(70)]) AS c
  UNION ALL SELECT provsql.provenance_aggregate(2108, 23, 717071, ARRAY[gba_leaf(71), gba_leaf(70)], true)
  UNION ALL SELECT provsql.provenance_aggregate(2101, 1700, 717071, ARRAY[gba_leaf(71), gba_leaf(70)])) t;
SELECT label, c::uuid = r::uuid AS same_token, c::uuid = provsql.gate_zero() AS is_zero
FROM (
  SELECT 'no child' AS label,
         provsql.provenance_aggregate(2108, 23, 0, '{}'::uuid[]) AS c,
         gba_ref.provenance_aggregate(2108, 23, 0, '{}'::uuid[]) AS r
  UNION ALL
  SELECT 'NULL children only',
         provsql.provenance_aggregate(2108, 23, 0, ARRAY[NULL::uuid]),
         gba_ref.provenance_aggregate(2108, 23, 0, ARRAY[NULL::uuid])
  UNION ALL
  SELECT 'NULL array',
         provsql.provenance_aggregate(2108, 23, 0, NULL::uuid[]),
         gba_ref.provenance_aggregate(2108, 23, 0, NULL::uuid[])
) t ORDER BY label;
-- A NULL value over rows (none of them present in the database as it is:
-- the group exists in other worlds) keeps its token, with a NULL value;
-- the reference implementation returned a NULL agg_token.
SELECT provsql.provenance_aggregate(2108, 23, NULL::int, ARRAY[gba_leaf(80), gba_leaf(81)]) IS NOT NULL AS c_token,
       provsql.agg_token_value(provsql.provenance_aggregate(2108, 23, NULL::int, ARRAY[gba_leaf(80), gba_leaf(81)])) IS NULL AS c_value_null,
       gba_ref.provenance_aggregate(2108, 23, NULL::int, ARRAY[gba_leaf(80), gba_leaf(81)]) IS NULL AS ref_null;

-- Two aggregations that record different things over the same children are
-- two gates.  With the former address they were one, and the second failed
-- with "already records": array_agg (2335) over integers and over their
-- texts has the same children (a value gate is addressed by its text) but
-- result types int[] (1007) and text[] (1009); min over int[] and over
-- text[] (2135) has different values ('{10}' < '{9}' as text[]).  A value
-- NULL and a value '' differ too.
SELECT label, count(DISTINCT a::uuid) AS gates,
       array_agg((provsql.get_infos(a::uuid)).info2 ORDER BY (provsql.get_infos(a::uuid)).info2) AS types,
       array_agg(provsql.get_extra(a::uuid) ORDER BY provsql.get_extra(a::uuid)) AS values
FROM (
  SELECT 'array_agg int / text' AS label,
         provsql.provenance_aggregate(2335, 1007, ARRAY[1, 2], gba_leaves(90, 2)) AS a
  UNION ALL
  SELECT 'array_agg int / text',
         provsql.provenance_aggregate(2335, 1009, ARRAY['1', '2'], gba_leaves(90, 2))
  UNION ALL
  SELECT 'min int[] / text[]',
         provsql.provenance_aggregate(2135, 1007, ARRAY[9], gba_leaves(91, 2))
  UNION ALL
  SELECT 'min int[] / text[]',
         provsql.provenance_aggregate(2135, 1009, ARRAY['10'], gba_leaves(91, 2))
) t GROUP BY label ORDER BY label;
SELECT provsql.provenance_aggregate(2108, 23, NULL::text, ARRAY[gba_leaf(93)]) IS NOT NULL AS null_value_keeps_token,
       provsql.provenance_aggregate(2108, 25, ''::text, ARRAY[gba_leaf(93)])::uuid
         <> provsql.provenance_aggregate(2108, 25, ':'::text, ARRAY[gba_leaf(93)])::uuid AS empty_and_colon_differ;

-- A gate is created together with what it records, without waiting for the
-- worker's answer, since the address determines the infos and the text.  If
-- something else was recorded at that address by hand, the first value stays
-- (the worker logs it), and the query goes on.
SELECT provsql.create_gate(public.uuid_generate_v5(provsql.uuid_ns_provsql(), 'valuegba by hand'), 'value',
                           NULL, NULL, NULL, 'something else');
SELECT provsql.get_extra((provsql.get_children(provsql.provenance_semimod('gba by hand'::text, gba_leaf(95))))[2]) AS first_value_stays;

DROP FUNCTION gba_leaf(int); DROP FUNCTION gba_leaves(int, int); DROP FUNCTION gba_semimod(uuid, uuid);
SET client_min_messages = warning;
DROP SCHEMA gba_ref CASCADE;
DROP TYPE gba_enum; DROP DOMAIN gba_dom;
