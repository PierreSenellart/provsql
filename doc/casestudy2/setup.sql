SET client_encoding = 'UTF8';

CREATE EXTENSION IF NOT EXISTS "uuid-ossp" WITH SCHEMA public;
CREATE EXTENSION IF NOT EXISTS provsql WITH SCHEMA public;

SET search_path TO public, provsql;

CREATE TYPE study_quality AS ENUM ('case_report', 'observational', 'rct', 'meta_analysis');

CREATE TABLE study (
    id integer PRIMARY KEY,
    title text NOT NULL,
    year integer,
    study_type study_quality NOT NULL,
    reliability double precision NOT NULL
);
CREATE TABLE exposure (id integer PRIMARY KEY, name text NOT NULL);
CREATE TABLE outcome  (id integer PRIMARY KEY, name text NOT NULL);
CREATE TABLE finding (
    id integer PRIMARY KEY,
    study_id integer NOT NULL,
    exposure_id integer NOT NULL,
    outcome_id integer NOT NULL,
    effect text NOT NULL,
    effect_size double precision
);

COPY study (id, title, year, study_type, reliability) FROM stdin;
1	Smith2018	2018	rct	0.92
2	Johnson2020	2020	meta_analysis	0.98
3	Chen2019	2019	observational	0.76
4	Williams2021	2021	rct	0.88
5	Garcia2017	2017	observational	0.65
6	Brown2022	2022	meta_analysis	0.95
7	Martinez2020	2020	case_report	0.45
8	Park2021	2021	rct	0.85
\.

COPY exposure (id, name) FROM stdin;
1	Coffee
2	Exercise
3	Red Meat
4	Aspirin
5	Omega-3
6	Alcohol
7	Processed Food
\.

COPY outcome (id, name) FROM stdin;
1	Cardiovascular Disease
2	Type 2 Diabetes
3	Inflammation
4	Colorectal Cancer
5	Cognitive Decline
\.

-- 25 findings; contradictions on Coffee→CVD and Alcohol→CVD;
-- single-study claims on Aspirin→Cognitive Decline, Omega-3→CVD, Exercise→Inflammation.
COPY finding (id, study_id, exposure_id, outcome_id, effect, effect_size) FROM stdin;
1	5	1	1	harmful	1.3
2	6	1	1	beneficial	0.85
3	3	1	1	neutral	1.0
4	1	2	1	beneficial	0.60
5	2	2	1	beneficial	0.55
6	4	2	1	beneficial	0.65
7	2	3	4	harmful	1.50
8	3	3	4	harmful	1.40
9	1	4	1	beneficial	0.75
10	4	4	1	beneficial	0.78
11	6	5	3	beneficial	0.70
12	8	5	3	beneficial	0.72
13	5	6	1	beneficial	0.90
14	7	6	1	harmful	1.20
15	2	7	2	harmful	1.80
16	3	7	2	harmful	1.60
17	6	1	5	beneficial	0.80
18	8	1	5	beneficial	0.75
19	3	3	3	harmful	1.30
20	7	3	3	harmful	1.25
21	4	6	2	harmful	1.40
22	5	6	2	harmful	1.35
23	8	4	5	beneficial	0.60
24	6	5	1	beneficial	0.80
25	1	2	3	beneficial	0.70
\.
