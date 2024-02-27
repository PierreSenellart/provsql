\set ECHO none
\pset format csv
SET search_path TO provsql_test,provsql;

CREATE TABLE weather_conditions (dummy VARCHAR, weather VARCHAR,
                ground VARCHAR, p FLOAT);
INSERT INTO weather_conditions VALUES ('dummy',    'rain', 'wet', 0.35);
INSERT INTO weather_conditions VALUES ('dummy',    'rain', 'dry', 0.05);
INSERT INTO weather_conditions VALUES ('dummy', 'no rain', 'wet', 0.1);
INSERT INTO weather_conditions VALUES ('dummy', 'no rain', 'dry', 0.5);

SELECT repair_key('weather_conditions','dummy');
DO $$ BEGIN
  PERFORM set_prob(provenance(), p) FROM weather_conditions;
END $$;

CREATE TABLE result_repair_key AS
  SELECT *, probability_evaluate(provenance()) prob FROM (
    SELECT ground FROM weather_conditions GROUP BY ground) t;

SELECT remove_provenance('result_repair_key');
SELECT ground, ROUND(prob::numeric, 3) FROM result_repair_key;

DROP TABLE result_repair_key;
DROP TABLE weather_conditions;
