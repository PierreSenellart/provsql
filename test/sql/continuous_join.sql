\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SET provsql.monte_carlo_seed = 42;

-- Two provenance-tracked tables of independent normals.  The join
-- multiplies the two rows' provenances via the existing JOIN path
-- (SR_TIMES); the WHERE comparing two RVs from different RTEs is
-- lifted into the provenance via the new RV-WHERE walker, conjoined
-- with the join's TIMES result.
CREATE TABLE left_sensors(id text, reading provsql.random_variable);
CREATE TABLE right_sensors(id text, reading provsql.random_variable);
INSERT INTO left_sensors VALUES ('l1', provsql.normal(1, 1));
INSERT INTO right_sensors VALUES ('r1', provsql.normal(0, 1));
SELECT add_provenance('left_sensors');
SELECT add_provenance('right_sensors');

-- Cartesian join with WHERE comparing two RVs from different RTEs.
-- P(N(1,1) > N(0,1)) = P(N(1,2) > 0) = Phi(1/sqrt(2)) ~= 0.7602.
CREATE TABLE result_a AS
  SELECT l.id AS l_id, r.id AS r_id,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - 0.7602) < 0.02 AS within_tolerance
    FROM left_sensors l, right_sensors r
    WHERE l.reading > r.reading;
SELECT remove_provenance('result_a');
SELECT * FROM result_a;
DROP TABLE result_a;

-- Same join with a non-RV id filter -- id pruning happens in the
-- executor, RV cmp lifts into provsql.
CREATE TABLE result_b AS
  SELECT l.id AS l_id, r.id AS r_id,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - 0.7602) < 0.02 AS within_tolerance
    FROM left_sensors l, right_sensors r
    WHERE l.id = 'l1' AND l.reading > r.reading;
SELECT remove_provenance('result_b');
SELECT * FROM result_b;
DROP TABLE result_b;

DROP TABLE left_sensors;
DROP TABLE right_sensors;

RESET provsql.monte_carlo_seed;

SELECT 'ok'::text AS continuous_join_done;
