-- Requires extensions "uuid-ossp" and "hstore"
-- drop schema public cascade; create schema public; create extension "uuid-ossp"; create extension "hstore"; grant all on schema public to pierre;

SET search_path TO public;

CREATE DOMAIN provenance_token AS UUID;

CREATE TYPE provenance_gate AS ENUM('and','or','not','input');

CREATE TABLE provenance_circuit_gate(
  gate provenance_token PRIMARY KEY,
  gate_type provenance_gate NOT NULL);

CREATE TABLE provenance_circuit_wire(
  f provenance_token,
  t provenance_token);

CREATE INDEX ON provenance_circuit_wire (f);
CREATE INDEX ON provenance_circuit_wire (t);

CREATE OR REPLACE FUNCTION add_provenance_circuit_gate_trigger()
  RETURNS TRIGGER AS
$$
DECLARE
  attribute RECORD;
BEGIN
  FOR attribute IN
    SELECT attname 
    FROM pg_attribute JOIN pg_type ON pg_type.oid=pg_attribute.atttypid
    WHERE typname='provenance_token' AND attrelid=TG_TABLE_NAME::regclass
  LOOP  
    EXECUTE format('INSERT INTO provenance_circuit_gate VALUES (%L, ''input'')',hstore(NEW)->attribute.attname);
  END LOOP;
  RETURN NEW;
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$$
BEGIN
  EXECUTE format('ALTER TABLE %I ADD COLUMN provsql provenance_token UNIQUE DEFAULT uuid_generate_v4()', _tbl);
  EXECUTE format('INSERT INTO provenance_circuit_gate SELECT provsql, ''input'' FROM %I',_tbl);
  EXECUTE format('CREATE TRIGGER add_provenance_circuit_gate BEFORE INSERT ON %I FOR EACH ROW EXECUTE PROCEDURE add_provenance_circuit_gate_trigger()',_tbl);
END
$$ LANGUAGE plpgsql;

CREATE FUNCTION provenance_and(VARIADIC tokens uuid[])
  RETURNS provenance_token AS
$$
DECLARE
  and_token provenance_token;
  token provenance_token;
BEGIN
  CASE array_length(tokens,1)
    WHEN 0 THEN
      and_token:=NULL;
    WHEN 1 THEN
      and_token:=tokens[1];
    ELSE
      and_token:=uuid_generate_v4();
      INSERT INTO provenance_circuit_gate VALUES(and_token,'and');
      FOREACH token IN ARRAY tokens LOOP
        INSERT INTO provenance_circuit_wire VALUES(and_token,token);
      END LOOP;
  END CASE;
  RETURN and_token;
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION provenance_or(state provenance_token, token provenance_token)
  RETURNS provenance_token AS
$$
DECLARE
  or_token provenance_token;
BEGIN
  IF token IS NULL THEN
    RETURN NULL;
  END IF;

  IF state IS NULL THEN
    or_token:=uuid_generate_v4();
    INSERT INTO provenance_circuit_gate VALUES(or_token,'or');
  ELSE
    or_token:=state;
  END IF;
  INSERT INTO provenance_circuit_wire VALUES(or_token,token);

  RETURN or_token;
END
$$ LANGUAGE plpgsql;

CREATE AGGREGATE provenance_agg(token provenance_token) (
  SFUNC = provenance_or,
  STYPE = provenance_token
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
    FROM pg_attribute JOIN pg_type ON pg_type.oid=pg_attribute.atttypid JOIN pg_class ON attrelid=pg_class.oid
    WHERE typname='provenance_token' AND relkind='r' AND relname NOT LIKE 'provenance_circuit_%'
  LOOP
    IF statement IS NOT NULL THEN
      statement:=concat(statement,' UNION ');
    ELSE
      statement:='';
    END IF;
    statement:=concat(statement,format('SELECT %I FROM %I',attribute.attname,attribute.relname));
  END LOOP;
  EXECUTE concat('DELETE FROM provenance_circuit_gate WHERE gate NOT IN (',statement,') AND gate_type=''input''');
  WITH RECURSIVE reachable(gate) AS (
    SELECT gate from provenance_circuit_gate
  UNION
    SELECT t AS gate from provenance_circuit_wire JOIN reachable ON f=gate)
  DELETE FROM provenance_circuit_wire WHERE f NOT IN (SELECT * FROM reachable);
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION provenance_formula(token provenance_token)
  RETURNS varchar AS
$$
DECLARE
  rec record;
  result varchar;
BEGIN
  SELECT gate_type INTO STRICT rec FROM provenance_circuit_gate WHERE gate = token;
  CASE rec.gate_type
  WHEN 'input' THEN
    result:=token::text;
  WHEN 'or' THEN
    SELECT string_agg(concat('(',provenance_formula(t),')'),' ∨ ')  INTO result FROM provenance_circuit_wire WHERE f=token;
  WHEN 'and' THEN
    SELECT string_agg(concat('(',provenance_formula(t),')'),' ∧ ')  INTO result FROM provenance_circuit_wire WHERE f=token;
  END CASE;
  RETURN result;
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION provenance_evaluate(
  token provenance_token,
  token2value regclass,
  element_one anyelement,
  value_type regtype,
  or_function regproc,
  and_function regproc)
  RETURNS anyelement AS
$$
DECLARE
  rec record;
  result ALIAS FOR $0;
BEGIN
  IF token IS NULL THEN
    RETURN element_one;
  END IF;

  SELECT gate_type INTO rec FROM provenance_circuit_gate WHERE gate = token;
  
  IF rec IS NULL THEN
    RETURN NULL;
  ELSIF rec.gate_type='input' THEN
    EXECUTE format('SELECT col1 FROM (SELECT * FROM %I WHERE provenance()=%L) tmp (col1)',token2value,token) INTO result;
    IF result IS NULL THEN
      result:=element_one;
    END IF;
  ELSIF rec.gate_type='or' THEN
    EXECUTE format('SELECT %I(provenance_evaluate(t,%L,%L::%I,%L,%L,%L)) FROM provenance_circuit_wire WHERE f=%L',
      or_function,token2value,element_one,value_type,value_type,or_function,and_function,token)
    INTO result;
  ELSIF rec.gate_type='and' THEN
    EXECUTE format('SELECT %I(provenance_evaluate(t,%L,%L::%I,%L,%L,%L)) FROM provenance_circuit_wire WHERE f=%L',
      and_function,token2value,element_one,value_type,value_type,or_function,and_function,token)
    INTO result;
  END IF;
  RETURN result;
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION provenance_evaluate(
  token provenance_token,
  token2value regclass,
  element_one anyelement,
  or_function regproc,
  and_function regproc)
  RETURNS anyelement AS
  'provsql','provenance_evaluate' LANGUAGE C;

CREATE OR REPLACE FUNCTION provenance() RETURNS provenance_token AS
$$
BEGIN
  RAISE EXCEPTION USING MESSAGE='provenance() called on a table without provenance';
END
$$ LANGUAGE plpgsql;
