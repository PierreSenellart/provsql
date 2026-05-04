-- Case Study 5: Wildlife Photo Archive
-- Setup script – load into a fresh PostgreSQL database:
--   psql -d mydb -f setup.sql

SET client_encoding = 'UTF8';

CREATE EXTENSION IF NOT EXISTS "uuid-ossp" WITH SCHEMA public;
CREATE EXTENSION IF NOT EXISTS provsql WITH SCHEMA public;

SET search_path TO public, provsql;

CREATE TABLE photo (
    id integer PRIMARY KEY,
    station text NOT NULL,
    date date NOT NULL,
    filename text NOT NULL
);

CREATE TABLE species (
    id integer PRIMARY KEY,
    name text NOT NULL,
    category text NOT NULL  -- 'mammal', 'bird', 'reptile'
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

-- Detections.  Each row is one (bounding-box, species) candidate produced
-- by the classifier; multiple rows for the same (photo_id, bbox_id) pair
-- represent alternative species hypotheses for that single bounding box.
-- Photo 5: low confidences across multiple bboxes, with one ambiguous box
--          (deer or roe deer); motivates probabilistic ranking in Step 7.
-- Photo 9: deer + high-confidence dog (excluded from "deer, no dogs").
-- Photo 14: deer + low-confidence near-miss dog (interesting EXCEPT case).
-- Photo 22: ambiguous box plus several clear ones (mixed herd at Rannoch).
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
