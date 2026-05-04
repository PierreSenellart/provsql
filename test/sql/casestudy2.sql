\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

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

-- Step 2: Enable provenance on finding, then join with lookup tables;
-- ProvSQL propagates the finding token through the join into f.
SELECT add_provenance('finding');

CREATE VIEW f AS
  SELECT study.title    AS study,
         study.study_type,
         study.reliability,
         exposure.name  AS exposure,
         outcome.name   AS outcome,
         finding.effect
  FROM finding
    JOIN study    ON finding.study_id    = study.id
    JOIN exposure ON finding.exposure_id = exposure.id
    JOIN outcome  ON finding.outcome_id  = outcome.id;

-- Step 3: Create provenance mapping on f (tokens trace back to finding rows)
SELECT create_provenance_mapping('study_mapping', 'f', 'study');

-- Step 4: sr_formula for three single-study findings
CREATE TABLE result_single AS
SELECT exposure, outcome, effect,
    sr_formula(provenance(), 'study_mapping') AS formula
FROM f
WHERE (exposure = 'Coffee'   AND outcome = 'Cardiovascular Disease' AND effect = 'harmful')
   OR (exposure = 'Alcohol'  AND outcome = 'Cardiovascular Disease' AND effect = 'beneficial')
   OR (exposure = 'Exercise' AND outcome = 'Inflammation'           AND effect = 'beneficial')
GROUP BY exposure, outcome, effect
ORDER BY exposure, outcome, effect;

SELECT remove_provenance('result_single');
SELECT * FROM result_single ORDER BY exposure, outcome, effect;
DROP TABLE result_single;

-- Step 5: Why-provenance for two replicated multi-study findings
CREATE TABLE result_why AS
SELECT exposure, outcome, effect,
    sr_why(provenance(), 'study_mapping') AS witnesses
FROM f
WHERE (exposure = 'Exercise' AND outcome = 'Cardiovascular Disease')
   OR (exposure = 'Aspirin'  AND outcome = 'Cardiovascular Disease')
GROUP BY exposure, outcome, effect
ORDER BY exposure, outcome, effect;

SELECT remove_provenance('result_why');
SELECT * FROM result_why ORDER BY exposure, outcome, effect;
DROP TABLE result_why;

-- Step 6: Evidence grade semiring — best study type supporting a finding.
CREATE FUNCTION quality_plus_state(state study_quality, q study_quality)
  RETURNS study_quality AS $$
    SELECT GREATEST(state, q)
$$ LANGUAGE SQL IMMUTABLE;

CREATE FUNCTION quality_times_state(state study_quality, q study_quality)
  RETURNS study_quality AS $$
    SELECT LEAST(state, q)
$$ LANGUAGE SQL IMMUTABLE;

CREATE AGGREGATE quality_plus(study_quality) (
  sfunc = quality_plus_state, stype = study_quality, initcond = 'case_report'
);
CREATE AGGREGATE quality_times(study_quality) (
  sfunc = quality_times_state, stype = study_quality, initcond = 'meta_analysis'
);

CREATE FUNCTION evidence_grade(token UUID, token2value regclass)
  RETURNS study_quality AS $$
BEGIN
  RETURN provenance_evaluate(
    token, token2value,
    'meta_analysis'::study_quality,
    'quality_plus', 'quality_times'
  );
END
$$ LANGUAGE plpgsql;

SELECT create_provenance_mapping('quality_mapping', 'f', 'study_type');

CREATE TABLE result_grade AS
SELECT exposure, outcome, effect,
    evidence_grade(provenance(), 'quality_mapping') AS grade
FROM f
GROUP BY exposure, outcome, effect
ORDER BY exposure, outcome, effect;

SELECT remove_provenance('result_grade');
SELECT * FROM result_grade ORDER BY exposure, outcome, effect;
DROP TABLE result_grade;

-- Step 7: Where-provenance on f — only finding columns are tracked
SET provsql.where_provenance = on;

CREATE TABLE result_where_f AS
SELECT study, study_type, exposure, outcome, effect,
    regexp_replace(
      where_provenance(provenance()), ':[0-9a-f-]*:', '::', 'g') AS source
FROM f
WHERE exposure = 'Exercise' AND outcome = 'Cardiovascular Disease'
  AND effect = 'beneficial' AND study = 'Smith2018';

SELECT remove_provenance('result_where_f');
SELECT study, study_type, exposure, outcome, effect, source FROM result_where_f;
DROP TABLE result_where_f;

SET provsql.where_provenance = off;

-- Step 8: Where-provenance on the base table — all finding columns tracked
SET provsql.where_provenance = on;

CREATE TABLE result_where AS
SELECT finding.study_id, finding.exposure_id, finding.outcome_id, finding.effect,
    regexp_replace(
      where_provenance(provenance()), ':[0-9a-f-]*:', '::', 'g') AS source
FROM finding
JOIN study    ON finding.study_id    = study.id    AND study.title    = 'Smith2018'
JOIN exposure ON finding.exposure_id = exposure.id AND exposure.name  = 'Exercise'
JOIN outcome  ON finding.outcome_id  = outcome.id  AND outcome.name   = 'Cardiovascular Disease'
WHERE finding.effect = 'beneficial';

