SET client_encoding = 'UTF8';

CREATE EXTENSION IF NOT EXISTS "uuid-ossp" WITH SCHEMA public;
CREATE EXTENSION IF NOT EXISTS provsql WITH SCHEMA public;

CREATE TYPE public.formula_state AS (
	formula text,
	nbargs integer
);

CREATE FUNCTION public.formula_monus(formula1 text, formula2 text) RETURNS text
    LANGUAGE sql IMMUTABLE STRICT
    AS $$
  SELECT concat('(',formula1,' ⊖ ',formula2,')')
$$;

CREATE FUNCTION public.formula_semimod(formula1 text, formula2 text) RETURNS text
    LANGUAGE sql IMMUTABLE STRICT
    AS $$
  SELECT concat('(',formula1,' * ',formula2,')')
$$;


CREATE FUNCTION public.formula_delta(formula text) RETURNS text
    LANGUAGE sql IMMUTABLE STRICT
    AS $$   
  SELECT concat('δ(',formula,')')
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

CREATE FUNCTION public.formula_agg_state(state public.formula_state, value text) RETURNS public.formula_state
    LANGUAGE plpgsql IMMUTABLE
    AS $$
BEGIN
  IF state IS NULL OR state.nbargs=0 THEN
    RETURN (value,1);
  ELSE
    RETURN (concat(state.formula,' , ',value),state.nbargs+1);
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

CREATE FUNCTION public.agg_state2formula(state public.formula_state) RETURNS text
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

CREATE AGGREGATE public.formula_agg(text) (
    SFUNC = public.formula_agg_state,
    STYPE = public.formula_state,
    INITCOND = '(1,0)'
    --FINALFUNC = public.formula_state2formula
);

CREATE FUNCTION public.formula_agg_final(state public.formula_state, fname varchar) RETURNS text
  LANGUAGE sql IMMUTABLE STRICT
  AS
  $$
    SELECT concat(fname,'{ ',state.formula,' }');
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
    'formula_monus',
    'formula_delta');
END
$$;

CREATE FUNCTION public.aggregation_formula(token anyelement, token2value regclass) RETURNS text
    LANGUAGE plpgsql
    AS $$
BEGIN
  RETURN aggregation_evaluate(
    token::provsql.provenance_token,
    token2value,
    'formula_agg_final',
    'formula_agg',
    'formula_semimod',
    '𝟙'::text,
    'formula_plus',
    'formula_times',
    'formula_monus',
    'formula_delta');
END
$$;
-- Counting semiring

CREATE FUNCTION public.counting_monus(counting1 integer, counting2 integer) RETURNS integer
    LANGUAGE sql IMMUTABLE STRICT
    AS $$
  SELECT CASE WHEN counting1 < counting2 THEN 0 ELSE counting1 - counting2 END
$$;

CREATE FUNCTION public.counting_delta(counting integer) RETURNS integer
    LANGUAGE sql IMMUTABLE STRICT
    AS $$
  SELECT CASE WHEN counting > 0 THEN 1 ELSE 1 END
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
    'counting_monus',
    'counting_delta');
END
$$;

-- Example tables

DROP TABLE public.person;
DROP TABLE public.job;

CREATE TABLE public.person (
    id integer NOT NULL,
    name text NOT NULL,
    age integer,
    job_id integer
);

CREATE TABLE public.job (
    id integer NOT NULL,
    descr text NOT null,
    dept integer,
    hours integer,
    salary double precision
);

COPY public.job (id, descr, dept, hours, salary) FROM stdin;
0	J0	0	0	0.0
1	J1	1	37	17000.0
2	J2	1	40	24000.0
3	J3	2	40	50000.0
4	J4	2	40	65000.0
5	J5	1	37	275000.0
6	J6	2	37	256000.0
\.

COPY public.person (id, name, age, job_id) FROM stdin;
0	Titus	50	5
1	Norah	37	3
2	Ginny	30	2
3	Demetra	62	3
4	Sheri	69	0
5	Karleen	15	0
6	Daisey	17	0
7	Audrey	10	0
8	Alaine	63	3
9	Edwin	31	1
10	Shelli	34	4
11	Santina	18	1
12	Bart	29	3
13	Harriette	65	4
14	Jody	57	6
15	Theodora	22	3
16	Roman	55	4
17	Jack	42	3
18	Daphine	21	1
19	Kyra	53	3
\.

ALTER TABLE ONLY public.person
    ADD CONSTRAINT person_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.job
    ADD CONSTRAINT job_pkey PRIMARY KEY (id);

ALTER TABLE ONLY public.person
    ADD CONSTRAINT person_job_fkey FOREIGN KEY (job_id) REFERENCES public.job(id);

-- provenance

SELECT provsql.add_provenance('public.person');
SELECT provsql.add_provenance('public.job');

SET search_path TO public, provsql;

SELECT create_provenance_mapping('person_mapping','person','name');
SELECT create_provenance_mapping('job_mapping','job','descr');
CREATE TABLE formula_mapping AS SELECT * FROM person_mapping UNION SELECT * FROM job_mapping;
