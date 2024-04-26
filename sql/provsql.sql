-- Requires extensions "uuid-ossp"

CREATE SCHEMA provsql;

SET search_path TO provsql;

-- Create agg_token type for aggregation display
CREATE TYPE agg_token;

CREATE OR REPLACE FUNCTION agg_token_in(cstring)
  RETURNS agg_token
  AS 'provsql','agg_token_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_token_out(agg_token)
  RETURNS cstring
  AS 'provsql','agg_token_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_token_cast(agg_token)
  RETURNS text
  AS 'provsql','agg_token_cast' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE agg_token (
  internallength = 117,
  input = agg_token_in,
  output = agg_token_out,
  alignment = char
);

CREATE OR REPLACE FUNCTION agg_token_uuid(aggtok agg_token)
  RETURNS uuid AS
$$
BEGIN
  RETURN agg_token_cast(aggtok)::uuid;
END
$$ LANGUAGE plpgsql STRICT SET search_path=provsql,pg_temp,public SECURITY DEFINER IMMUTABLE PARALLEL SAFE;

CREATE CAST (agg_token AS UUID) WITH FUNCTION agg_token_uuid(agg_token) AS IMPLICIT;

CREATE TYPE provenance_gate AS
  ENUM(
    'input',   -- Input (variable) gate of the circuit
    'plus',    -- Semiring plus
    'times',   -- Semiring times
    'monus',   -- M-Semiring monus
    'project', -- Project gate (for where provenance)
    'zero',    -- Semiring zero
    'one',     -- Semiring one
    'eq',      -- Equijoin gate (for where provenance)
    'agg',     -- Aggregation operator (for aggregate provenance)
    'semimod', -- Semimodule scalar multiplication (for aggregate provenance)
    'cmp',     -- Currently unused, meant for comparison of aggregate values
    'delta',   -- δ-semiring operator (see Amsterdamer, Deutch, Tannen, PODS 2011)
    'value',   -- Scalar value (for aggregate provenance)
    'mulinput' -- Multivalued input (for Boolean provenance)
    );

CREATE OR REPLACE FUNCTION create_gate(
  token UUID,
  type provenance_gate,
  children uuid[] DEFAULT NULL)
  RETURNS void AS
  'provsql','create_gate' LANGUAGE C PARALLEL SAFE;
CREATE OR REPLACE FUNCTION get_gate_type(
  token UUID)
  RETURNS provenance_gate AS
  'provsql','get_gate_type' LANGUAGE C IMMUTABLE PARALLEL SAFE;
CREATE OR REPLACE FUNCTION get_children(
  token UUID)
  RETURNS uuid[] AS
  'provsql','get_children' LANGUAGE C IMMUTABLE PARALLEL SAFE;
CREATE OR REPLACE FUNCTION set_prob(
  token UUID, p DOUBLE PRECISION)
  RETURNS void AS
  'provsql','set_prob' LANGUAGE C PARALLEL SAFE;
CREATE OR REPLACE FUNCTION get_prob(
  token UUID)
  RETURNS DOUBLE PRECISION AS
  'provsql','get_prob' LANGUAGE C STABLE PARALLEL SAFE;

-- Two integer values associated to gate, used in different ways by
-- different gate types:
-- * for mulinput, info1 indicates the value of this multivalued variable
-- * for eq, info1 and info2 indicate the attribute index of the
--   equijoin in, respectively, the first and second columns
-- * for agg, info1 is the oid of the aggregate function and info2 the
--   oid of the aggregate result type
CREATE OR REPLACE FUNCTION set_infos(
  token UUID, info1 INT, info2 INT DEFAULT NULL)
  RETURNS void AS
  'provsql','set_infos' LANGUAGE C PARALLEL SAFE;
CREATE OR REPLACE FUNCTION get_infos(
  token UUID, OUT info1 INT, OUT info2 INT)
  RETURNS record AS
  'provsql','get_infos' LANGUAGE C STABLE PARALLEL SAFE;

-- Extra arbitrary text-encoded information associated to gate, used in
-- different ways by different gate types:
-- * for project, it is a text-encoded ARRAY of two-element ARRAYs that
--   indicate mappings between input attribute (first element) and output
--   attribute (second element)
-- * for value and agg, it is the text-encoded (base for value, computed
--   for agg) scalar value
CREATE OR REPLACE FUNCTION set_extra(
  token UUID, data TEXT)
  RETURNS void AS
  'provsql','set_extra' LANGUAGE C PARALLEL SAFE STRICT;
