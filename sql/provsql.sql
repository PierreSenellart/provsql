-- Requires extensions "uuid-ossp"

CREATE SCHEMA provsql;

SET search_path TO provsql;

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

CREATE OR REPLACE FUNCTION provsql.add_provenance_circuit_gate_trigger()
  RETURNS TRIGGER AS
$$
DECLARE
  attribute RECORD;
BEGIN
  INSERT INTO provenance_circuit_gate VALUES (NEW.provsql, 'input');
  RETURN NEW;
END
$$ LANGUAGE plpgsql SET search_path=provsql;

CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$$
BEGIN
  EXECUTE format('ALTER TABLE %I ADD COLUMN provsql provsql.provenance_token UNIQUE DEFAULT uuid_generate_v4()', _tbl);
  EXECUTE format('INSERT INTO provsql.provenance_circuit_gate SELECT provsql, ''input'' FROM %I',_tbl);
  EXECUTE format('CREATE TRIGGER add_provenance_circuit_gate BEFORE INSERT ON %I FOR EACH ROW EXECUTE PROCEDURE provsql.add_provenance_circuit_gate_trigger()',_tbl);
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
$$ LANGUAGE plpgsql SET search_path=provsql,public SECURITY DEFINER;

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
$$ LANGUAGE plpgsql SET search_path=provsql,public SECURITY DEFINER;

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
    EXECUTE concat('DELETE FROM provsql.provenance_circuit_gate WHERE gate NOT IN (',statement,') AND gate_type=''input''');
    WITH RECURSIVE reachable(gate) AS (
      SELECT gate from provsql.provenance_circuit_gate
    UNION
      SELECT t AS gate from provsql.provenance_circuit_wire JOIN reachable ON f=gate)
    DELETE FROM provsql.provenance_circuit_wire WHERE f NOT IN (SELECT * FROM reachable);
  END IF;
END
$$ LANGUAGE plpgsql SECURITY DEFINER;

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

  SELECT gate_type INTO rec FROM provsql.provenance_circuit_gate WHERE gate = token;
  
  IF rec IS NULL THEN
    RETURN NULL;
  ELSIF rec.gate_type='input' THEN
    EXECUTE format('SELECT col1 FROM (SELECT * FROM %I WHERE provsql.provenance()=%L) tmp (col1)',token2value,token) INTO result;
    IF result IS NULL THEN
      result:=element_one;
    END IF;
  ELSIF rec.gate_type='or' THEN
    EXECUTE format('SELECT %I(provsql.provenance_evaluate(t,%L,%L::%I,%L,%L,%L)) FROM provsql.provenance_circuit_wire WHERE f=%L',
      or_function,token2value,element_one,value_type,value_type,or_function,and_function,token)
    INTO result;
  ELSIF rec.gate_type='and' THEN
    EXECUTE format('SELECT %I(provsql.provenance_evaluate(t,%L,%L::%I,%L,%L,%L)) FROM provsql.provenance_circuit_wire WHERE f=%L',
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

GRANT USAGE ON SCHEMA provsql TO PUBLIC;
GRANT INSERT, SELECT ON provenance_circuit_gate TO PUBLIC;
GRANT SELECT ON provenance_circuit_wire TO PUBLIC;

SET search_path TO public;
