SET client_encoding = 'UTF8';

CREATE EXTENSION IF NOT EXISTS "uuid-ossp" WITH SCHEMA public;
CREATE EXTENSION IF NOT EXISTS provsql WITH SCHEMA public;


-- Formula semiring

CREATE TYPE public.formula_state AS (
	formula text,
	nbargs integer
);

CREATE FUNCTION public.formula_monus(formula1 text, formula2 text) RETURNS text
    LANGUAGE sql IMMUTABLE STRICT
    AS $$
  SELECT concat('(',formula1,' ⊖ ',formula2,')')
$$;

CREATE FUNCTION public.formula_plus_state(state public.formula_state, value text) RETURNS public.formula_state
    LANGUAGE plpgsql IMMUTABLE
    AS $$
BEGIN
  IF state IS NULL OR state.nbargs=0 THEN
    RETURN (value,1);
  ELSE
    RETURN (concat(state.formula,' ⊕ ',value),state.nbargs+1);
  END IF;
END
$$;

CREATE FUNCTION public.formula_state2formula(state public.formula_state) RETURNS text
    LANGUAGE sql IMMUTABLE STRICT
    AS $$
  SELECT
    CASE
      WHEN state.nbargs<2 THEN state.formula
      ELSE concat('(',state.formula,')')
    END;
$$;

CREATE FUNCTION public.formula_times_state(state public.formula_state, value text) RETURNS public.formula_state
    LANGUAGE plpgsql IMMUTABLE
    AS $$
BEGIN    
  IF state IS NULL OR state.nbargs=0 THEN
    RETURN (value,1);
  ELSE
    RETURN (concat(state.formula,' ⊗ ',value),state.nbargs+1);
  END IF;
END
$$;

CREATE FUNCTION public.formula(token provsql.provenance_token, token2value regclass) RETURNS text
    LANGUAGE plpgsql
    AS $$
BEGIN
  RETURN provenance_evaluate(
    token,
    token2value,
    '𝟙'::text,
    'formula_plus',
    'formula_times',
    'formula_monus');
END
$$;



-- Counting semiring

CREATE FUNCTION public.counting(token provsql.provenance_token, token2value regclass) RETURNS integer
    LANGUAGE plpgsql
    AS $$
BEGIN
  RETURN provenance_evaluate(
    token,
    token2value,
    1,
    'counting_plus',
    'counting_times',
    'counting_monus');
END
$$;

CREATE FUNCTION public.counting_monus(counting1 integer, counting2 integer) RETURNS integer
    LANGUAGE sql IMMUTABLE STRICT
    AS $$
  SELECT CASE WHEN counting1 < counting2 THEN 0 ELSE counting1 - counting2 END
$$;

CREATE FUNCTION public.counting_plus_state(state integer, value integer) RETURNS integer
    LANGUAGE sql IMMUTABLE
    AS $$
  SELECT CASE WHEN state IS NULL THEN value ELSE state + value END
$$;

CREATE FUNCTION public.counting_times_state(state integer, value integer) RETURNS integer
    LANGUAGE sql IMMUTABLE
    AS $$
SELECT CASE WHEN state IS NULL THEN value ELSE state * value END
$$;

CREATE AGGREGATE public.counting_plus(integer) (
    SFUNC = public.counting_plus_state,
    STYPE = integer,
    INITCOND = '0'
);

CREATE AGGREGATE public.counting_times(integer) (
    SFUNC = public.counting_times_state,
    STYPE = integer,
    INITCOND = '1'
);

CREATE AGGREGATE public.formula_plus(text) (
    SFUNC = public.formula_plus_state,
    STYPE = public.formula_state,
    INITCOND = '(𝟘,0)',
    FINALFUNC = public.formula_state2formula
);

CREATE AGGREGATE public.formula_times(text) (
    SFUNC = public.formula_times_state,
    STYPE = public.formula_state,
    INITCOND = '(𝟙,0)',
    FINALFUNC = public.formula_state2formula
);


-- Example tables

CREATE TABLE public.person (
    id integer NOT NULL,
    name text NOT NULL,
    date_of_birth date,
    height smallint
);

CREATE TABLE public.reliability (
    person integer NOT NULL,
    score double precision NOT NULL
);

CREATE TABLE public.room (
    id integer NOT NULL,
    name text NOT NULL,
    area smallint
);

CREATE TABLE public.sightings (
    "time" time without time zone NOT NULL,
    person integer NOT NULL,
    room integer NOT NULL,
    witness integer
);