CREATE OR REPLACE FUNCTION get_extra(token UUID)
  RETURNS TEXT AS
  'provsql','get_extra' LANGUAGE C STABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT;

CREATE OR REPLACE FUNCTION get_nb_gates() RETURNS BIGINT AS
  'provsql', 'get_nb_gates' LANGUAGE C PARALLEL SAFE;

CREATE OR REPLACE FUNCTION add_gate_trigger()
  RETURNS TRIGGER AS
$$
DECLARE
  attribute RECORD;
BEGIN
  PERFORM create_gate(NEW.provsql, 'input');
  RETURN NEW;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp SECURITY DEFINER;

CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$$
BEGIN
  EXECUTE format('ALTER TABLE %I ADD COLUMN provsql UUID UNIQUE DEFAULT public.uuid_generate_v4()', _tbl);
  EXECUTE format('SELECT provsql.create_gate(provsql, ''input'') FROM %I', _tbl);
  EXECUTE format('CREATE TRIGGER add_gate BEFORE INSERT ON %I FOR EACH ROW EXECUTE PROCEDURE provsql.add_gate_trigger()',_tbl);
END
$$ LANGUAGE plpgsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION remove_provenance(_tbl regclass)
  RETURNS void AS
$$
DECLARE
BEGIN
  EXECUTE format('ALTER TABLE %I DROP COLUMN provsql', _tbl);
  BEGIN
    EXECUTE format('DROP TRIGGER add_gate on %I', _tbl);
  EXCEPTION WHEN undefined_object THEN
  END;
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION repair_key(_tbl regclass, key_att text)
  RETURNS void AS
$$
DECLARE
  key RECORD;
  key_token uuid;
  token uuid;
  record RECORD;
  nb_rows INTEGER;
  ind INTEGER;
  select_key_att TEXT;
  where_condition TEXT;
BEGIN
  IF key_att = '' THEN
    key_att := '()';
    select_key_att := '1';
  ELSE
    select_key_att := key_att;
  END IF;

  EXECUTE format('ALTER TABLE %I ADD COLUMN provsql_temp UUID UNIQUE DEFAULT public.uuid_generate_v4()', _tbl);

  FOR key IN
    EXECUTE format('SELECT %s AS key FROM %I GROUP BY %s', select_key_att, _tbl, key_att)
  LOOP
    IF key_att = '()' THEN
      where_condition := '';
    ELSE
      where_condition := format('WHERE %s = %L', key_att, key.key);
    END IF;

    EXECUTE format('SELECT COUNT(*) FROM %I %s', _tbl, where_condition) INTO nb_rows;

    key_token := public.uuid_generate_v4();
    ind := 1;
    FOR record IN
      EXECUTE format('SELECT provsql_temp FROM %I %s', _tbl, where_condition)
    LOOP
      token:=record.provsql_temp;
      PERFORM provsql.create_gate(token, 'mulinput', ARRAY[key_token]);
      PERFORM provsql.set_prob(token, 1./nb_rows);
      PERFORM provsql.set_infos(token, ind);
      ind := ind + 1;
    END LOOP;
  END LOOP;
  EXECUTE format('ALTER TABLE %I RENAME COLUMN provsql_temp TO provsql', _tbl);
  EXECUTE format('CREATE TRIGGER add_gate BEFORE INSERT ON %I FOR EACH ROW EXECUTE PROCEDURE provsql.add_gate_trigger()',_tbl);
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION create_provenance_mapping(newtbl text, oldtbl regclass, att text)
  RETURNS void AS
$$
DECLARE
BEGIN
  EXECUTE format('CREATE TEMP TABLE tmp_provsql ON COMMIT DROP AS TABLE %I', oldtbl);
  ALTER TABLE tmp_provsql RENAME provsql TO provenance;
  EXECUTE format('CREATE TABLE %I AS SELECT %s AS value, provenance FROM tmp_provsql', newtbl, att);
  EXECUTE format('CREATE INDEX ON %I(provenance)', newtbl);
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION uuid_ns_provsql() RETURNS uuid AS
$$
 -- uuid_generate_v5(uuid_ns_url(),'http://pierre.senellart.com/software/provsql/')
 SELECT '920d4f02-8718-5319-9532-d4ab83a64489'::uuid
