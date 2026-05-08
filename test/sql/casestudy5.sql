\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Case Study 5: The Wildlife Photo Archive
-- Tests VALUES, repair_key with synthetic key, mulinput probability,
-- EXCEPT with monus, CTEs, and expected() aggregates.

CREATE TABLE photo (
    id integer PRIMARY KEY,
    station text NOT NULL,
    date date NOT NULL,
    filename text NOT NULL
);
CREATE TABLE species (
    id integer PRIMARY KEY,
    name text NOT NULL,
    category text NOT NULL
);
CREATE TABLE detection (
    photo_id   integer NOT NULL,
    bbox_id    integer NOT NULL,
    species_id integer NOT NULL,
    confidence double precision NOT NULL
);

COPY photo (id, station, date, filename) FROM stdin;
1	Loch Torridon	2024-06-15	LT-20240615-0001.jpg
2	Loch Torridon	2024-06-15	LT-20240615-0002.jpg
3	Loch Torridon	2024-07-02	LT-20240702-0001.jpg
4	Loch Torridon	2024-07-15	LT-20240715-0001.jpg
5	Loch Torridon	2024-08-03	LT-20240803-0001.jpg
6	Loch Torridon	2024-08-12	LT-20240812-0001.jpg
7	Loch Torridon	2024-08-20	LT-20240820-0001.jpg
8	Loch Torridon	2024-09-01	LT-20240901-0001.jpg
9	Glen Affric	2024-06-20	GA-20240620-0001.jpg
10	Glen Affric	2024-07-04	GA-20240704-0001.jpg
11	Glen Affric	2024-07-15	GA-20240715-0001.jpg
12	Glen Affric	2024-08-08	GA-20240808-0001.jpg
13	Glen Affric	2024-08-25	GA-20240825-0001.jpg
14	Glen Affric	2024-09-10	GA-20240910-0001.jpg
15	Glen Affric	2024-09-22	GA-20240922-0001.jpg
16	Rannoch Moor	2024-06-10	RM-20240610-0001.jpg
17	Rannoch Moor	2024-06-25	RM-20240625-0001.jpg
18	Rannoch Moor	2024-07-12	RM-20240712-0001.jpg
19	Rannoch Moor	2024-07-30	RM-20240730-0001.jpg
20	Rannoch Moor	2024-08-15	RM-20240815-0001.jpg
21	Rannoch Moor	2024-09-02	RM-20240902-0001.jpg
22	Rannoch Moor	2024-09-15	RM-20240915-0001.jpg
23	Cairngorms	2024-06-18	CG-20240618-0001.jpg
24	Cairngorms	2024-07-01	CG-20240701-0001.jpg
25	Cairngorms	2024-07-20	CG-20240720-0001.jpg
26	Cairngorms	2024-08-05	CG-20240805-0001.jpg
27	Cairngorms	2024-08-22	CG-20240822-0001.jpg
28	Cairngorms	2024-09-08	CG-20240908-0001.jpg
29	Cairngorms	2024-09-20	CG-20240920-0001.jpg
30	Cairngorms	2024-10-01	CG-20241001-0001.jpg
\.

COPY species (id, name, category) FROM stdin;
1	Red Deer	mammal
2	Roe Deer	mammal
3	Red Fox	mammal
4	Mountain Hare	mammal
5	Pine Marten	mammal
6	Red Squirrel	mammal
7	Golden Eagle	bird
8	Osprey	bird
9	Capercaillie	bird
10	Adder	reptile
11	Common Lizard	reptile
12	Wildcat	mammal
13	Domestic Dog	mammal
\.