COPY public.person (id, name, date_of_birth, height) FROM stdin;
0	Titus	1969-04-03	163
1	Norah	1983-10-15	194
2	Ginny	1989-10-23	169
3	Demetra	1957-07-20	167
4	Sheri	1950-10-19	195
5	Karleen	2004-09-01	199
6	Daisey	2002-08-19	163
7	Audrey	2009-12-20	167
8	Alaine	1956-09-07	192
9	Edwin	1987-02-21	210
10	Shelli	1985-03-05	195
11	Santina	1991-09-04	164
12	Bart	1989-08-12	163
13	Harriette	1959-06-24	160
14	Jody	1962-12-18	202
15	Theodora	1995-11-08	204
16	Roman	1964-12-14	171
17	Jack	1976-06-11	167
18	Daphine	1998-09-21	191
19	Kyra	1966-05-04	202
\.

COPY public.reliability (person, score) FROM stdin;
0	0.23828493492944236
1	0.657319818187148019
2	0.745325911826738019
3	0.656730287512349964
4	0.942979116189337052
5	0.600921893448834954
6	0.874435606539356036
7	0.990416985535926053
8	0.59251775051353095
9	0.688247502287665958
10	0.939401152561129993
11	0.960847979674174013
12	0.818769283596453956
13	0.834442059579594053
14	0.788371825897704048
15	0.620618845450902956
16	0.977769943596806024
17	0.840542782838639035
19	0.681836780693319988
\.

COPY public.room (id, name, area) FROM stdin;
0	Dining room	23
1	Blue bedroom	20
2	Red bedroom	31
3	Yellow bedroom	27
4	Green bedroom	37
5	Living room	14
6	Kitchen	18
7	First bathroom	26
8	Second bathroom	34
9	Library	27
\.

