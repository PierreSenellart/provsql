\set ECHO none
\pset format unaligned

-- HAVING possible-worlds evaluation with CORRELATED contributors.  The
-- value-as-text (choose) and boolean (bool_or / bool_and) HAVING paths take a
-- certified fast path only when the grouped rows' provenance is a set of
-- independent input literals; otherwise they fall back to the general
-- m-semiring world enumeration (the prefix/telescope and 1 ⊖ k factors).  Here
-- every group row is joined against a single shared switch row `sw`, so each
-- contributor is a conjunction (own token ∧ switch) rather than an independent
-- literal, which forces that general branch.  Probabilities are exact: every
-- input atom is independent at p = 0.5, and the shared switch multiplies each
-- group's satisfying mass by P(sw) = 0.5.

CREATE TABLE sw(x int);
INSERT INTO sw VALUES (1);
SELECT add_provenance('sw');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sw; END $$;

-- ---- choose() over text, correlated.  Rows in insertion order: A = (a, b),
--      B = (a).  choose() is PICKFIRST: the value of the lowest-index present
--      row. ----
CREATE TABLE cc(g text, nm text);
INSERT INTO cc VALUES ('A','a'), ('A','b'), ('B','a');
SELECT add_provenance('cc');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM cc; END $$;

-- choose(nm) = 'a': the 'a' row is first, so it suffices it be present
-- (sw ∧ a-row).                                            A 0.25   B 0.25
CREATE TABLE r1 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM cc, sw GROUP BY g HAVING choose(nm) = 'a';
SELECT remove_provenance('r1'); SELECT 'choose=a' AS q, g, p FROM r1 ORDER BY g;

-- choose(nm) = 'b': the match telescopes past the absent 'a' row, so it needs
-- the 'a' row absent and the 'b' row present.              A 0.125  B 0
CREATE TABLE r2 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM cc, sw GROUP BY g HAVING choose(nm) = 'b';
SELECT remove_provenance('r2'); SELECT 'choose=b' AS q, g, p FROM r2 ORDER BY g;

-- ---- boolean aggregates, correlated.  A = (true, false),
--      C = (true, true, false), D = (false, false). ----
CREATE TABLE cb(g text, flag boolean);
INSERT INTO cb VALUES
  ('A', true), ('A', false),
  ('C', true), ('C', true), ('C', false),
  ('D', false), ('D', false);
SELECT add_provenance('cb');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM cb; END $$;

-- bool_or = true: at least one true-row present (sw ∧ some true row).
--                                                  A 0.25   C 0.375  D 0
CREATE TABLE r3 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM cb, sw GROUP BY g HAVING bool_or(flag) = true;
SELECT remove_provenance('r3'); SELECT 'or=true' AS q, g, p FROM r3 ORDER BY g;

-- bool_or = false: no true-row present, group non-empty (the false class
-- supplies the present row -- exercises the noneF complement factor).
--                                                  A 0.125  C 0.0625 D 0.375
CREATE TABLE r4 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM cb, sw GROUP BY g HAVING bool_or(flag) = false;
SELECT remove_provenance('r4'); SELECT 'or=false' AS q, g, p FROM r4 ORDER BY g;

-- bool_and = false: at least one false-row present (telescopes over D's two
-- false rows).                                     A 0.25   C 0.25   D 0.375
CREATE TABLE r5 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM cb, sw GROUP BY g HAVING bool_and(flag) = false;
SELECT remove_provenance('r5'); SELECT 'and=false' AS q, g, p FROM r5 ORDER BY g;

DROP TABLE r1; DROP TABLE r2; DROP TABLE r3; DROP TABLE r4; DROP TABLE r5;
DROP TABLE cc; DROP TABLE cb; DROP TABLE sw;