COPY detection (photo_id, bbox_id, species_id, confidence) FROM stdin;
1	1	1	0.78
1	2	1	0.65
1	3	1	0.71
2	1	1	0.85
2	2	3	0.72
3	1	3	0.68
3	2	6	0.55
4	1	5	0.81
5	1	1	0.40
5	1	2	0.30
5	2	1	0.42
5	3	1	0.38
5	4	3	0.43
5	5	3	0.48
6	1	7	0.91
7	1	1	0.76
7	2	1	0.62
7	3	1	0.59
7	4	2	0.58
8	1	9	0.83
9	1	1	0.79
9	2	13	0.92
10	1	1	0.73
10	2	3	0.66
11	1	5	0.74
11	2	6	0.62
12	1	3	0.81
12	2	4	0.55
13	1	12	0.71
14	1	1	0.84
14	2	13	0.18
15	1	7	0.86
15	2	8	0.79
16	1	10	0.62
16	2	11	0.58
17	1	3	0.74
17	2	4	0.66
18	1	1	0.61
18	2	3	0.55
19	1	9	0.78
20	1	7	0.89
21	1	11	0.53
21	2	10	0.49
22	1	1	0.82
22	2	1	0.71
22	3	3	0.77
22	3	1	0.20
22	4	3	0.64
22	5	2	0.69
22	6	2	0.55
23	1	5	0.71
23	2	6	0.68
24	1	1	0.74
24	2	4	0.62
25	1	7	0.88
25	2	9	0.74
26	1	12	0.65
27	1	8	0.83
28	1	1	0.76
28	2	3	0.69
29	1	5	0.79
30	1	6	0.71
30	2	4	0.59
\.

-- Step 2: enable provenance, build a name mapping by hand
-- (a provenance mapping is just any (value, provenance) table)
SELECT add_provenance('detection');
CREATE TABLE species_mapping AS
  SELECT s.name AS value, d.provsql AS provenance
  FROM detection d JOIN species s ON s.id = d.species_id;
SELECT remove_provenance('species_mapping');
CREATE INDEX ON species_mapping(provenance);

-- Step 3: VALUES clause — provenance propagates from detection through join
CREATE TABLE result_cs5_values AS
SELECT v.label, p.id,
       sr_formula(provenance(), 'species_mapping') AS formula
FROM (VALUES (1, 'mammal of interest'),
             (3, 'predator of interest')) AS v(species_id, label),
     detection d, photo p
WHERE d.species_id = v.species_id AND d.photo_id = p.id;

SELECT remove_provenance('result_cs5_values');
SELECT label, id, formula FROM result_cs5_values
WHERE id IN (2, 5)
ORDER BY id, label;
DROP TABLE result_cs5_values;

-- Step 4: naive conjunction (Red Deer ⊗ Red Fox); inflated formula on photo 5
CREATE TABLE result_cs5_naive AS
SELECT p.id,
       sr_formula(provenance(), 'species_mapping') AS formula
FROM detection d1
JOIN detection d2 ON d1.photo_id = d2.photo_id
JOIN photo p ON p.id = d1.photo_id
WHERE d1.species_id = 1 AND d2.species_id = 3
GROUP BY p.id;

SELECT remove_provenance('result_cs5_naive');
SELECT id, formula FROM result_cs5_naive
WHERE id IN (2, 5)
ORDER BY id;
DROP TABLE result_cs5_naive;

-- Step 5: repair_key replaces input gates with mulinput key gates
DROP TABLE species_mapping;
SELECT remove_provenance('detection');

ALTER TABLE detection ADD COLUMN photo_bbox text;
UPDATE detection SET photo_bbox = photo_id || '/' || bbox_id;
SELECT repair_key('detection', 'photo_bbox');

CREATE TABLE species_mapping AS
  SELECT s.name AS value, d.provsql AS provenance
  FROM detection d JOIN species s ON s.id = d.species_id;
SELECT remove_provenance('species_mapping');
CREATE INDEX ON species_mapping(provenance);

-- After repair_key: structural change visible by inspecting the gate types
-- of leaf inputs.  Each detection row is now a mulinput, sharing one key
-- per (photo, species) pair.
CREATE TABLE result_cs5_gate_types AS
SELECT photo_id, species_id, get_gate_type(provsql) AS gtype
FROM detection
WHERE photo_id IN (2, 5);

SELECT remove_provenance('result_cs5_gate_types');
SELECT photo_id, species_id, gtype FROM result_cs5_gate_types
ORDER BY photo_id, species_id, gtype;
DROP TABLE result_cs5_gate_types;

-- Step 6: set probabilities from confidence and verify mutual exclusion
DO $$ BEGIN
  PERFORM set_prob(provenance(), confidence) FROM detection;
END $$;

-- Demo of repair_key's numerical effect: bbox 1 of photo 5 has two
-- candidates (deer 0.40, roe deer 0.30); the mulinput sums them to 0.70,
-- vs 1 - (1-0.40)(1-0.30) = 0.58 if they were independent.
CREATE TABLE result_cs5_mutex AS
SELECT photo_id, bbox_id,
    sr_boolexpr(provenance()) AS bexpr,
    ROUND(probability_evaluate(provenance(), 'tree-decomposition')::numeric, 4) AS p