$$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION gate_zero() RETURNS uuid AS
$$
  SELECT public.uuid_generate_v5(provsql.uuid_ns_provsql(),'zero');
$$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION gate_one() RETURNS uuid AS
$$
  SELECT public.uuid_generate_v5(provsql.uuid_ns_provsql(),'one');
$$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION provenance_times(VARIADIC tokens uuid[])
  RETURNS UUID AS
$$
DECLARE
  times_token uuid;
BEGIN
  CASE array_length(tokens, 1)
    WHEN 0 THEN
      times_token:=gate_one();
    WHEN 1 THEN
      times_token:=tokens[1];
    ELSE
      times_token := uuid_generate_v5(uuid_ns_provsql(),concat('times',tokens));

      PERFORM create_gate(times_token, 'times', tokens);
  END CASE;

  RETURN times_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE;

CREATE OR REPLACE FUNCTION provenance_monus(token1 UUID, token2 UUID)
  RETURNS UUID AS
$$
DECLARE
  monus_token uuid;
BEGIN
  IF token2 IS NULL THEN
    -- Special semantics, because of a LEFT OUTER JOIN used by the
    -- difference operator: token2 NULL means there is no second argument
    RETURN token1;
  END IF;

  IF token1 = token2 THEN
    -- X-X=0
    monus_token:=gate_zero();
  ELSIF token1 = gate_zero() THEN
    -- 0-X=0
    monus_token:=gate_zero();
  ELSIF token2 = gate_zero() THEN
    -- X-0=X
    monus_token:=token1;
  ELSE
    monus_token:=uuid_generate_v5(uuid_ns_provsql(),concat('monus',token1,token2));
    PERFORM create_gate(monus_token, 'monus', ARRAY[token1::uuid, token2::uuid]);
  END IF;

  RETURN monus_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE;

CREATE OR REPLACE FUNCTION provenance_project(token UUID, VARIADIC positions int[])
  RETURNS UUID AS
$$
DECLARE
  project_token uuid;
  rec record;
BEGIN
  project_token:=uuid_generate_v5(uuid_ns_provsql(),concat('project', token, positions));
  PERFORM create_gate(project_token, 'project', ARRAY[token]);
  PERFORM set_extra(project_token, ARRAY_AGG(pair)::text)
  FROM (
    SELECT ARRAY[(CASE WHEN info=0 THEN NULL ELSE info END), idx] AS pair
    FROM unnest(positions) WITH ORDINALITY AS a(info, idx)
    ORDER BY idx
  ) t;

  RETURN project_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE OR REPLACE FUNCTION provenance_eq(token UUID, pos1 int, pos2 int)
  RETURNS UUID AS
$$
DECLARE
  eq_token uuid;
  rec record;
BEGIN
  eq_token:=uuid_generate_v5(uuid_ns_provsql(),concat('project',token,pos1,',',pos2));

  PERFORM create_gate(eq_token, 'eq', ARRAY[token::uuid]);
  PERFORM set_infos(eq_token, pos1, pos2);
  RETURN eq_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE OR REPLACE FUNCTION provenance_plus(tokens uuid[])
  RETURNS UUID AS
$$
DECLARE
  c INTEGER;
  plus_token uuid;
  filtered_tokens uuid[];
BEGIN
  c:=array_length(tokens, 1);

  IF c = 0 THEN
    plus_token := gate_zero();
  ELSIF c = 1 THEN
    plus_token := tokens[1];
  ELSE
    plus_token := uuid_generate_v5(
      uuid_ns_provsql(),
      concat('plus', tokens));

    PERFORM create_gate(plus_token, 'plus', tokens);
  END IF;

  RETURN plus_token;
