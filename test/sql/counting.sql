\set ECHO none
SET search_path TO public, provsql;

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

/* Example of provenance evaluation */
SELECT create_provenance_mapping('personnel_count', 'personnel', '1');
CREATE TABLE result_counting AS SELECT 
  p1.city,
  counting(provenance(),'personnel_count')
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_counting');
SELECT * FROM result_counting;

DROP TABLE result_counting;
