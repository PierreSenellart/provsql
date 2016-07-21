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

SELECT p1.name AS n1, p2.name AS n2, provenance(p1.prov,p2.prov)
FROM personal p1 JOIN personal p2 ON p1.city=p2.city
WHERE p1.id<p2.id
GROUP BY p1.name,p2.name;

CREATE OR REPLACE FUNCTION security_min(levels classification_level[])
  RETURNS classification_level AS
$$
  SELECT min(x) FROM unnest(levels) AS x;   
$$ LANGUAGE SQL;

CREATE OR REPLACE FUNCTION security_max(levels classification_level[])
  RETURNS classification_level AS
$$
  SELECT max(x) FROM unnest(levels) AS x;   
$$ LANGUAGE SQL;

SELECT p1.city,
       provenance_evaluate(
         provenance(p1.prov,p2.prov),
         array(select hstore(ARRAY['token','value'],ARRAY[prov::text,classification::text]) FROM personal),
         'unclassified'::classification_level,
         'classification_level',
         'security_min',
         'security_max') AS prov
FROM personal p1 JOIN personal p2 ON p1.city=p2.city
WHERE p1.id <> p2.id
GROUP BY p1.city;
