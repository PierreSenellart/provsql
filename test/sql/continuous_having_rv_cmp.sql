\set ECHO none
\pset format unaligned

-- A comparison on an aggregate over random_variable in HAVING is lifted into a
-- comparison gate, like its WHERE counterpart: the group is an answer in the
-- worlds where it exists and the comparison holds, δ(⊕ k) ⊗ ⟦cmp⟧.  It used to
-- be left to PostgreSQL, which cannot evaluate the placeholder operator.
--
--   g=1: x ~ N(10,1), N(20,1), N(30,1)      g=2: x ~ N(5,1)
-- chr_c has certain rows, chr_h the same rows at probability 0.5.
-- Probabilities come from sampling: they are checked within a tolerance.

CREATE TABLE chr_c(g int, x random_variable);
INSERT INTO chr_c VALUES (1, normal(10,1)), (1, normal(20,1)), (1, normal(30,1)),
  (2, normal(5,1));
CREATE TABLE chr_h AS SELECT * FROM chr_c;
SELECT add_provenance('chr_c'); SELECT add_provenance('chr_h');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM chr_h; END $$;

CREATE FUNCTION chr_near(u uuid, expected float8) RETURNS boolean LANGUAGE sql AS
  $$ SELECT abs(provsql.probability_evaluate(u) - expected) < 0.03 $$;

-- Certain rows: sum ~ N(60, 3).  > 60 is 0.5 by symmetry; g=2 never gets there.
CREATE TABLE chr_1 AS SELECT g, chr_near(provenance(), CASE g WHEN 1 THEN 0.5 ELSE 0 END) AS ok
  FROM chr_c GROUP BY g HAVING sum(x) > 60;
SELECT remove_provenance('chr_1'); SELECT 'sum > 60' AS q, g, ok FROM chr_1 ORDER BY g;
CREATE TABLE chr_2 AS SELECT g, chr_near(provenance(), CASE g WHEN 1 THEN 1 ELSE 0 END) AS ok
  FROM chr_c GROUP BY g HAVING sum(x) > 40;
SELECT remove_provenance('chr_2'); SELECT 'sum > 40' AS q, g, ok FROM chr_2 ORDER BY g;

-- Uncertain rows: > 40 holds for {20,30} and {10,20,30}, and half the time for
-- {10,30}: (1 + 1 + 0.5) / 8 = 0.3125.
CREATE TABLE chr_3 AS SELECT g, chr_near(provenance(), CASE g WHEN 1 THEN 0.3125 ELSE 0 END) AS ok
  FROM chr_h GROUP BY g HAVING sum(x) > 40;
SELECT remove_provenance('chr_3'); SELECT 'uncertain, sum > 40' AS q, g, ok FROM chr_3 ORDER BY g;

-- The δ factor: a comparison that always holds leaves group existence, 1 - 1/8
-- and 1/2, not 1 (the empty selection has sum 0 < 1000, but no group).
CREATE TABLE chr_4 AS SELECT g, chr_near(provenance(), CASE g WHEN 1 THEN 0.875 ELSE 0.5 END) AS ok
  FROM chr_h GROUP BY g HAVING sum(x) < 1000;
SELECT remove_provenance('chr_4'); SELECT 'uncertain, always true' AS q, g, ok FROM chr_4 ORDER BY g;

-- Scalar aggregation: the row always exists.  sum of the four ~ N(65, 4).
CREATE TABLE chr_5 AS SELECT chr_near(provenance(), 0.5) AS ok FROM chr_c HAVING sum(x) > 65;
SELECT remove_provenance('chr_5'); SELECT 'scalar' AS q, ok FROM chr_5;
-- ... including over an empty selection, where sum(x) < 1000 holds: 1, not 15/16.
CREATE TABLE chr_6 AS SELECT chr_near(provenance(), 1) AS ok FROM chr_h HAVING sum(x) < 1000;
SELECT remove_provenance('chr_6'); SELECT 'scalar, uncertain, always true' AS q, ok FROM chr_6;

-- NOT, other aggregates, a regular atom mixed in.
CREATE TABLE chr_7 AS SELECT g, chr_near(provenance(), CASE g WHEN 1 THEN 0.5 ELSE 0 END) AS ok
  FROM chr_c GROUP BY g HAVING NOT (sum(x) <= 60);
SELECT remove_provenance('chr_7'); SELECT 'NOT' AS q, g, ok FROM chr_7 ORDER BY g;
CREATE TABLE chr_8 AS SELECT g, chr_near(provenance(), CASE g WHEN 1 THEN 0.5 ELSE 0 END) AS ok
  FROM chr_c GROUP BY g HAVING avg(x) > 20;
SELECT remove_provenance('chr_8'); SELECT 'avg > 20' AS q, g, ok FROM chr_8 ORDER BY g;
CREATE TABLE chr_9 AS SELECT g, chr_near(provenance(), CASE g WHEN 1 THEN 1 ELSE 0 END) AS ok
  FROM chr_c GROUP BY g HAVING sum(x) > 1 AND g = 1;
SELECT remove_provenance('chr_9'); SELECT 'regular atom' AS q, g, ok FROM chr_9 ORDER BY g;

-- The deterministic form is untouched: PostgreSQL filters on the scalar.
CREATE TABLE chr_10 AS SELECT g FROM chr_c GROUP BY g HAVING expected(sum(x)) > 40;
SELECT remove_provenance('chr_10'); SELECT 'expected(sum) > 40' AS q, g FROM chr_10 ORDER BY g;

-- Mixing with a comparison on an ordinary aggregate is refused.
SELECT g FROM chr_c GROUP BY g HAVING sum(x) > 40 AND count(*) > 1;

DROP FUNCTION chr_near(uuid, float8);
DROP TABLE chr_c, chr_h, chr_1, chr_2, chr_3, chr_4, chr_5, chr_6, chr_7, chr_8, chr_9, chr_10;