END
$$ LANGUAGE plpgsql STRICT SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE;

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
BEGIN
  SELECT get_gate_type(token) INTO gate_type;

  IF gate_type IS NULL THEN
    RETURN NULL;
  ELSIF gate_type='input' THEN
    EXECUTE format('SELECT * FROM %I WHERE provenance=%L',token2value,token) INTO result;
    IF result IS NULL THEN
      result:=element_one;
    END IF;
  ELSIF gate_type='mulinput' THEN
    SELECT concat('{',(get_children(token))[1]::text,'=',(get_infos(token)).info1,'}') INTO result;
  ELSIF gate_type='plus' THEN
    EXECUTE format('SELECT %I(provsql.provenance_evaluate(t,%L,%L::%s,%L,%L,%L,%L,%L)) FROM unnest(get_children(%L)) AS t',
      plus_function,token2value,element_one,value_type,value_type,plus_function,times_function,monus_function,delta_function,token)
    INTO result;
  ELSIF gate_type='times' THEN
    EXECUTE format('SELECT %I(provsql.provenance_evaluate(t,%L,%L::%s,%L,%L,%L,%L,%L)) FROM unnest(get_children(%L)) AS t',
      times_function,token2value,element_one,value_type,value_type,plus_function,times_function,monus_function,delta_function,token)
    INTO result;
  ELSIF gate_type='monus' THEN
    IF monus_function IS NULL THEN
      RAISE EXCEPTION USING MESSAGE='Provenance with negation evaluated over a semiring without monus function';
    ELSE
      EXECUTE format('SELECT %I(a[1],a[2]) FROM (SELECT array_agg(provsql.provenance_evaluate(t,%L,%L::%s,%L,%L,%L,%L,%L)) AS a FROM unnest(get_children(%L)) AS t) tmp',
        monus_function,token2value,element_one,value_type,value_type,plus_function,times_function,monus_function,delta_function,token)
      INTO result;
    END IF;
  ELSIF gate_type='eq' THEN
    EXECUTE format('SELECT provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L)',
      token,token2value,element_one,value_type,value_type,plus_function,times_function,monus_function,delta_function)
    INTO result;
  ELSIF gate_type='delta' THEN
    IF delta_function IS NULL THEN
      RAISE EXCEPTION USING MESSAGE='Provenance with aggregation evaluated over a semiring without delta function';
    ELSE
      EXECUTE format('SELECT %I(a) FROM (SELECT provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L) AS a) tmp',
        delta_function,token,token2value,element_one,value_type,value_type,plus_function,times_function,monus_function,delta_function)
      INTO result;
    END IF;
  ELSIF gate_type='zero' THEN
    EXECUTE format('SELECT %I(a) FROM (SELECT %L::%I AS a WHERE FALSE) temp',plus_function,element_one,value_type) INTO result;
  ELSIF gate_type='one' THEN
    EXECUTE format('SELECT %L::%I',element_one,value_type) INTO result;
  ELSIF gate_type='project' THEN
    EXECUTE format('SELECT provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L)',
      token,token2value,element_one,value_type,value_type,plus_function,times_function,monus_function,delta_function)
    INTO result;
  ELSE
    RAISE EXCEPTION USING MESSAGE='Unknown gate type';
  END IF;
  RETURN result;
END
$$ LANGUAGE plpgsql PARALLEL SAFE;

CREATE OR REPLACE FUNCTION aggregation_evaluate(
  token UUID,
  token2value regclass,
  agg_function_final regproc,
  agg_function regproc,
  semimod_function regproc,
  element_one anyelement,
  value_type regtype,
  plus_function regproc,
  times_function regproc,
  monus_function regproc,
  delta_function regproc)
  RETURNS anyelement AS
$$
DECLARE
  gt provenance_gate;
  result ALIAS FOR $0;
BEGIN
  SELECT get_gate_type(token) INTO gt;

  IF gt IS NULL THEN
    RETURN NULL;
  ELSIF gt='agg' THEN
    EXECUTE format('SELECT %I(%I(provsql.aggregation_evaluate(t,%L,%L,%L,%L,%L::%s,%L,%L,%L,%L,%L)),pp.proname::varchar) FROM
                    unnest(get_children(%L)) AS t, pg_proc pp
                    WHERE pp.oid=(get_infos(%L)).info1
                    GROUP BY pp.proname',
      agg_function_final, agg_function,token2value,agg_function_final,agg_function,semimod_function,element_one,value_type,value_type,plus_function,times_function,
      monus_function,delta_function,token,token)
    INTO result;
  ELSE
    -- gt='semimod'
    EXECUTE format('SELECT %I(get_extra((get_children(%L))[2]),provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L))',
      semimod_function,token,token,token2value,element_one,value_type,value_type,plus_function,times_function,monus_function,delta_function)
    INTO result;
  END IF;
  RETURN result;
END
$$ LANGUAGE plpgsql;

