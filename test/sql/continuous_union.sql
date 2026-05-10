\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SET provsql.monte_carlo_seed = 42;

-- UNION ALL of two RV selections.  Each branch is rewritten
-- independently -- the lifted gate_cmp goes into that branch's
-- provsql column; the UNION ALL itself uses the existing SR_PLUS path
-- to combine the two branches.
CREATE TABLE sensors_a(id text, reading provsql.random_variable);
CREATE TABLE sensors_b(id text, reading provsql.random_variable);
INSERT INTO sensors_a VALUES ('a1', provsql.normal(2, 1));
INSERT INTO sensors_b VALUES ('b1', provsql.normal(0, 1));
SELECT add_provenance('sensors_a');
SELECT add_provenance('sensors_b');

-- Each branch contributes its own gate_cmp via the WHERE rewrite:
-- a1: P(N(2,1) > 1) = Phi(1) ~= 0.8413
-- b1: P(N(0,1) > 1) = 1 - Phi(1) ~= 0.1587
CREATE TABLE result_u AS
  SELECT id,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - CASE id WHEN 'a1' THEN 0.8413 WHEN 'b1' THEN 0.1587 END) < 0.02
         AS within_tolerance
    FROM (SELECT id FROM sensors_a WHERE reading > 1
          UNION ALL
          SELECT id FROM sensors_b WHERE reading > 1) u;
SELECT remove_provenance('result_u');
SELECT * FROM result_u ORDER BY id;
DROP TABLE result_u;

DROP TABLE sensors_a;
DROP TABLE sensors_b;

RESET provsql.monte_carlo_seed;

SELECT 'ok'::text AS continuous_union_done;
