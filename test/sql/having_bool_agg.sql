\set ECHO none
\pset format unaligned

-- HAVING on a boolean aggregate (bool_or / bool_and / every) compared with a
-- boolean constant. A boolean aggregate has only two possible values, so the
-- worlds yielding each value are characterised directly in the m-semiring
-- (no 2^n enumeration). Probabilities below are exact and hand-checkable with
-- every input atom independent at p = 0.5:
--   A = {true, false}, B = {true}, C = {true, true, false}.

CREATE TABLE hba(g text, flag boolean);
INSERT INTO hba VALUES
  ('A', true), ('A', false),
  ('B', true),
  ('C', true), ('C', true), ('C', false);
SELECT add_provenance('hba');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM hba; END $$;

-- bool_or = true  : at least one true-row present.       A 0.5  B 0.5  C 0.75
CREATE TABLE r1 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hba GROUP BY g HAVING bool_or(flag) = true;
SELECT remove_provenance('r1');
SELECT 'or=true' AS q, g, p FROM r1 ORDER BY g;

-- bool_or = false : no true-row, but non-empty.          A 0.25 B 0    C 0.125
CREATE TABLE r2 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hba GROUP BY g HAVING bool_or(flag) = false;
SELECT remove_provenance('r2');
SELECT 'or=false' AS q, g, p FROM r2 ORDER BY g;

-- bool_and = true : no false-row, but non-empty.         A 0.25 B 0.5  C 0.375
CREATE TABLE r3 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hba GROUP BY g HAVING bool_and(flag) = true;
SELECT remove_provenance('r3');
SELECT 'and=true' AS q, g, p FROM r3 ORDER BY g;

-- every() is an alias of bool_and: identical result.
CREATE TABLE r4 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hba GROUP BY g HAVING every(flag) = true;
SELECT remove_provenance('r4');
SELECT 'every=true' AS q, g, p FROM r4 ORDER BY g;

-- bool_and = false : at least one false-row present.     A 0.5  B 0    C 0.5
CREATE TABLE r5 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hba GROUP BY g HAVING bool_and(flag) = false;
SELECT remove_provenance('r5');
SELECT 'and=false' AS q, g, p FROM r5 ORDER BY g;

DROP TABLE r1; DROP TABLE r2; DROP TABLE r3; DROP TABLE r4; DROP TABLE r5;
DROP TABLE hba;
