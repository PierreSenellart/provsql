\set ECHO none
SET search_path TO public, provsql;

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

/* Example of provenance evaluation */
CREATE VIEW personal_level as SELECT classification AS value FROM personal;
CREATE TABLE result_security AS SELECT 
  p1.city,
  security(provenance(),'personal_level')
FROM personal p1, personal p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_security');
SELECT * FROM result_security;

DROP TABLE result_security;
