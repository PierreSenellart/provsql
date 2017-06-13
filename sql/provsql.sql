-- Requires extensions "uuid-ossp"

CREATE SCHEMA provsql;

SET search_path TO provsql;

CREATE DOMAIN provenance_token AS UUID NOT NULL;

CREATE TYPE provenance_gate AS ENUM('input','plus','times','monus','zero','one');

CREATE TABLE provenance_circuit_gate(
  gate provenance_token PRIMARY KEY,
  gate_type provenance_gate NOT NULL);

CREATE TABLE provenance_circuit_wire(
  f provenance_token REFERENCES provenance_circuit_gate(gate) ON DELETE CASCADE,
  t provenance_token REFERENCES provenance_circuit_gate(gate) ON DELETE CASCADE);

CREATE INDEX ON provenance_circuit_wire (f);
CREATE INDEX ON provenance_circuit_wire (t);

CREATE OR REPLACE FUNCTION add_provenance_circuit_gate_trigger()
  RETURNS TRIGGER AS
$$
DECLARE
  attribute RECORD;
BEGIN
  INSERT INTO provenance_circuit_gate VALUES (NEW.provsql, 'input');
  RETURN NEW;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp SECURITY DEFINER;

CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$$
BEGIN
  EXECUTE format('ALTER TABLE %I ADD COLUMN provsql provsql.provenance_token UNIQUE DEFAULT uuid_generate_v4()', _tbl);
  EXECUTE format('INSERT INTO provsql.provenance_circuit_gate SELECT provsql, ''input'' FROM %I',_tbl);
  EXECUTE format('CREATE TRIGGER add_provenance_circuit_gate BEFORE INSERT ON %I FOR EACH ROW EXECUTE PROCEDURE provsql.add_provenance_circuit_gate_trigger()',_tbl);
  EXECUTE format('ALTER TABLE %I ADD CONSTRAINT provsqlfk FOREIGN KEY (provsql) REFERENCES provsql.provenance_circuit_gate(gate)', _tbl);
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE OR REPLACE FUNCTION remove_provenance(_tbl regclass)
  RETURNS void AS
$$
DECLARE
BEGIN
  EXECUTE format('ALTER TABLE %I DROP COLUMN provsql', _tbl);
  BEGIN
    EXECUTE format('DROP TRIGGER add_provenance_circuit_gate on %I', _tbl);
  EXCEPTION WHEN undefined_object THEN
  END;
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION create_provenance_mapping(newtbl text, oldtbl regclass, att text)
  RETURNS void AS
$$
DECLARE
BEGIN
  EXECUTE format('CREATE TABLE %I AS SELECT %s AS value, provenance() FROM %I', newtbl, att, oldtbl);
  EXECUTE format('CREATE INDEX ON %I(provenance)', newtbl);
  EXECUTE format('SELECT provsql.remove_provenance(%L)', newtbl);
END
$$ LANGUAGE plpgsql;

CREATE FUNCTION uuid_ns_provsql() RETURNS uuid AS
$$
 -- uuid_generate_v5(uuid_ns_url(),'http://pierre.senellart.com/software/provsql/')
 SELECT '920d4f02-8718-5319-9532-d4ab83a64489'::uuid
$$ LANGUAGE SQL IMMUTABLE;

CREATE FUNCTION gate_zero() RETURNS uuid AS
$$
  SELECT public.uuid_generate_v5(uuid_ns_provsql(),'zero');
$$ LANGUAGE SQL IMMUTABLE;
CREATE FUNCTION gate_one() RETURNS uuid AS
$$
  SELECT public.uuid_generate_v5(uuid_ns_provsql(),'one');
$$ LANGUAGE SQL IMMUTABLE;
      
INSERT INTO provenance_circuit_gate
  VALUES(gate_zero(),'zero');
INSERT INTO provenance_circuit_gate
  VALUES(gate_one(),'one');

CREATE FUNCTION uuid_provsql_concat(state uuid, token provenance_token)
  RETURNS provenance_token AS
$$
  SELECT
    CASE
    WHEN state IS NULL THEN
      token
    ELSE
      uuid_generate_v5(uuid_ns_provsql(),concat(state,token))::provenance_token
    END;
