\set ECHO none
\pset format unaligned

CREATE TABLE result_count AS SELECT
  city,
  COUNT(*) AS c
FROM personnel
GROUP BY city;

CREATE TABLE result_count2 AS SELECT
  city,
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting
FROM result_count
WHERE c > 2
ORDER BY city;

SELECT remove_provenance('result_count2');
SELECT city, formula, counting
FROM result_count2;

DROP TABLE result_count2;
DROP TABLE result_count;

CREATE TABLE result_sum AS SELECT
  city,
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting FROM (
    SELECT city, SUM(id) AS s FROM personnel GROUP BY city
  ) t
WHERE s > 2 AND city<>'Berlin'
ORDER BY city;

SELECT remove_provenance('result_sum');
SELECT city, formula, counting
FROM result_sum;

DROP TABLE result_sum;

-- An aggregate comparison written in an outer WHERE is migrated into this
-- query level's HAVING, and its provenance supersedes the compared group's
-- delta(+ tokens) -- but only that group's.  Every other input at this level,
-- in particular a join partner, must survive into the circuit.
CREATE TABLE woa_r(g int, v int);
INSERT INTO woa_r VALUES (1,10),(1,20),(1,30),(2,5),(2,7);
CREATE TABLE woa_s(g int, w int);
INSERT INTO woa_s VALUES (1,100),(2,200),(3,300);
SELECT add_provenance('woa_r');
SELECT add_provenance('woa_s');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM woa_r;
  PERFORM set_prob(provsql, 0.3) FROM woa_s;
END $$;

-- No comparison: delta(+ tokens) times the join partner.
-- g=1: (1-0.5^3)*0.3 = 0.2625;  g=2: (1-0.5^2)*0.3 = 0.225
CREATE TABLE woa_nocmp AS
  SELECT x.g AS g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT g, count(*) AS c FROM woa_r GROUP BY g) x, woa_s
  WHERE x.g = woa_s.g;
SELECT remove_provenance('woa_nocmp');
SELECT 'no comparison' AS shape, g, p FROM woa_nocmp ORDER BY g;
DROP TABLE woa_nocmp;

-- Comparison, no join partner: the cmp gate alone, the delta it supersedes
-- dropped rather than multiplied back in.  g=1: 0.5; g=2: 0.25
CREATE TABLE woa_nojoin AS
  SELECT x.g AS g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT g, count(*) AS c FROM woa_r GROUP BY g) x
  WHERE x.c >= 2;
SELECT remove_provenance('woa_nojoin');
SELECT 'comparison, no join' AS shape, g, p FROM woa_nojoin ORDER BY g;
DROP TABLE woa_nojoin;

-- Comparison above a join: the cmp gate AND the join partner.
-- g=1: 0.5*0.3 = 0.15;  g=2: 0.25*0.3 = 0.075
CREATE TABLE woa_join AS
  SELECT x.g AS g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT g, count(*) AS c FROM woa_r GROUP BY g) x, woa_s
  WHERE x.g = woa_s.g AND x.c >= 2;
SELECT remove_provenance('woa_join');
SELECT 'comparison above join' AS shape, g, p FROM woa_join ORDER BY g;
DROP TABLE woa_join;

-- Two groups compared, plus a join partner: both deltas superseded, both cmp
-- gates kept, the partner retained.  0.5 * 0.25 * 0.3 = 0.0375
CREATE TABLE woa_two AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT g, count(*) AS c FROM woa_r GROUP BY g) a,
       (SELECT g, count(*) AS c FROM woa_r GROUP BY g) b, woa_s
  WHERE a.g = 1 AND b.g = 2 AND woa_s.g = 1 AND a.c >= 2 AND b.c >= 2;
SELECT remove_provenance('woa_two');
SELECT 'two groups above join' AS shape, p FROM woa_two;
DROP TABLE woa_two;

DROP TABLE woa_r;
DROP TABLE woa_s;

-- The aggregate result must keep its agg_token type however many subquery
-- levels separate the aggregation from the comparison.  Retyping only one
-- level up leaves the column declared as its pre-rewrite scalar type, and
-- the comparison is then executed natively on the raw composite datum --
-- silently, with neither the right rows nor the right annotation.
CREATE TABLE woa2_r(g int, v int);
INSERT INTO woa2_r VALUES (1,10),(1,20),(1,30),(2,5),(2,7);
SELECT add_provenance('woa2_r');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM woa2_r; END $$;

-- One, two and three levels of nesting all give the same annotation:
-- g=1 -> 0.5 (at least two of three), g=2 -> 0.25 (both).
CREATE TABLE woa2_n1 AS
  SELECT g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT g, count(*) AS c FROM woa2_r GROUP BY g) a
  WHERE c >= 2;
SELECT remove_provenance('woa2_n1');
SELECT 'one level' AS nesting, g, p FROM woa2_n1 ORDER BY g;
DROP TABLE woa2_n1;

CREATE TABLE woa2_n2 AS
  SELECT g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT * FROM (SELECT g, count(*) AS c FROM woa2_r GROUP BY g) a) b
  WHERE c >= 2;
SELECT remove_provenance('woa2_n2');
SELECT 'two levels' AS nesting, g, p FROM woa2_n2 ORDER BY g;
DROP TABLE woa2_n2;

CREATE TABLE woa2_n3 AS
  SELECT g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT * FROM
         (SELECT * FROM (SELECT g, count(*) AS c FROM woa2_r GROUP BY g) a) b) c2
  WHERE c >= 2;
SELECT remove_provenance('woa2_n3');
SELECT 'three levels' AS nesting, g, p FROM woa2_n3 ORDER BY g;
DROP TABLE woa2_n3;

-- A nested view is the same shape through the rewriter, and an equality
-- pins the value rather than just its existence: count(*) = 3 holds only in
-- the single world where all three of group 1's rows are present (0.125),
-- and never for group 2.
CREATE VIEW woa2_v1 AS SELECT g, count(*) AS c FROM woa2_r GROUP BY g;
CREATE VIEW woa2_v2 AS SELECT * FROM woa2_v1;

CREATE TABLE woa2_eq AS
  SELECT g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM woa2_v2 WHERE c = 3;
SELECT remove_provenance('woa2_eq');
SELECT 'nested view, c = 3' AS nesting, g, p FROM woa2_eq ORDER BY g;
DROP TABLE woa2_eq;

DROP VIEW woa2_v2;
DROP VIEW woa2_v1;
DROP TABLE woa2_r;
