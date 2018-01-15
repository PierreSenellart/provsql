\set ECHO none
SET search_path TO public, provsql;

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

/* Example of provenance evaluation */
SELECT create_provenance_mapping('personnel_name', 'personnel', 'name');
CREATE TABLE result_formula AS SELECT 
  p1.city,
  formula(provenance(), 'personnel_name')
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_formula');
SELECT * FROM result_formula;

DROP TABLE result_formula;
