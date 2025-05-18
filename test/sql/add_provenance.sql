\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

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

CREATE TABLE nb_gates (x INT);
INSERT INTO nb_gates SELECT get_nb_gates();

SELECT add_provenance('personnel');

SELECT get_nb_gates()-x AS nb FROM nb_gates;
DROP TABLE nb_gates;

SELECT attname
FROM pg_attribute
WHERE attrelid ='personnel'::regclass AND attnum>1
ORDER BY attname;

SELECT create_provenance_mapping('personnel_name', 'personnel', 'name');