$$ LANGUAGE SQL IMMUTABLE SET search_path=provsql,public;

CREATE AGGREGATE uuid_provsql_agg(provenance_token) (
  SFUNC = uuid_provsql_concat,
  STYPE = provenance_token
);

CREATE FUNCTION provenance_times(VARIADIC tokens uuid[])
  RETURNS provenance_token AS
$$
DECLARE
  times_token uuid;
BEGIN
  CASE array_length(tokens,1)
    WHEN 0 THEN
      times_token:=gate_one();
    WHEN 1 THEN
      times_token:=tokens[1];
    ELSE
      SELECT uuid_generate_v5(uuid_ns_provsql(),concat('times',uuid_provsql_agg(t)))
      INTO times_token
      FROM unnest(tokens) t;

      BEGIN
        INSERT INTO provenance_circuit_gate VALUES(times_token,'times');
        INSERT INTO provenance_circuit_wire SELECT times_token, t FROM unnest(tokens) t;
      EXCEPTION WHEN unique_violation THEN
      END;
  END CASE;
  RETURN times_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE FUNCTION provenance_monus(token1 provenance_token, token2 provenance_token)
  RETURNS provenance_token AS
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
  ELSE  
    monus_token:=uuid_generate_v5(uuid_ns_provsql(),concat('monus',token1,token2));
    BEGIN
      INSERT INTO provenance_circuit_gate VALUES(monus_token,'monus');
      INSERT INTO provenance_circuit_wire VALUES(monus_token,token1);
      INSERT INTO provenance_circuit_wire VALUES(monus_token,token2);
    EXCEPTION WHEN unique_violation THEN
    END;
  END IF;  

  RETURN monus_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE OR REPLACE FUNCTION provenance_plus
  (state provenance_token, token provenance_token)
  RETURNS provenance_token AS
$$
DECLARE
  plus_token uuid;
BEGIN
  IF token = gate_zero() THEN
    return state;
  END IF;

  IF state IS NULL THEN
    plus_token:=uuid_generate_v4();
    INSERT INTO provenance_circuit_gate VALUES(plus_token,'plus');
  ELSE
    plus_token:=state;
  END IF;
  INSERT INTO provenance_circuit_wire VALUES(plus_token,token);

  RETURN plus_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE OR REPLACE FUNCTION provenance_plus_make_deterministic(state provenance_token)
  RETURNS provenance_token AS
$$
DECLARE
  c INTEGER;
  plus_token uuid;
BEGIN
  SELECT COUNT(*) INTO c FROM provenance_circuit_wire WHERE f=state;

  IF c = 0 THEN
    plus_token := gate_zero();
  ELSIF c = 1 THEN
    SELECT t INTO STRICT plus_token FROM provenance_circuit_wire WHERE f=state;
    DELETE FROM provenance_circuit_wire WHERE f=state;
  ELSE
    SELECT uuid_generate_v5(uuid_ns_provsql(),concat('plus',uuid_provsql_agg(t)))
    INTO plus_token
    FROM provenance_circuit_wire
    WHERE f=state;

    BEGIN
      INSERT INTO provenance_circuit_gate VALUES(plus_token,'plus');
      UPDATE provenance_circuit_wire SET f=plus_token WHERE f=state;
    EXCEPTION WHEN unique_violation THEN
      DELETE FROM provenance_circuit_wire WHERE f=state;
    END;
  END IF;
  DELETE FROM provenance_circuit_gate WHERE gate=state;

  RETURN plus_token;
END
$$ LANGUAGE plpgsql STRICT SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE AGGREGATE provenance_agg(token provenance_token) (
  SFUNC = provenance_plus,
  STYPE = provenance_token,
  FINALFUNC = provenance_plus_make_deterministic
);

CREATE OR REPLACE FUNCTION trim_circuit()
  RETURNS void AS
$$
DECLARE
  attribute record;
  statement varchar;
