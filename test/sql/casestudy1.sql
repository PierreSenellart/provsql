\set ECHO none
\pset format unaligned

-- Case Study 1: The Intelligence Agency
-- Uses an 'agents' table to avoid conflict with the existing provsql_test.personnel table.

CREATE TABLE agents (
    id SERIAL PRIMARY KEY,
    name TEXT NOT NULL,
    position TEXT NOT NULL,
    city TEXT NOT NULL,
    classification classification_level NOT NULL
);

INSERT INTO agents (name, position, city, classification) VALUES
    ('Juma',   'Director',     'Nairobi', 'unclassified'),
    ('Paul',   'Janitor',      'Nairobi', 'restricted'),
    ('David',  'Analyst',      'Paris',   'confidential'),
    ('Ellen',  'Field agent',  'Beijing', 'secret'),
    ('Aaheli', 'Double agent', 'Paris',   'top_secret'),
    ('Nancy',  'HR',           'Paris',   'restricted'),
    ('Jing',   'Analyst',      'Beijing', 'secret');

SELECT add_provenance('agents');
SELECT create_provenance_mapping('agents_name', 'agents', 'name');

-- Step 3: sr_formula – cities with multiple agents
CREATE TABLE result_cs1_formula AS
SELECT p1.city,
    sr_formula(provenance(), 'agents_name') AS formula
FROM agents p1
JOIN agents p2 ON p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city;

SELECT remove_provenance('result_cs1_formula');
SELECT city, formula FROM result_cs1_formula
WHERE city IN ('Nairobi','Beijing') ORDER BY city;
DROP TABLE result_cs1_formula;

-- Step 4: sr_minmax – security clearance for each shared city
SELECT create_provenance_mapping('agents_level', 'agents', 'classification');

CREATE TABLE result_cs1_security AS
SELECT p1.city,
    sr_minmax(provenance(), 'agents_level',
              'unclassified'::classification_level) AS min_clearance
FROM agents p1
JOIN agents p2 ON p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city;

SELECT remove_provenance('result_cs1_security');
SELECT city, min_clearance FROM result_cs1_security ORDER BY city;
DROP TABLE result_cs1_security;

-- Step 5: EXCEPT / monus – cities with exactly one agent (formula for Nairobi only)
CREATE TABLE result_cs1_except AS
SELECT city, sr_formula(provenance(), 'agents_name') AS formula
FROM (
    SELECT DISTINCT city FROM agents
  EXCEPT
    SELECT p1.city FROM agents p1
    JOIN agents p2 ON p1.city = p2.city AND p1.id < p2.id
    GROUP BY p1.city
) t;

SELECT remove_provenance('result_cs1_except');
SELECT city, formula FROM result_cs1_except WHERE city = 'Nairobi';
DROP TABLE result_cs1_except;

-- Step 6: where-provenance – trace city column origin in shared-city query
SET provsql.provenance = 'where';

CREATE TABLE result_cs1_where AS
SELECT p1.city,
    regexp_replace(
      where_provenance(provenance()), ':[0-9a-f-]*:', '::', 'g') AS source
FROM agents p1
JOIN agents p2 ON p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city;

SELECT remove_provenance('result_cs1_where');
SELECT city, source FROM result_cs1_where ORDER BY city;
DROP TABLE result_cs1_where;

SET provsql.provenance = 'semiring';

-- Steps 7-8: probabilities – assign and evaluate (possible-worlds)
DO $$ BEGIN
  PERFORM set_prob(provenance(), id / 10.0) FROM agents;
END $$;

CREATE TABLE result_cs1_prob AS
SELECT city,
    ROUND(probability_evaluate(provenance())::numeric, 4) AS prob
FROM (
    SELECT DISTINCT city FROM agents
  EXCEPT
    SELECT p1.city FROM agents p1
    JOIN agents p2 ON p1.city = p2.city AND p1.id < p2.id
    GROUP BY p1.city
) t;

