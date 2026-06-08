\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Boolean analysis of a selection predicate that MIXES a probabilistic
-- comparison (random_variable or aggregate) with an ordinary (regular-column)
-- one inside a single Boolean expression is supported: the ordinary leaf
-- becomes a deterministic indicator (gate_one when it holds, gate_zero
-- otherwise), composed via provenance_times (AND) / provenance_plus (OR) with
-- the probabilistic gate -- the χ / Boolean-combination case of the
-- HAVING-provenance semantics.  (A purely-regular predicate stays an ordinary
-- relational filter; only mixed predicates use this construction.)

CREATE TABLE h(g int, region text, x int, p float);
INSERT INTO h VALUES (1,'north',10,0.5),(1,'north',20,0.5);
SELECT add_provenance('h');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM h; END $$;

-- HAVING, aggregate OR regular: region='north' holds, so the group is
-- certainly kept (⊕ with 𝟙); against 'south' it reduces to P(SUM>25)=0.25.
CREATE TABLE hr_or_t AS SELECT g, probability_evaluate(provenance()) AS pr
  FROM h GROUP BY g, region HAVING sum(x) > 25 OR region = 'north';
SELECT remove_provenance('hr_or_t');
CREATE TABLE hr_or_f AS SELECT g, probability_evaluate(provenance()) AS pr
  FROM h GROUP BY g, region HAVING sum(x) > 25 OR region = 'south';
SELECT remove_provenance('hr_or_f');
SELECT (SELECT round(pr::numeric,4) FROM hr_or_t) AS or_region_true,
       (SELECT round(pr::numeric,4) FROM hr_or_f) AS or_region_false;
DROP TABLE hr_or_t; DROP TABLE hr_or_f;

-- HAVING, aggregate AND regular: region='north' (𝟙) leaves P(SUM>25)=0.25;
-- 'south' (𝟘) makes the group impossible (probability 0).
CREATE TABLE hr_and_t AS SELECT g, probability_evaluate(provenance()) AS pr
  FROM h GROUP BY g, region HAVING sum(x) > 25 AND region = 'north';
SELECT remove_provenance('hr_and_t');
CREATE TABLE hr_and_f AS SELECT g, probability_evaluate(provenance()) AS pr
  FROM h GROUP BY g, region HAVING sum(x) > 25 AND region = 'south';
SELECT remove_provenance('hr_and_f');
SELECT (SELECT round(pr::numeric,4) FROM hr_and_t) AS and_region_true,
       (SELECT round(pr::numeric,4) FROM hr_and_f) AS and_region_false;
DROP TABLE hr_and_t; DROP TABLE hr_and_f;

SELECT remove_provenance('h');
DROP TABLE h;

-- WHERE, random_variable OR regular: on a row where the regular leaf holds the
-- tuple is certainly present (P=1); otherwise P(reading>0)=0.5.
CREATE TABLE sens(id text, reading random_variable);
INSERT INTO sens VALUES ('a', normal(0,1)), ('b', normal(0,1));
SELECT add_provenance('sens');
DO $$ BEGIN PERFORM set_prob(provenance(), 1) FROM sens; END $$;
CREATE TABLE wr AS SELECT id, probability_evaluate(provenance()) AS pr
  FROM sens WHERE reading > 0 OR id = 'a';
SELECT remove_provenance('wr');
SELECT id, round(pr::numeric,4) AS pr FROM wr ORDER BY id;
DROP TABLE wr;
SELECT remove_provenance('sens');
DROP TABLE sens;
