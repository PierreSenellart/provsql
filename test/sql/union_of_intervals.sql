\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Union of intervals semiring, Belkis Djeffal
-- See https://inria.hal.science/hal-04342025

CREATE TABLE personnel_intervals(
  id SERIAL PRIMARY KEY,
  name varchar,
  position varchar,
  city varchar,
  valid_time datemultirange
);

INSERT INTO personnel_intervals (name, position, city, valid_time) VALUES
  ('John', 'Director', 'New York', '{["2021-01-15","2023-03-22"],["2019-07-10","2020-12-05"]}'),
  ('Paul', 'Janitor', 'New York', '{["2020-05-28","2022-11-15"],["2018-03-20","2019-09-10"]}'),
  ('Dave', 'Analyst', 'Paris', '{["2019-12-01","2022-08-15"],["2017-06-10","2018-11-30"]}'),
  ('Ellen', 'Field agent', 'Berlin', '{["2020-10-15","2023-05-28"],["2018-04-05","2019-11-20"]}'),
  ('Magdalen', 'Double agent', 'Paris', '{["2018-11-22","2021-09-01"],["2016-07-15","2018-02-28"]}'),
  ('Nancy', 'HR', 'Paris', '{["2017-08-03","2019-10-25"],["2015-02-10","2017-05-15"]}'),
  ('Susan', 'Analyst', 'Berlin', '{["2019-04-12","2022-02-01"],["2016-10-05","2018-01-15"]}');

/* The union_intervals  semiring */
CREATE OR REPLACE FUNCTION union_intervals_plus_state(state datemultirange, value datemultirange)
  RETURNS datemultirange AS
$$
  SELECT CASE WHEN state IS NULL THEN value ELSE state + value END
$$ LANGUAGE SQL IMMUTABLE;

CREATE OR REPLACE FUNCTION union_intervals_times_state(state datemultirange, value datemultirange)
  RETURNS datemultirange AS
$$
SELECT CASE WHEN state IS NULL THEN value ELSE state * value END
$$ LANGUAGE SQL IMMUTABLE;

CREATE OR REPLACE AGGREGATE union_intervals_plus(datemultirange)
(
  sfunc = union_intervals_plus_state,
  stype = datemultirange,
  initcond = '{}'
);

CREATE OR REPLACE AGGREGATE union_intervals_times(datemultirange)
(
  sfunc = union_intervals_times_state,
  stype = datemultirange,
  initcond = '{(,)}'
);

CREATE OR REPLACE FUNCTION union_intervals_monus(state datemultirange, value datemultirange)
  RETURNS datemultirange AS
$$
SELECT CASE WHEN state <@ value THEN '{}'::datemultirange ELSE state - value END
$$ LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION union_intervals(token UUID, token2value regclass)
  RETURNS datemultirange AS
$$
BEGIN
  RETURN provenance_evaluate(
    token,
    token2value,
    '{(,)}'::datemultirange,
    'union_intervals_plus',
    'union_intervals_times',
    'union_intervals_monus');
END
$$ LANGUAGE plpgsql PARALLEL SAFE;

SELECT add_provenance('personnel_intervals');

SELECT create_provenance_mapping('time_validity','personnel_intervals','valid_time');

CREATE TABLE union_intervals_result AS
SELECT *,union_intervals(provenance(),'time_validity')
FROM (
  SELECT DISTINCT P1.city
  FROM personnel_intervals P1 JOIN personnel_intervals P2
  ON P1.city = P2.city
  WHERE P1.id < P2.id
) t;
SELECT remove_provenance('union_intervals_result');
SELECT * FROM union_intervals_result;
DROP TABLE union_intervals_result;

CREATE TABLE union_intervals_result AS
SELECT *,union_intervals(provenance(),'time_validity')
FROM (
  SELECT DISTINCT city FROM personnel_intervals
  EXCEPT
    SELECT DISTINCT P1.city
    FROM personnel_intervals P1 JOIN personnel_intervals P2
    ON P1.city = P2.city
    WHERE P1.city = P2.city AND P1.id < P2.id
) t;
SELECT remove_provenance('union_intervals_result');
SELECT * FROM union_intervals_result;
DROP TABLE union_intervals_result;