SELECT remove_provenance('result_cs1_prob');
SELECT city, prob FROM result_cs1_prob ORDER BY city;
DROP TABLE result_cs1_prob;

-- Step 9: Monte Carlo (rounded to 1 decimal for stability)
CREATE TABLE result_cs1_mc AS
SELECT city,
    probability_evaluate(provenance(), 'monte-carlo', '10000') AS prob
FROM (
    SELECT DISTINCT city FROM agents
  EXCEPT
    SELECT p1.city FROM agents p1
    JOIN agents p2 ON p1.city = p2.city AND p1.id < p2.id
    GROUP BY p1.city
) t;

SELECT remove_provenance('result_cs1_mc');
-- True probability for Beijing is 0.54 (Ellen XOR Jing = 0.4*0.3 + 0.6*0.7).
-- With N=10000 the SE is ~0.005, so test tolerance >> SE for stable CI.
SELECT city, ABS(prob - 0.54) < 0.05 AS prob_within_tolerance
FROM result_cs1_mc
WHERE city = 'Beijing' ORDER BY city;
DROP TABLE result_cs1_mc;

-- Step 13 (benchmark variant): tree-decomposition exact probability
CREATE TABLE result_cs1_td AS
SELECT city,
    ROUND(probability_evaluate(provenance(), 'tree-decomposition')::numeric, 4) AS prob
FROM (
    SELECT DISTINCT city FROM agents
  EXCEPT
    SELECT p1.city FROM agents p1
    JOIN agents p2 ON p1.city = p2.city AND p1.id < p2.id
    GROUP BY p1.city
) t;

SELECT remove_provenance('result_cs1_td');
SELECT city, prob FROM result_cs1_td ORDER BY city;
DROP TABLE result_cs1_td;

-- Step 14: sr_boolexpr – abstract Boolean formula on the Nairobi monus token
CREATE TABLE result_cs1_boolexpr AS
SELECT city, sr_boolexpr(provenance()) AS boolexpr
FROM (
    SELECT DISTINCT city FROM agents
  EXCEPT
    SELECT p1.city FROM agents p1
    JOIN agents p2 ON p1.city = p2.city AND p1.id < p2.id
    GROUP BY p1.city
) t
WHERE city = 'Nairobi';

SELECT remove_provenance('result_cs1_boolexpr');
SELECT city, boolexpr FROM result_cs1_boolexpr;
DROP TABLE result_cs1_boolexpr;

-- Step 15: programmatic circuit inspection (get_gate_type, get_children, identify_token)
CREATE TABLE nairobi_token AS
SELECT provenance() AS prov
FROM (
    SELECT DISTINCT city FROM agents
  EXCEPT
    SELECT p1.city FROM agents p1
    JOIN agents p2 ON p1.city = p2.city AND p1.id < p2.id
    GROUP BY p1.city
) t
WHERE city = 'Nairobi';

SELECT remove_provenance('nairobi_token');

SELECT get_nb_gates() > 0 AS has_gates;

SELECT get_gate_type(prov) AS root_type,
       array_length(get_children(prov), 1) AS root_n_children
FROM nairobi_token;

SELECT get_gate_type((get_children(prov))[1]) AS plus_type,
       array_length(get_children((get_children(prov))[1]), 1) AS plus_n_children
FROM nairobi_token;

SELECT (identify_token(child)).table_name AS source_table,
       (identify_token(child)).nb_columns AS nb_columns
FROM nairobi_token,
     unnest(get_children((get_children(prov))[1])) AS child
ORDER BY 1, 2;

-- Step 16: revising a probability.  set_prob writes a probability once;
-- replace_input gives Juma's row a fresh input gate at 0.9, so Nairobi's
-- single-agent probability becomes 0.9 * 0.8 + 0.1 * 0.2 = 0.74, while the
-- token saved above, built from the old gate, still evaluates to 0.26.
UPDATE agents SET provsql = replace_input(provsql, 0.9) WHERE name = 'Juma';
CREATE TABLE result_cs1_revised AS
SELECT city,
    ROUND(probability_evaluate(provenance())::numeric, 4) AS prob
