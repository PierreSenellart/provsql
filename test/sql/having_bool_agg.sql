\set ECHO none
\pset format unaligned

-- HAVING on a boolean aggregate (bool_or / bool_and / every) compared with a
-- boolean constant. A boolean aggregate has only two possible values, so the
-- worlds yielding each value are characterised directly in the m-semiring
-- (no 2^n enumeration). Probabilities below are exact and hand-checkable with
-- every input atom independent at p = 0.5:
--   A = {true, false}, B = {true}, C = {true, true, false}.

CREATE TABLE hba(g text, flag boolean, level classification_level);
INSERT INTO hba VALUES
  ('A', true, 'secret'), ('A', false, 'unclassified'),
  ('B', true, 'confidential'),
  ('C', true, 'restricted'), ('C', true, 'unclassified'),
  ('C', false, 'secret');
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

-- A bare boolean aggregate is accepted as a HAVING condition (≡ "= true"),
-- and NOT of one is accepted too.
-- HAVING bool_or(flag)       ≡ bool_or = true    A 0.5  B 0.5  C 0.75
CREATE TABLE r6 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hba GROUP BY g HAVING bool_or(flag);
SELECT remove_provenance('r6');
SELECT 'bare bool_or' AS q, g, p FROM r6 ORDER BY g;

-- HAVING NOT(every(flag))    ≡ bool_and = false   A 0.5  B 0    C 0.5
CREATE TABLE r7 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hba GROUP BY g HAVING NOT(every(flag));
SELECT remove_provenance('r7');
SELECT 'not every' AS q, g, p FROM r7 ORDER BY g;

-- HAVING NOT(bool_or(flag))  ≡ bool_or = false    A 0.25 B 0    C 0.125
CREATE TABLE r8 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hba GROUP BY g HAVING NOT(bool_or(flag));
SELECT remove_provenance('r8');
SELECT 'not bool_or' AS q, g, p FROM r8 ORDER BY g;

-- In the security (min-max) semiring, absorptive but with ⊗ not
-- distributing over ⊖, the existential bool_or = true and bool_and =
-- false are the enum-min of the trigger rows' levels (no monus); the
-- other two enumerate, and a world leaving out a row annotated 𝟙 =
-- unclassified is annihilated by its 𝟙 ⊖ (… ⊕ 𝟙) = 𝟘 factor.  The
-- levels are the level column of hba:
--   A = {true: secret, false: unclassified}, B = {true: confidential},
--   C = {true: restricted, true: unclassified, false: secret}.
-- bool_or = true  : A secret (the false row is free)  B confidential
--                   C unclassified
-- bool_and = false: A unclassified  B 𝟘 = unavailable  C secret
-- bool_or = false : A false present, true absent: unclassified ⊗ (𝟙 ⊖
--                   secret) = unclassified;  B 𝟘;  C secret ⊗ (𝟙 ⊖
--                   (restricted ⊕ unclassified)) = secret ⊗ 𝟘 = 𝟘.
-- bool_and = true : A secret ⊗ (𝟙 ⊖ unclassified) = 𝟘;  B confidential;
--                   C the true rows with the false row absent: {r},
--                   {u}, {r, u} annotated restricted ⊗ (𝟙 ⊖ (u ⊕ s)) =
--                   𝟘, unclassified ⊗ (𝟙 ⊖ (r ⊕ s)) = unclassified,
--                   restricted ⊗ (𝟙 ⊖ s) = restricted, so unclassified.
SELECT create_provenance_mapping('hba_level', 'hba', 'level');
CREATE TABLE r9 AS
  SELECT g, sr_minmax(provenance(), 'hba_level', 'unclassified'::classification_level) AS level
  FROM hba GROUP BY g HAVING bool_or(flag) = true;
SELECT remove_provenance('r9');
SELECT 'minmax or=true' AS q, g, level FROM r9 ORDER BY g;
CREATE TABLE r10 AS
  SELECT g, sr_minmax(provenance(), 'hba_level', 'unclassified'::classification_level) AS level
  FROM hba GROUP BY g HAVING bool_and(flag) = false;
SELECT remove_provenance('r10');
SELECT 'minmax and=false' AS q, g, level FROM r10 ORDER BY g;
CREATE TABLE r11 AS
  SELECT g, sr_minmax(provenance(), 'hba_level', 'unclassified'::classification_level) AS level
  FROM hba GROUP BY g HAVING bool_or(flag) = false;
SELECT remove_provenance('r11');
SELECT 'minmax or=false' AS q, g, level FROM r11 ORDER BY g;
CREATE TABLE r12 AS
  SELECT g, sr_minmax(provenance(), 'hba_level', 'unclassified'::classification_level) AS level
  FROM hba GROUP BY g HAVING bool_and(flag) = true;
SELECT remove_provenance('r12');
SELECT 'minmax and=true' AS q, g, level FROM r12 ORDER BY g;

-- Why-provenance (idempotent, not absorptive) enumerates the worlds of
-- the existential bool_or = true without monus factors: C lists every
-- non-empty subset of its three rows containing a true row, i.e. all but
-- the false row alone (the mapping labels each row by its level).
SELECT create_provenance_mapping('hba_name', 'hba', 'level');
CREATE TABLE r13 AS
  SELECT g, sr_why(provenance(), 'hba_name') AS why
  FROM hba GROUP BY g HAVING bool_or(flag) = true;
SELECT remove_provenance('r13');
SELECT 'why or=true' AS q, g, why FROM r13 ORDER BY g;

DROP TABLE r1; DROP TABLE r2; DROP TABLE r3; DROP TABLE r4; DROP TABLE r5;
DROP TABLE r6; DROP TABLE r7; DROP TABLE r8;
DROP TABLE r9; DROP TABLE r10; DROP TABLE r11; DROP TABLE r12; DROP TABLE r13;
DROP TABLE hba_name;
DROP TABLE hba_level;
DROP TABLE hba;
