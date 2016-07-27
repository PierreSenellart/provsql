SET search_path TO public, provsql;

/* Cleaning up past sessions */
DROP VIEW IF EXISTS personal_level;
DROP VIEW IF EXISTS personal_name;
DROP TABLE IF EXISTS personal;
SELECT trim_circuit();

/* The security semiring */
DROP TYPE IF EXISTS classification_level CASCADE;

CREATE TYPE classification_level AS ENUM ('unclassified','restricted','confidential','secret','top_secret');

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

/* The provenance formula semiring */
DROP TYPE IF EXISTS formula_state CASCADE;
DROP AGGREGATE IF EXISTS formula_or(text);
DROP AGGREGATE IF EXISTS formula_and(text);

CREATE TYPE formula_state AS (
  formula text,
  nbargs int
);

CREATE FUNCTION formula_or_state(state formula_state, value text)
  RETURNS formula_state AS
$$
BEGIN
  IF state IS NULL OR state.nbargs=0 THEN
    RETURN (value,1);
  ELSE
    RETURN (concat(state.formula,'∨',value),state.nbargs+1);
  END IF;
END
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE FUNCTION formula_and_state(state formula_state, value text)
  RETURNS formula_state AS
$$
BEGIN    
  IF state IS NULL OR state.nbargs=0 THEN
    RETURN (value,1);
  ELSE
    RETURN (concat(state.formula,'∧',value),state.nbargs+1);
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

CREATE AGGREGATE formula_or(text)
(
  sfunc = formula_or_state,
  stype = formula_state,
  initcond = '(⊥,0)',
  finalfunc = formula_state2formula
);

CREATE AGGREGATE formula_and(text)
(
  sfunc = formula_and_state,
  stype = formula_state,
  initcond = '(⊤,0)',
  finalfunc = formula_state2formula
);

/* Create example */
CREATE TABLE personal(
  id SERIAL PRIMARY KEY,
  name varchar,
  position varchar,
  city varchar,
  classification classification_level
);

INSERT INTO personal (name,position,city,classification) VALUES
  ('John','Director','New York','unclassified'),
  ('Paul','Janitor','New York','restricted'),
  ('Dave','Analyst','Paris','confidential'),
  ('Ellen','Field agent','Berlin','secret'),
  ('Magdalen','Double agent','Paris','top_secret'),
  ('Nancy','HR','Paris','restricted'),
  ('Susan','Analyst','Berlin','secret');

SELECT add_provenance('personal');

/* Examples of provenance evaluation */
CREATE VIEW personal_level as SELECT classification AS value FROM personal;
SELECT 
  p1.city,
  provenance_evaluate(
    provenance(),
    'personal_level',
    'unclassified'::classification_level,
    'security_min',
    'security_max') AS level
FROM personal p1, personal p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city;

CREATE VIEW personal_name as SELECT name AS value FROM personal;
SELECT 
  p1.city,
  provenance_evaluate(
    provenance(),
    'personal_name',
    '⊤'::text,
    'formula_or',
    'formula_and') AS formula
FROM personal p1, personal p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city;
