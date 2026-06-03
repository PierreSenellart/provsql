\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_formula AS SELECT
  *, sr_formula(provenance(),'personnel_name') AS formula FROM (
    (SELECT DISTINCT city FROM personnel)
  EXCEPT
    (SELECT p1.city
     FROM personnel p1, personnel p2
     WHERE p1.city = p2.city AND p1.id < p2.id
     GROUP BY p1.city)
  ) t
ORDER BY city;

SELECT remove_provenance('result_formula');
SELECT city, replace(formula,'(Dave ⊗ Nancy) ⊕ (Dave ⊗ Magdalen)','(Dave ⊗ Magdalen) ⊕ (Dave ⊗ Nancy)') AS formula FROM result_formula;

DROP TABLE result_formula;

CREATE TABLE result_formula AS SELECT
  *, sr_formula(provenance(),'personnel_name') AS formula FROM (
    (SELECT city
     FROM personnel
     WHERE FALSE)
  EXCEPT
    (SELECT city
     FROM personnel p)
  ) t
ORDER BY city;

SELECT remove_provenance('result_formula');
SELECT * FROM result_formula;

DROP TABLE result_formula;

-- EXCEPT ALL implements the NOT-IN semantics of the ICDE 2026 paper (§IV-B):
--   ⟪q1 − q2⟫ = {{ (u, α ⊖ ⊕_{β:(u,β)∈q2} β) | (u,α) ∈ q1 }}
-- The sum ⊕β ranges over ALL right tuples equal to u (the right arm is grouped
-- first), and every left tuple is kept.  With a duplicate-valued right arm this
-- differs from the old per-match ⊕(α⊖βi).
CREATE TABLE ea_a(x int);
CREATE TABLE ea_b(x int);
INSERT INTO ea_a VALUES (1);
INSERT INTO ea_b VALUES (1),(1);
SELECT add_provenance('ea_a');
SELECT add_provenance('ea_b');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ea_a;
  PERFORM set_prob(provsql, 0.5) FROM ea_b;
END $$;

-- One left row, two equal right rows: NOT-IN keeps one row with provenance
-- a ⊖ (b1 ⊕ b2), so P = P(a ∧ ¬b1 ∧ ¬b2) = 0.5^3 = 0.125 (not the old 0.375),
-- and EXCEPT ALL returns exactly one row (not two).
CREATE TABLE ea_all AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT x FROM ea_a EXCEPT ALL SELECT x FROM ea_b) t;
SELECT remove_provenance('ea_all');
SELECT count(*) AS n_rows, min(p) AS p_except_all FROM ea_all;
DROP TABLE ea_all;

-- EXCEPT (set semantics ε(q1−q2)) coincides here (single left value).
CREATE TABLE ea_set AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT x FROM ea_a EXCEPT SELECT x FROM ea_b) t;
SELECT remove_provenance('ea_set');
SELECT count(*) AS n_rows, min(p) AS p_except FROM ea_set;
DROP TABLE ea_set;

DROP TABLE ea_a;
DROP TABLE ea_b;
