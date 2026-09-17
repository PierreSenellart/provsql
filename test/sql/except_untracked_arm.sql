\set ECHO none
\pset format unaligned

-- EXCEPT with an arm that carries no provenance (an untracked relation,
-- constant rows).  Its rows are present in every world: the arm gets the token
-- 𝟙, as an untracked UNION arm does, and the difference is taken as usual,
-- ⟪q₁ − q₂⟫ = { (u, α ⊖ ⊕β) }, with EXCEPT being ε(q₁ − q₂).  Without a token
-- for the arm no monus was built: an untracked right arm removed nothing, and
-- an untracked left arm handed its rows the token of what removes them, while
-- its unmatched rows vanished.
--
--   tracked   r = {1, 1, 2, 9, NULL}   (each row p = 0.5)
--   untracked u = {1, 9, 7, NULL}

CREATE TABLE eua_r(a int); INSERT INTO eua_r VALUES (1), (1), (2), (9), (NULL);
CREATE TABLE eua_u(a int); INSERT INTO eua_u VALUES (1), (9), (7), (NULL);
SELECT add_provenance('eua_r');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM eua_r; END $$;

CREATE FUNCTION eua_p(u uuid) RETURNS numeric LANGUAGE sql AS
  $$ SELECT round(provsql.probability_evaluate(u)::numeric, 4) $$;

-- r EXCEPT u.  SQL: {2}.  The matched rows stay, with a provenance α ⊖ 𝟙 that
-- is zero for probabilities and for the Boolean semiring.
CREATE TABLE eua_1 AS SELECT a, eua_p(provenance()) AS p, present(provenance()) AS here
  FROM (SELECT a FROM eua_r EXCEPT SELECT a FROM eua_u) t;
SELECT remove_provenance('eua_1');
SELECT 'r EXCEPT u' AS q, a, p, here FROM eua_1 ORDER BY a;

-- EXCEPT ALL is refused as soon as an arm is tracked, and is plain PostgreSQL
-- otherwise.
SELECT a FROM eua_r EXCEPT ALL SELECT a FROM eua_u;
SELECT a FROM eua_u EXCEPT ALL SELECT a FROM eua_r;
CREATE TABLE eua_2 AS SELECT a FROM eua_u EXCEPT ALL SELECT a FROM eua_u WHERE a = 1;
SELECT 'u EXCEPT ALL u (untracked)' AS q, a FROM eua_2 ORDER BY a;

-- u EXCEPT r.  SQL: {7}.  A matched row survives exactly when every tracked
-- row equal to it is absent: 1 - P(some r row): 0.25 for a=1 (two rows), 0.5
-- for 9 and for NULL (syntactic matching); 7 is certain.
CREATE TABLE eua_3 AS SELECT a, eua_p(provenance()) AS p, present(provenance()) AS here
  FROM (SELECT a FROM eua_u EXCEPT SELECT a FROM eua_r) t;
SELECT remove_provenance('eua_3');
SELECT 'u EXCEPT r' AS q, a, p, here FROM eua_3 ORDER BY a;

-- Constant rows as the right arm, and as the left one.
CREATE TABLE eua_4 AS SELECT a, eua_p(provenance()) AS p
  FROM (SELECT a FROM eua_r EXCEPT SELECT 9) t;
SELECT remove_provenance('eua_4');
SELECT 'r EXCEPT SELECT 9' AS q, a, p FROM eua_4 ORDER BY a;
CREATE TABLE eua_5 AS SELECT a, eua_p(provenance()) AS p
  FROM (SELECT 9 AS a EXCEPT SELECT a FROM eua_r) t;
SELECT remove_provenance('eua_5');
SELECT 'SELECT 9 EXCEPT r' AS q, a, p FROM eua_5 ORDER BY a;

-- In a nested set operation: (r EXCEPT u) UNION ALL u.
CREATE TABLE eua_6 AS SELECT a, eua_p(provenance()) AS p
  FROM ((SELECT a FROM eua_r EXCEPT SELECT a FROM eua_u) UNION ALL SELECT a FROM eua_u) t;
SELECT remove_provenance('eua_6');
SELECT 'nested' AS q, a, p FROM eua_6 ORDER BY a, p;

-- The counting semiring: α ⊖ 𝟙 over ℕ.  A single row gives 1 ⊖ 1 = 0, and
-- EXCEPT, being ε(q₁ − q₂), adds the per-row differences: 0 + 0 for a=1.
CREATE TABLE eua_one AS SELECT 1 AS value, provsql AS provenance FROM eua_r;
SELECT remove_provenance('eua_one');
CREATE TABLE eua_7 AS SELECT a, sr_counting(provenance(), 'eua_one') AS c
  FROM (SELECT a FROM eua_r EXCEPT SELECT a FROM eua_u) t;
SELECT remove_provenance('eua_7');
SELECT 'counting' AS q, a, c FROM eua_7 ORDER BY a;

-- NOT IN over an untracked subquery is an ordinary filter (unchanged).
CREATE TABLE eua_8 AS SELECT a, eua_p(provenance()) AS p
  FROM eua_r WHERE a NOT IN (SELECT a FROM eua_u WHERE a IS NOT NULL);
SELECT remove_provenance('eua_8');
SELECT 'NOT IN untracked' AS q, a, p FROM eua_8 ORDER BY a;

DROP FUNCTION eua_p(uuid);
DROP TABLE eua_r, eua_u, eua_one, eua_1, eua_2, eua_3, eua_4, eua_5, eua_6, eua_7, eua_8;
