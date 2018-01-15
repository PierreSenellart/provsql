CREATE EXTENSION "uuid-ossp";
CREATE EXTENSION provsql;

SET search_path TO public, provsql;

CREATE TYPE classification_level AS ENUM ('unclassified','restricted','confidential','secret','top_secret');

CREATE TABLE personnel(
  id SERIAL PRIMARY KEY,
  name varchar,
  position varchar,
  city varchar,
  classification classification_level
);

INSERT INTO personnel (name,position,city,classification) VALUES
  ('John','Director','New York','unclassified'),
  ('Paul','Janitor','New York','restricted'),
  ('Dave','Analyst','Paris','confidential'),
  ('Ellen','Field agent','Berlin','secret'),
  ('Magdalen','Double agent','Paris','top_secret'),
  ('Nancy','HR','Paris','restricted'),
  ('Susan','Analyst','Berlin','secret');

/* The provenance formula m-semiring */
CREATE TYPE formula_state AS (
  formula text,
  nbargs int
);

CREATE FUNCTION formula_plus_state(state formula_state, value text)
  RETURNS formula_state AS
$$
BEGIN
  IF state IS NULL OR state.nbargs=0 THEN
    RETURN (value,1);
  ELSE
    RETURN (concat(state.formula,' ⊕ ',value),state.nbargs+1);
  END IF;
END
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE FUNCTION formula_times_state(state formula_state, value text)
  RETURNS formula_state AS
$$
BEGIN    
  IF state IS NULL OR state.nbargs=0 THEN
    RETURN (value,1);
  ELSE
    RETURN (concat(state.formula,' ⊗ ',value),state.nbargs+1);
  END IF;
END
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE FUNCTION formula_state2formula(state formula_state)
  RETURNS text AS
$$
  SELECT
    CASE
      WHEN state.nbargs<2 THEN state.formula
      ELSE concat('(',state.formula,')')
    END;
$$ LANGUAGE SQL IMMUTABLE STRICT;

CREATE AGGREGATE formula_plus(text)
(
  sfunc = formula_plus_state,
  stype = formula_state,
  initcond = '(𝟘,0)',
  finalfunc = formula_state2formula
);

CREATE AGGREGATE formula_times(text)
(
  sfunc = formula_times_state,
  stype = formula_state,
  initcond = '(𝟙,0)',
  finalfunc = formula_state2formula
);

CREATE FUNCTION formula_monus(formula1 text, formula2 text) RETURNS text AS
$$
  SELECT concat('(',formula1,' ⊖ ',formula2,')')
$$ LANGUAGE SQL IMMUTABLE STRICT;

CREATE FUNCTION formula(token provenance_token, token2value regclass)
  RETURNS text AS
$$
BEGIN
  RETURN provenance_evaluate(
    token,
    token2value,
    '𝟙'::text,
    'formula_plus',
    'formula_times',
    'formula_monus');
END
$$ LANGUAGE plpgsql;

/* The counting m-semiring */

CREATE FUNCTION counting_plus_state(state INTEGER, value INTEGER)
  RETURNS INTEGER AS
$$
  SELECT CASE WHEN state IS NULL THEN value ELSE state + value END
$$ LANGUAGE SQL IMMUTABLE;

CREATE FUNCTION counting_times_state(state INTEGER, value INTEGER)
  RETURNS INTEGER AS
$$
SELECT CASE WHEN state IS NULL THEN value ELSE state * value END
$$ LANGUAGE SQL IMMUTABLE;

CREATE AGGREGATE counting_plus(INTEGER)
(
  sfunc = counting_plus_state,
  stype = INTEGER,
  initcond = 0
);

CREATE AGGREGATE counting_times(INTEGER)
(
  sfunc = counting_times_state,
  stype = INTEGER,
  initcond = 1
);

CREATE FUNCTION counting_monus(counting1 INTEGER, counting2 INTEGER) RETURNS INTEGER AS
$$
  SELECT CASE WHEN counting1 < counting2 THEN 0 ELSE counting1 - counting2 END
$$ LANGUAGE SQL IMMUTABLE STRICT;

CREATE FUNCTION counting(token provenance_token, token2value regclass)
  RETURNS INTEGER AS
$$
BEGIN
  RETURN provenance_evaluate(
    token,
    token2value,
    1,
    'counting_plus',
    'counting_times',
    'counting_monus');
END
$$ LANGUAGE plpgsql;

/* The security semiring */
CREATE FUNCTION security_min_state(state classification_level, level classification_level)
  RETURNS classification_level AS
$$
  SELECT CASE WHEN state IS NULL THEN level WHEN state<level THEN state ELSE level END
$$ LANGUAGE SQL IMMUTABLE;

CREATE FUNCTION security_max_state(state classification_level, level classification_level)
  RETURNS classification_level AS
$$
  SELECT CASE WHEN state IS NULL THEN level WHEN state<level THEN level ELSE state END
$$ LANGUAGE SQL IMMUTABLE;

CREATE AGGREGATE security_min(classification_level)
(
  sfunc = security_min_state,
  stype = classification_level,
  initcond = 'top_secret'
);

CREATE AGGREGATE security_max(classification_level)
(
  sfunc = security_max_state,
  stype = classification_level,
  initcond = 'unclassified'
);

CREATE FUNCTION security(token provenance_token, token2value regclass)
  RETURNS classification_level AS
$$
BEGIN
  RETURN provenance_evaluate(
    token,
    token2value,
    'unclassified'::classification_level,
    'security_min',
    'security_max');
END
$$ LANGUAGE plpgsql;

CREATE TABLE x AS SELECT ones.n + 10*tens.n FROM (VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9)) ones(n),
     (VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9)                ) tens(n);
CREATE TABLE r AS SELECT x1."?column?" x, x2."?column?" y, random() AS prob FROM x x1,x x2 ORDER BY x1.*,x2.*;
DROP TABLE x;