COPY public.sightings ("time", person, room, witness) FROM stdin;
02:30:00	19	8	0
05:00:00	11	9	0
03:00:00	19	2	0
13:00:00	8	8	0
22:30:00	5	1	0
05:30:00	19	5	0
16:00:00	11	8	0
18:30:00	11	9	0
13:30:00	11	4	0
13:30:00	6	7	0
05:30:00	13	3	0
10:00:00	5	0	0
04:00:00	14	6	0
01:30:00	12	1	0
15:00:00	1	5	0
21:00:00	16	6	0
06:30:00	17	6	1
01:30:00	10	5	1
09:30:00	9	7	1
07:00:00	17	1	1
10:30:00	3	3	1
01:00:00	18	2	1
09:00:00	17	2	1
05:30:00	18	6	1
04:30:00	16	2	2
15:30:00	14	8	2
19:00:00	1	8	2
22:00:00	5	9	2
22:30:00	0	0	2
18:00:00	10	3	2
06:00:00	11	5	2
05:00:00	17	8	2
17:00:00	14	3	2
17:30:00	12	9	2
22:30:00	10	3	2
21:00:00	5	7	2
09:00:00	9	4	2
08:30:00	18	2	2
10:00:00	13	3	2
23:00:00	7	9	2
13:30:00	5	6	3
19:00:00	16	3	3
03:00:00	16	4	3
12:30:00	16	0	3
20:30:00	8	0	3
14:00:00	15	1	3
22:00:00	8	3	3
10:00:00	15	7	3
11:00:00	15	3	3
00:00:00	15	4	3
22:00:00	14	9	3
02:30:00	15	7	4
08:00:00	11	6	4
15:00:00	13	3	4
20:00:00	8	7	4
21:00:00	7	3	4
19:00:00	15	7	4
22:30:00	9	6	5
06:00:00	0	1	5
02:30:00	0	5	5
17:30:00	1	1	5
18:00:00	7	4	5
04:30:00	18	3	5
14:30:00	17	9	5
21:30:00	15	4	5
10:00:00	1	9	5
03:00:00	3	0	5
05:30:00	3	8	5
19:30:00	17	4	6
16:30:00	0	5	7
11:00:00	1	9	7
13:30:00	18	8	7
13:00:00	12	9	7
19:30:00	3	9	7
20:30:00	3	0	7
15:00:00	6	3	7
19:30:00	6	7	7
19:30:00	10	5	7
13:00:00	5	3	8
15:00:00	14	2	8
01:00:00	1	6	8
08:00:00	7	4	8
09:30:00	12	1	8
20:30:00	12	9	8
10:30:00	11	9	8
06:30:00	7	0	8
11:30:00	13	1	8
15:30:00	5	0	9
04:00:00	6	1	9
22:30:00	2	5	9
01:00:00	8	6	9
13:30:00	15	1	9
07:00:00	19	9	9
21:00:00	7	2	9
18:00:00	0	3	9
08:30:00	14	0	9
03:30:00	11	9	9
05:30:00	3	6	9
20:00:00	15	3	9
06:00:00	7	6	9
16:00:00	14	1	9
19:00:00	7	1	10
12:00:00	17	8	10
09:00:00	8	7	10
21:00:00	8	1	10
01:00:00	8	0	10
09:30:00	17	5	10
08:00:00	3	4	10
21:00:00	18	1	10
22:00:00	3	4	10
11:00:00	15	2	10
01:30:00	18	1	10
08:00:00	14	0	10
06:00:00	7	2	10
04:00:00	18	2	10
21:00:00	12	4	10
01:00:00	4	0	10
18:30:00	13	4	10
22:00:00	5	1	10
23:30:00	11	6	10
04:30:00	5	3	11
04:30:00	12	2	11
13:30:00	7	1	11
08:30:00	7	9	11
00:00:00	6	7	11
11:30:00	6	1	11
20:00:00	6	2	11
07:00:00	9	6	11
10:00:00	16	2	11
04:00:00	8	0	11
07:30:00	15	1	11
20:30:00	10	9	11
19:30:00	3	0	11
04:30:00	4	7	11
12:00:00	1	0	11
23:30:00	17	9	11
18:00:00	4	4	11
21:00:00	0	6	11
14:30:00	17	8	12
07:30:00	10	8	12
05:30:00	13	7	12
19:30:00	18	5	12
21:30:00	8	4	12
21:30:00	11	5	12
13:30:00	3	9	12
08:00:00	2	2	12
08:00:00	5	1	12
01:00:00	13	7	12
15:00:00	19	3	12
21:30:00	3	3	12
11:00:00	12	2	13
04:30:00	3	0	13
02:30:00	3	9	13
05:30:00	5	5	13
01:00:00	1	5	13
09:00:00	15	2	13
22:00:00	18	7	13
18:30:00	7	7	13
08:30:00	18	7	13
09:30:00	6	1	13
21:00:00	6	5	13
16:30:00	19	5	13
15:30:00	1	6	14
07:30:00	7	9	14
04:30:00	13	2	14
10:00:00	17	9	14
07:30:00	12	5	14
15:30:00	8	6	14
10:00:00	18	9	14
18:00:00	0	6	14
17:30:00	2	7	14
18:30:00	5	5	14
04:00:00	4	8	14
12:30:00	7	4	14
00:30:00	19	5	14
14:30:00	9	1	14
09:00:00	3	9	14
14:00:00	7	2	14
00:30:00	12	6	14
16:00:00	8	4	15
23:00:00	12	1	15
13:30:00	18	2	15
11:30:00	2	4	15
00:00:00	10	9	15
00:30:00	3	7	15
03:30:00	3	1	15
00:30:00	0	2	15
16:30:00	10	2	15
08:00:00	8	6	15
06:00:00	2	2	15
03:00:00	13	1	15
06:00:00	8	5	15
15:00:00	18	3	15
01:30:00	3	0	15
02:30:00	5	8	15
22:30:00	19	7	15
22:00:00	15	4	16
10:30:00	0	5	17
17:00:00	1	5	17
12:30:00	5	3	17
00:00:00	19	7	17
12:00:00	1	7	17
16:00:00	5	7	17
14:00:00	3	8	17
14:30:00	14	0	17
04:00:00	6	3	19
11:00:00	4	5	19
15:30:00	5	5	19
\.

ALTER TABLE ONLY public.person
    ADD CONSTRAINT person_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.reliability
    ADD CONSTRAINT reliability_pkey PRIMARY KEY (person);

ALTER TABLE ONLY public.room
    ADD CONSTRAINT room_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.reliability
    ADD CONSTRAINT reliability_person_fkey FOREIGN KEY (person) REFERENCES public.person(id);

ALTER TABLE ONLY public.sightings
    ADD CONSTRAINT sightings_person_fkey FOREIGN KEY (person) REFERENCES public.person(id);

ALTER TABLE ONLY public.sightings
    ADD CONSTRAINT sightings_room_fkey FOREIGN KEY (room) REFERENCES public.room(id);

ALTER TABLE ONLY public.sightings
    ADD CONSTRAINT sightings_witness_fkey FOREIGN KEY (witness) REFERENCES public.person(id);