SELECT remove_provenance('result_where');
SELECT study_id, exposure_id, outcome_id, effect, source FROM result_where;
DROP TABLE result_where;

SET provsql.where_provenance = off;

-- Step 9: Assign probabilities from reliability scores
DO $$ BEGIN
  PERFORM set_prob(provenance(), reliability) FROM f;
END $$;

-- Probability that at least one study reports a finding (no replication requirement)
CREATE TABLE result_prob AS
SELECT exposure, outcome, effect,
    ROUND(probability_evaluate(provenance())::numeric, 4) AS prob
FROM f
WHERE (exposure = 'Exercise' AND outcome = 'Cardiovascular Disease' AND effect = 'beneficial')
   OR (exposure = 'Coffee'   AND outcome = 'Cardiovascular Disease' AND effect = 'harmful')
   OR (exposure = 'Aspirin'  AND outcome = 'Cognitive Decline'      AND effect = 'beneficial')
GROUP BY exposure, outcome, effect
ORDER BY exposure, outcome, effect;

SELECT remove_provenance('result_prob');
SELECT * FROM result_prob ORDER BY exposure, outcome, effect;
DROP TABLE result_prob;

-- Step 10: Build f_replicated view with HAVING replication threshold
CREATE VIEW f_replicated AS
SELECT exposure, outcome, effect FROM f
GROUP BY exposure, outcome, effect
HAVING COUNT(*) >= 2;

-- Step 11: sr_counting to inspect zero vs. non-zero provenance
ALTER TABLE finding ADD COLUMN cnt int DEFAULT 1;
SELECT create_provenance_mapping('count_mapping', 'finding', 'cnt');

CREATE TABLE result_replicated AS
SELECT exposure, outcome, effect,
    sr_counting(provenance(), 'count_mapping') AS replicated
FROM f_replicated
ORDER BY exposure, outcome, effect;

SELECT remove_provenance('result_replicated');
SELECT * FROM result_replicated ORDER BY exposure, outcome, effect;
DROP TABLE result_replicated;

-- Step 12: Probability of replication (single-study findings → 0 via f_replicated)
CREATE TABLE result_replication AS
SELECT exposure, outcome, effect,
    ROUND(probability_evaluate(provenance())::numeric, 4) AS prob
FROM f_replicated
ORDER BY exposure, outcome, effect;

SELECT remove_provenance('result_replication');
SELECT * FROM result_replication ORDER BY exposure, outcome, effect;
DROP TABLE result_replication;

-- Step 13: Shapley values — which study drives Exercise→CVD→beneficial?
CREATE TABLE result_shapley AS
SELECT fin.study,
       shapley(target.prov, fin.prov) AS sv
FROM (
  SELECT provenance() AS prov
  FROM (
    SELECT DISTINCT exposure, outcome, effect FROM f
    WHERE exposure = 'Exercise' AND outcome = 'Cardiovascular Disease'
      AND effect = 'beneficial'
  ) t
) target,
(
  SELECT study, provenance() AS prov
  FROM f
  WHERE exposure = 'Exercise' AND outcome = 'Cardiovascular Disease'
    AND effect = 'beneficial'
) fin;

SELECT remove_provenance('result_shapley');
SELECT study, ROUND(sv::numeric, 4) AS sv
FROM result_shapley ORDER BY sv DESC, study;
DROP TABLE result_shapley;

-- Step 14: Banzhaf values — alternative game-theoretic contribution measure
CREATE TABLE result_banzhaf AS
SELECT fin.study,
       banzhaf(target.prov, fin.prov) AS bv
FROM (
  SELECT provenance() AS prov
  FROM (
    SELECT DISTINCT exposure, outcome, effect FROM f
    WHERE exposure = 'Exercise' AND outcome = 'Cardiovascular Disease'
      AND effect = 'beneficial'
  ) t
) target,
(
  SELECT study, provenance() AS prov
  FROM f
  WHERE exposure = 'Exercise' AND outcome = 'Cardiovascular Disease'
    AND effect = 'beneficial'
) fin;

SELECT remove_provenance('result_banzhaf');
SELECT study, ROUND(bv::numeric, 4) AS bv
FROM result_banzhaf ORDER BY bv DESC, study;
DROP TABLE result_banzhaf;

-- Clean up
DROP VIEW f_replicated;
DROP VIEW f;
DROP TABLE finding;
DROP TABLE outcome;
DROP TABLE exposure;
DROP TABLE study;
DROP TABLE quality_mapping;
DROP TABLE count_mapping;
DROP TABLE study_mapping;
DROP AGGREGATE quality_plus(study_quality);
DROP AGGREGATE quality_times(study_quality);
DROP FUNCTION evidence_grade(UUID, regclass);
DROP FUNCTION quality_plus_state(study_quality, study_quality);
DROP FUNCTION quality_times_state(study_quality, study_quality);
DROP TYPE study_quality;
