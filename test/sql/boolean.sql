\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

/* The BOOLEAN m-semiring */
CREATE FUNCTION boolean_plus_state(state BOOLEAN, value BOOLEAN)
  RETURNS BOOLEAN AS
$$
BEGIN
  IF state IS NULL THEN
    RETURN value;
  ELSE
    RETURN state OR value;
  END IF;
END
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE FUNCTION boolean_times_state(state BOOLEAN, value BOOLEAN)
  RETURNS BOOLEAN AS
$$
BEGIN
  IF state IS NULL THEN
    RETURN value;
  ELSE
    RETURN state AND value;
  END IF;
END
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE AGGREGATE boolean_plus(BOOLEAN)
(
  sfunc = boolean_plus_state,
  stype = BOOLEAN,
  initcond = FALSE
);

CREATE AGGREGATE boolean_times(BOOLEAN)
(
  sfunc = boolean_times_state,
  stype = BOOLEAN,
  initcond = TRUE
);

CREATE FUNCTION boolean_monus(b1 BOOLEAN, b2 BOOLEAN) RETURNS BOOLEAN AS
$$
  SELECT b1 AND NOT b2
$$ LANGUAGE SQL IMMUTABLE STRICT;

CREATE FUNCTION boolean_sr(token UUID, token2value regclass)
  RETURNS BOOLEAN AS
$$
BEGIN
  RETURN provenance_evaluate(
    token,
    token2value,
    TRUE,
    'boolean_plus',
    'boolean_times',
    'boolean_monus');
END
$$ LANGUAGE plpgsql PARALLEL SAFE;

CREATE TABLE boolean_result AS
  SELECT p1.city, boolean_sr(provenance(), 'personnel_active') AS b
  FROM personnel p1 JOIN personnel p2 ON p1.city=p2.city AND p1.id<p2.id
  GROUP BY p1.city;
SELECT remove_provenance('boolean_result');
SELECT city FROM boolean_result WHERE b;
DROP TABLE boolean_result;