BEGIN
  FOR attribute IN
    SELECT attname, relname
    FROM pg_attribute JOIN pg_type ON atttypid=pg_type.oid JOIN pg_namespace ns1 ON typnamespace=ns1.oid
                      JOIN pg_class ON attrelid=pg_class.oid JOIN pg_namespace ns2 ON relnamespace=ns2.oid
    WHERE typname='provenance_token' AND relkind='r' AND ns1.nspname='provsql' AND ns2.nspname<>'provsql'
  LOOP
    IF statement IS NOT NULL THEN
      statement:=concat(statement,' UNION ');
    ELSE
      statement:='';
    END IF;
    statement:=concat(statement,format('SELECT %I FROM %I',attribute.attname,attribute.relname));
  END LOOP;
  IF statement IS NOT NULL THEN
    EXECUTE concat(
$concat$
      WITH RECURSIVE reachable(gate) AS (
$concat$,
      statement,
$concat$      
      UNION
      SELECT t AS gate from provsql.provenance_circuit_wire JOIN reachable ON f=gate)
    DELETE FROM provsql.provenance_circuit_gate WHERE gate NOT IN (SELECT * FROM reachable)
$concat$);
  ELSE
    TRUNCATE provsql.provenance_circuit_gate, provsql.provenance_circuit_wire;
  END IF;
END
$$ LANGUAGE plpgsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION provenance_evaluate(
  token provenance_token,
  token2value regclass,
  element_one anyelement,
  value_type regtype,
  plus_function regproc,
  times_function regproc,
  monus_function regproc)
  RETURNS anyelement AS
$$
DECLARE
  rec record;
  result ALIAS FOR $0;
BEGIN
  SELECT gate_type INTO rec FROM provsql.provenance_circuit_gate WHERE gate = token;
  
  IF rec IS NULL THEN
    RETURN NULL;
  ELSIF rec.gate_type='input' THEN
    EXECUTE format('SELECT * FROM %I WHERE provenance=%L',token2value,token) INTO result;
    IF result IS NULL THEN
      result:=element_one;
    END IF;
  ELSIF rec.gate_type='plus' THEN
    EXECUTE format('SELECT %I(provsql.provenance_evaluate(t,%L,%L::%s,%L,%L,%L,%L)) FROM provsql.provenance_circuit_wire WHERE f=%L',
      plus_function,token2value,element_one,value_type,value_type,plus_function,times_function,monus_function,token)
    INTO result;
  ELSIF rec.gate_type='times' THEN
    EXECUTE format('SELECT %I(provsql.provenance_evaluate(t,%L,%L::%s,%L,%L,%L,%L)) FROM provsql.provenance_circuit_wire WHERE f=%L',
      times_function,token2value,element_one,value_type,value_type,plus_function,times_function,monus_function,token)
    INTO result;
  ELSIF rec.gate_type='monus' THEN
    IF monus_function IS NULL THEN
      RAISE EXCEPTION USING MESSAGE='Provenance with negation evaluated over a semiring without monus function';
    ELSE
      EXECUTE format('SELECT %I(a[1],a[2]) FROM (SELECT array_agg(provsql.provenance_evaluate(t,%L,%L::%s,%L,%L,%L,%L)) AS a FROM provsql.provenance_circuit_wire WHERE f=%L) t',
        monus_function,token2value,element_one,value_type,value_type,plus_function,times_function,monus_function,token)
      INTO result;
    END IF;
  ELSIF rec.gate_type='zero' THEN
    EXECUTE format('SELECT %I(a) FROM (SELECT %L::%I AS a WHERE FALSE) temp',plus_function,element_one,value_type) INTO result;
  ELSIF rec.gate_type='one' THEN
    EXECUTE format('SELECT %L::%I',element_one,value_type) INTO result;
  ELSE
    RAISE EXCEPTION USING MESSAGE='Unknown gate type';
  END IF;
  RETURN result;
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION provenance_evaluate(
  token provenance_token,
  token2value regclass,
  element_one anyelement,
  plus_function regproc,
  times_function regproc,
  monus_function regproc = NULL)
  RETURNS anyelement AS
  'provsql','provenance_evaluate' LANGUAGE C;

CREATE OR REPLACE FUNCTION provenance() RETURNS provenance_token AS
 'provsql', 'provenance' LANGUAGE C;

GRANT USAGE ON SCHEMA provsql TO PUBLIC;
GRANT SELECT ON provenance_circuit_gate TO PUBLIC;
GRANT SELECT ON provenance_circuit_wire TO PUBLIC;

SET search_path TO public;
