\set ECHO none
SET search_path TO public, provsql;

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

CREATE FUNCTION boolean_sr(token provenance_token, token2value regclass)
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
$$ LANGUAGE plpgsql;