CREATE TYPE gate_with_desc AS (f UUID, t UUID, gate_type provenance_gate, desc_str CHARACTER VARYING, infos INTEGER[], extra TEXT);

CREATE OR REPLACE FUNCTION sub_circuit_with_desc(
  token UUID,
  token2desc regclass) RETURNS SETOF gate_with_desc AS
$$
BEGIN
  RETURN QUERY EXECUTE
    'WITH RECURSIVE transitive_closure(f,t,gate_type) AS (
      SELECT $1,t,provsql.get_gate_type($1) FROM unnest(provsql.get_children($1)) AS t
        UNION ALL
      SELECT p1.t,u,provsql.get_gate_type(p1.t) FROM transitive_closure p1, unnest(provsql.get_children(p1.t)) AS u)
    SELECT *, ARRAY[(get_infos(f)).info1, (get_infos(f)).info2], get_extra(f) FROM (
      SELECT f::uuid,t::uuid,gate_type,NULL FROM transitive_closure
        UNION ALL
      SELECT p2.provenance::uuid as f, NULL::uuid, ''input'', CAST (p2.value AS varchar) FROM transitive_closure p1 JOIN ' || token2desc || ' AS p2
        ON p2.provenance=t
        UNION ALL
      SELECT provenance::uuid as f, NULL::uuid, ''input'', CAST (value AS varchar) FROM ' || token2desc || ' WHERE provenance=$1
    ) t'
  USING token LOOP;
  RETURN;
END
$$ LANGUAGE plpgsql PARALLEL SAFE;

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
      (SELECT count(*) FROM pg_attribute a2 WHERE a2.attrelid=a1.attrelid AND attnum>0)-1 c
    FROM pg_attribute a1 JOIN pg_type ON atttypid=pg_type.oid
                        JOIN pg_class ON attrelid=pg_class.oid
                        JOIN pg_namespace ON relnamespace=pg_namespace.oid
    WHERE typname='uuid' AND relkind='r'
                                     AND nspname<>'provsql'
                                     AND attname='provsql'
  LOOP
    EXECUTE format('SELECT * FROM %I WHERE provsql=%L',t.relname,token) INTO result;
    IF result IS NOT NULL THEN
      table_name:=t.relname;
      nb_columns:=t.c;
      EXIT;
    END IF;
  END LOOP;
END
$$ LANGUAGE plpgsql STRICT;

CREATE OR REPLACE FUNCTION sub_circuit_for_where(token UUID)
  RETURNS TABLE(f UUID, t UUID, gate_type provenance_gate, table_name REGCLASS, nb_columns INTEGER, infos INTEGER[], extra TEXT) AS
$$
    WITH RECURSIVE transitive_closure(f,t,idx,gate_type) AS (
      SELECT $1,t,id,provsql.get_gate_type($1) FROM unnest(provsql.get_children($1)) WITH ORDINALITY AS a(t,id)
        UNION ALL
      SELECT p1.t,u,id,provsql.get_gate_type(p1.t) FROM transitive_closure p1, unnest(provsql.get_children(p1.t)) WITH ORDINALITY AS a(u, id)
    ) SELECT f, t, gate_type, table_name, nb_columns, ARRAY[(get_infos(f)).info1, (get_infos(f)).info2], get_extra(f) FROM (
      SELECT f, t::uuid, idx, gate_type, NULL AS table_name, NULL AS nb_columns FROM transitive_closure
      UNION ALL
        SELECT DISTINCT t, NULL::uuid, NULL::int, 'input'::provenance_gate, (id).table_name, (id).nb_columns FROM transitive_closure JOIN (SELECT t AS prov, provsql.identify_token(t) as id FROM transitive_closure WHERE t NOT IN (SELECT f FROM transitive_closure)) temp ON t=prov
      UNION ALL
        SELECT DISTINCT $1, NULL::uuid, NULL::int, 'input'::provenance_gate, (id).table_name, (id).nb_columns FROM (SELECT provsql.identify_token($1) AS id WHERE $1 NOT IN (SELECT f FROM transitive_closure)) temp
      ) t
$$
LANGUAGE sql;

--functions and aggregates for aggregate evaluation
CREATE OR REPLACE FUNCTION provenance_delta
  (token UUID)
  RETURNS UUID AS
$$
DECLARE
  delta_token uuid;
