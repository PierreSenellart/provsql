CREATE TYPE classification_level AS ENUM ('unclassified','restricted','confidential','secret','top_secret');

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

CREATE OR REPLACE FUNCTION security_min_state(state classification_level, level classification_level)
  RETURNS classification_level AS
$$
  SELECT CASE WHEN state IS NULL THEN level WHEN state<level THEN state ELSE level END
$$ LANGUAGE SQL IMMUTABLE;

CREATE OR REPLACE FUNCTION security_max_state(state classification_level, level classification_level)
  RETURNS classification_level AS
$$
  SELECT CASE WHEN state IS NULL THEN level WHEN state<level THEN level ELSE state END
$$ LANGUAGE SQL IMMUTABLE;

CREATE OR REPLACE AGGREGATE security_min(classification_level)
(
  sfunc = security_min_state,
  stype = classification_level,
  initcond = 'top_secret'
);

CREATE OR REPLACE AGGREGATE security_max(classification_level)
(
  sfunc = security_max_state,
  stype = classification_level,
  initcond = 'unclassified'
);

CREATE VIEW personal_level as SELECT classification AS value FROM personal;

SELECT 
  p1.city,
  provenance_evaluate(
    provenance(),
    'personal_level',
    'unclassified'::classification_level,
    'security_min',
    'security_max')
FROM personal p1, personal p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city;