FROM detection
WHERE photo_id = 5 AND bbox_id = 1 AND species_id IN (1, 2)
GROUP BY photo_id, bbox_id;
SELECT remove_provenance('result_cs5_mutex');
SELECT photo_id, bbox_id, bexpr, p FROM result_cs5_mutex;
DROP TABLE result_cs5_mutex;

-- Step 7: probabilistic ranking
CREATE TABLE result_cs5_pqe AS
SELECT p.id,
    ROUND(probability_evaluate(provenance(), 'tree-decomposition')::numeric, 4) AS prob
FROM detection d1
JOIN detection d2 ON d1.photo_id = d2.photo_id
JOIN photo p ON p.id = d1.photo_id
WHERE d1.species_id = 1 AND d2.species_id = 3
GROUP BY p.id;

SELECT remove_provenance('result_cs5_pqe');
SELECT id, prob FROM result_cs5_pqe ORDER BY prob DESC, id;
DROP TABLE result_cs5_pqe;

-- Step 7 (continued): naive threshold filter
CREATE TABLE result_cs5_threshold AS
SELECT DISTINCT p.id
FROM detection d1
JOIN detection d2 ON d1.photo_id = d2.photo_id
JOIN photo p ON p.id = d1.photo_id
WHERE d1.species_id = 1 AND d2.species_id = 3
  AND d1.confidence >= 0.5 AND d2.confidence >= 0.5;

SELECT remove_provenance('result_cs5_threshold');
SELECT id FROM result_cs5_threshold ORDER BY id;
DROP TABLE result_cs5_threshold;

-- Step 8: EXCEPT — Red Deer minus Domestic Dog, with monus discount
CREATE TABLE result_cs5_except AS
SELECT p.id,
    ROUND(probability_evaluate(provenance(), 'tree-decomposition')::numeric, 4) AS prob
FROM (
    SELECT photo_id FROM detection WHERE species_id = 1
  EXCEPT
    SELECT photo_id FROM detection WHERE species_id = 13
) t
JOIN photo p ON p.id = t.photo_id
GROUP BY p.id;

SELECT remove_provenance('result_cs5_except');
SELECT id, prob FROM result_cs5_except
WHERE id IN (9, 14, 22)  -- contrast dog-affected (9, 14) vs clean (22)
ORDER BY id;
DROP TABLE result_cs5_except;

-- Step 9: CTE — deer AND fox AND no dogs, ranked by probability
CREATE TABLE result_cs5_cte AS
WITH deer_and_fox AS (
  SELECT d1.photo_id
  FROM detection d1
  JOIN detection d2 ON d1.photo_id = d2.photo_id
  WHERE d1.species_id = 1 AND d2.species_id = 3
  GROUP BY d1.photo_id
),
no_dogs AS (
  SELECT photo_id FROM deer_and_fox
  EXCEPT
  SELECT photo_id FROM detection WHERE species_id = 13
)
SELECT p.id,
    ROUND(probability_evaluate(provenance(), 'tree-decomposition')::numeric, 4) AS prob
FROM no_dogs t
JOIN photo p ON p.id = t.photo_id;

SELECT remove_provenance('result_cs5_cte');
SELECT id, prob FROM result_cs5_cte ORDER BY prob DESC, id;
DROP TABLE result_cs5_cte;

-- Step 10: expected aggregates
CREATE TABLE result_cs5_expected AS
SELECT p.id,
    ROUND(expected(COUNT(*))::numeric, 4) AS exp_detections,
    ROUND(expected(SUM(d.confidence))::numeric, 4) AS exp_total_conf
FROM detection d
JOIN photo p ON p.id = d.photo_id
GROUP BY p.id;

SELECT remove_provenance('result_cs5_expected');
SELECT id, exp_detections, exp_total_conf FROM result_cs5_expected
WHERE id IN (1, 5, 7, 9, 22)  -- representative photos
ORDER BY id;
DROP TABLE result_cs5_expected;

-- Clean up
DROP TABLE species_mapping;
DROP TABLE detection;
DROP TABLE species;
DROP TABLE photo;
