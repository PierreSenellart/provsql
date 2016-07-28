\set ECHO none
SET client_min_messages = 'ERROR'; -- temporary as I remove warnings
SET search_path TO public, provsql;

/* The provenance formula semiring */
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

/* Example of provenance evaluation */
CREATE VIEW personal_name as SELECT name AS value FROM personal;
CREATE TABLE result_formula AS SELECT 
  p1.city,
  provenance_evaluate(
    provenance(),
    'personal_name',
    '⊤'::text,
    'formula_or',
    'formula_and') AS formula
FROM personal p1, personal p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_formula');
SELECT * FROM result_formula;

DROP VIEW personal_name;
DROP TABLE result_formula;