BEGIN
  IF token = gate_zero() THEN
    return token;
  END IF;

  IF token = gate_one() THEN
    return token;
  END IF;

  delta_token:=uuid_generate_v5(uuid_ns_provsql(),concat('delta',token));

  PERFORM create_gate(delta_token,'delta',ARRAY[token::uuid]);

  RETURN delta_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE;

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
  agg_tok_tuple agg_token;
BEGIN
  c:=array_length(tokens, 1);

  agg_val = CAST(val as VARCHAR);

  IF c = 0 THEN
    agg_tok := gate_zero();
  ELSE
    agg_tok := uuid_generate_v5(
      uuid_ns_provsql(),
      concat('agg',ARRAY[tokens]));
    PERFORM create_gate(agg_tok, 'agg', array_agg(t))
      FROM unnest(tokens) AS t
      WHERE t != gate_zero();
    PERFORM set_infos(agg_tok, aggfnoid, aggtype);
    PERFORM set_extra(agg_tok, agg_val);
  END IF;

  RETURN '( '||agg_tok||' , '||agg_val||' )';
END
$$ LANGUAGE plpgsql STRICT SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE OR REPLACE FUNCTION provenance_semimod(val anyelement, token UUID)
  RETURNS UUID AS
$$
DECLARE
  semimod_token uuid;
  value_token uuid;
BEGIN
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
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE OR REPLACE FUNCTION provenance_evaluate(
  token UUID,
  token2value regclass,
  element_one anyelement,
  plus_function regproc,
  times_function regproc,
  monus_function regproc = NULL,
  delta_function regproc = NULL)
  RETURNS anyelement AS
  'provsql','provenance_evaluate' LANGUAGE C STABLE;

CREATE OR REPLACE FUNCTION aggregation_evaluate(
  token UUID,
  token2value regclass,
  agg_function_final regproc,
  agg_function regproc,
  semimod_function regproc,
  element_one anyelement,
  plus_function regproc,
  times_function regproc,
  monus_function regproc = NULL,
  delta_function regproc = NULL)
  RETURNS anyelement AS
  'provsql','aggregation_evaluate' LANGUAGE C STABLE;

CREATE OR REPLACE FUNCTION probability_evaluate(
  token UUID,
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS
  'provsql','probability_evaluate' LANGUAGE C STABLE;

CREATE OR REPLACE FUNCTION shapley(
  token UUID,
  variable UUID,
  method text = NULL,
  arguments text = NULL,
  banzhaf BOOLEAN = 'f')
  RETURNS DOUBLE PRECISION AS
  'provsql','shapley' LANGUAGE C STABLE;

CREATE OR REPLACE FUNCTION shapley_all_vars(
  IN token UUID,
  IN method text = NULL,
  IN arguments text = NULL,
  IN banzhaf BOOLEAN = 'f',
  OUT variable UUID,
  OUT value DOUBLE PRECISION)
  RETURNS SETOF record AS
  'provsql', 'shapley_all_vars'
  LANGUAGE C STABLE;

CREATE OR REPLACE FUNCTION banzhaf(
  token UUID,
  variable UUID,
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS
  $$ SELECT provsql.shapley(token, variable, method, arguments, 't') $$
  LANGUAGE SQL;

CREATE OR REPLACE FUNCTION banzhaf_all_vars(
  IN token UUID,
  IN method text = NULL,
  IN arguments text = NULL,
  OUT variable UUID,
  OUT value DOUBLE PRECISION)
  RETURNS SETOF record AS
  $$ SELECT * FROM provsql.shapley_all_vars(token, method, arguments, 't') $$
  LANGUAGE SQL;

CREATE OR REPLACE FUNCTION view_circuit(
  token UUID,
  token2desc regclass,
  dbg int = 0)
  RETURNS TEXT AS
  'provsql','view_circuit' LANGUAGE C;

CREATE OR REPLACE FUNCTION provenance() RETURNS UUID AS
 'provsql', 'provenance' LANGUAGE C;

CREATE OR REPLACE FUNCTION where_provenance(token UUID)
  RETURNS text AS
  'provsql','where_provenance' LANGUAGE C;

SELECT create_gate(gate_zero(), 'zero');
SELECT create_gate(gate_one(), 'one');

GRANT USAGE ON SCHEMA provsql TO PUBLIC;

SET search_path TO public;
