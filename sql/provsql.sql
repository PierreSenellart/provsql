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
    EXECUTE format('INSERT INTO provenance_circuit_gate VALUES (%s, ''input'')',quote_literal(hstore(NEW)->attribute.attname));
  END LOOP;
  RETURN NEW;
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass,
                                          _col varchar DEFAULT 'prov')
  RETURNS void AS
$$
BEGIN
  EXECUTE format('ALTER TABLE %s ADD COLUMN %I provenance_token UNIQUE DEFAULT uuid_generate_v4()', _tbl, _col);
  EXECUTE format('INSERT INTO provenance_circuit_gate SELECT %I, ''input'' FROM %s',_col,_tbl);
  EXECUTE format('CREATE TRIGGER add_provenance_circuit_gate BEFORE INSERT ON %s FOR EACH ROW EXECUTE PROCEDURE add_provenance_circuit_gate_trigger()',_tbl);
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION provenance_or(state provenance_token, tokens uuid[])
  RETURNS provenance_token AS
$$
DECLARE
  and_token provenance_token;
  or_token provenance_token;
  token provenance_token;
BEGIN
  CASE array_length(tokens,1)
    WHEN 0 THEN
      RETURN state;
    WHEN 1 THEN
      and_token:=tokens[1];
    ELSE
      and_token:=uuid_generate_v4();
      INSERT INTO provenance_circuit_gate VALUES(and_token,'and');
      FOREACH token IN ARRAY tokens LOOP
        INSERT INTO provenance_circuit_wire VALUES(and_token,token);
      END LOOP;
  END CASE;

  IF state IS NULL THEN
    or_token:=uuid_generate_v4();
    INSERT INTO provenance_circuit_gate VALUES(or_token,'or');
  ELSE
    or_token:=state;
  END IF;
  INSERT INTO provenance_circuit_wire VALUES(or_token,and_token);

  RETURN or_token;
END
$$ LANGUAGE plpgsql;

CREATE AGGREGATE provenance(VARIADIC tokens uuid[]) (
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
  token2value hstore[],
  default_value anyelement,
  value_type regtype,
  or_function regproc,
  and_function regproc)
  RETURNS anyelement AS
$$
DECLARE
  rec record;
  result ALIAS FOR $0;
BEGIN
  SELECT gate_type INTO STRICT rec
  FROM provenance_circuit_gate
  WHERE gate = token;
  
  IF rec.gate_type='input' THEN
    SELECT x->'value' INTO result
    FROM unnest(token2value) AS x
    WHERE (x->'token')::provenance_token=token;
  ELSIF rec.gate_type='or' THEN
    EXECUTE format('SELECT %I(array_agg(provenance_evaluate(t,%s,%s::%I,%s,%s,%s))) FROM provenance_circuit_wire WHERE f=%s',
      or_function,quote_literal(token2value),quote_literal(default_value),value_type,quote_literal(value_type),quote_literal(or_function),quote_literal(and_function),quote_literal(token))
    INTO result;
  ELSIF rec.gate_type='and' THEN
    EXECUTE format('SELECT %I(array_agg(provenance_evaluate(t,%s,%s::%I,%s,%s,%s))) FROM provenance_circuit_wire WHERE f=%s',
      and_function,quote_literal(token2value),quote_literal(default_value),value_type,quote_literal(value_type),quote_literal(or_function),quote_literal(and_function),quote_literal(token))
    INTO result;
  END IF;
  RETURN result;
END
$$ LANGUAGE plpgsql;