FROM (
    SELECT DISTINCT city FROM agents
  EXCEPT
    SELECT p1.city FROM agents p1
      JOIN agents p2 ON p1.city = p2.city AND p1.id < p2.id
    GROUP BY p1.city
) t;
SELECT remove_provenance('result_cs1_revised');
SELECT city, prob FROM result_cs1_revised ORDER BY city;
DROP TABLE result_cs1_revised;
SELECT ROUND(probability_evaluate(prov)::numeric, 4) AS saved_prob
FROM nairobi_token;

DROP TABLE nairobi_token;

-- Step 17: clearance levels of the EXCEPT query of Step 5.  An analyst sees
-- the agents at or below their level: Nairobi looks single-agent only at
-- unclassified (Paul hidden), Paris only at restricted (Nancy alone),
-- Beijing never; sr_minmax reads these as up-sets.  The mapping is rebuilt
-- since Juma's token changed in Step 16.
DROP TABLE agents_level;
SELECT create_provenance_mapping('agents_level', 'agents', 'classification');
CREATE TABLE result_cs1_clearance AS
SELECT city,
    sr_clearance(provenance(), 'agents_level',
                 'unclassified'::classification_level) AS levels,
    visible_at(provenance(), 'agents_level', 'unclassified') AS at_unclassified,
    (clearance_settling(provenance(), 'agents_level',
                        'unclassified'::classification_level)).*,
    sr_minmax(provenance(), 'agents_level',
              'unclassified'::classification_level) AS minmax
FROM (
    SELECT DISTINCT city FROM agents
  EXCEPT
    SELECT p1.city FROM agents p1
      JOIN agents p2 ON p1.city = p2.city AND p1.id < p2.id
    GROUP BY p1.city
) t;
SELECT remove_provenance('result_cs1_clearance');
SELECT * FROM result_cs1_clearance ORDER BY city;
DROP TABLE result_cs1_clearance;

-- Step 18: the allied services each personnel file is shared with; the
-- services whose view of the files makes a city look single-agent.  MI6 sees
-- Juma and Paul, DGSE only Juma: Nairobi single-agent for DGSE.  Paris: MI6
-- sees Nancy alone (David with DGSE and BND, Aaheli shared with none).
-- Beijing: Ellen for BND, Jing for DGSE, both single-agent.  Over all files,
-- no city is.
CREATE TYPE cs1_ally AS ENUM ('bnd', 'dgse', 'mi6');
CREATE TABLE agents_shared AS
  SELECT provsql AS provenance,
         CASE name WHEN 'Juma'   THEN '{mi6,dgse}'
                   WHEN 'Paul'   THEN '{mi6}'
                   WHEN 'David'  THEN '{dgse,bnd}'
                   WHEN 'Ellen'  THEN '{bnd}'
                   WHEN 'Aaheli' THEN '{}'
                   WHEN 'Nancy'  THEN '{mi6,dgse,bnd}'
                   WHEN 'Jing'   THEN '{dgse}' END::cs1_ally[] AS value
  FROM agents;
CREATE TABLE result_cs1_allies AS
SELECT city,
    sr_subset(provenance(), 'agents_shared', 'mi6'::cs1_ally) AS allies
FROM (
    SELECT DISTINCT city FROM agents
  EXCEPT
    SELECT p1.city FROM agents p1
      JOIN agents p2 ON p1.city = p2.city AND p1.id < p2.id
    GROUP BY p1.city
) t;
SELECT remove_provenance('result_cs1_allies');
SELECT * FROM result_cs1_allies ORDER BY city;
DROP TABLE result_cs1_allies;
DROP TABLE agents_shared;
DROP TYPE cs1_ally;

-- Clean up
DROP TABLE agents_level;
DROP TABLE agents_name;
DROP TABLE agents;
