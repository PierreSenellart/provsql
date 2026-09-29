\set ECHO none
\pset format unaligned

CREATE TABLE result_having_boolean_connectors AS SELECT
  city,
  ROUND(probability_evaluate(provenance())::numeric, 2) AS prob
FROM personnel
GROUP BY city
HAVING COUNT(*) >=1 AND COUNT(*) <= 2;

SELECT remove_provenance('result_having_boolean_connectors');
SELECT * FROM result_having_boolean_connectors
ORDER BY city;

DROP TABLE result_having_boolean_connectors;
CREATE TABLE result_having_boolean_connectors AS SELECT
  city,
  ROUND(probability_evaluate(provenance())::numeric, 2) AS prob
FROM personnel
GROUP BY city
HAVING NOT(COUNT(*) = 0 OR COUNT(*) > 2);

SELECT remove_provenance('result_having_boolean_connectors');
SELECT * FROM result_having_boolean_connectors
ORDER BY city;

DROP TABLE result_having_boolean_connectors;
CREATE TABLE result_having_boolean_connectors AS SELECT
  city,
  COUNT(*) AS c,
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting,
  probability_evaluate(provenance()) AS prob
FROM personnel
GROUP BY city
HAVING COUNT(*) > 2 OR COUNT(*) = 1;

SELECT remove_provenance('result_having_boolean_connectors');
SELECT city, c, formula, counting, ROUND(prob::NUMERIC, 2)
FROM result_having_boolean_connectors
ORDER BY city;

DROP TABLE result_having_boolean_connectors;

CREATE TABLE result_having_boolean_connectors AS SELECT
  city,
  COUNT(*) AS c,
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting,
  probability_evaluate(provenance()) AS prob
FROM personnel
GROUP BY city
HAVING NOT(NOT(COUNT(*) > 2) AND COUNT(*) <> 1);

SELECT remove_provenance('result_having_boolean_connectors');
SELECT city, c, formula, counting, ROUND(prob::NUMERIC, 2)
FROM result_having_boolean_connectors
ORDER BY city;

DROP TABLE result_having_boolean_connectors;

CREATE TABLE result_having_boolean_connectors AS
SELECT
  city,
  c,
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting,
  probability_evaluate(provenance()) AS prob FROM (
    SELECT city, COUNT(*) AS c FROM personnel GROUP BY city
  ) AS t
WHERE c>2 OR c=1;

SELECT remove_provenance('result_having_boolean_connectors');
SELECT city, c, formula, counting, ROUND(prob::NUMERIC, 2)
FROM result_having_boolean_connectors
ORDER BY city;
DROP TABLE result_having_boolean_connectors;

CREATE TABLE result_having_boolean_connectors AS
SELECT
  city,
  c,
  s,
  probability_evaluate(provenance()) AS prob FROM (
    SELECT city, COUNT(*) AS c, SUM(id) AS s FROM personnel GROUP by city
  ) t
  WHERE c=2 AND s>4;

SELECT remove_provenance('result_having_boolean_connectors');
SELECT city, c, s, ROUND(prob::NUMERIC, 2)
FROM result_having_boolean_connectors
ORDER BY city;
DROP TABLE result_having_boolean_connectors;

CREATE TABLE result_having_boolean_connectors AS
SELECT city, COUNT(*) AS c, SUM(id) AS s, probability_evaluate(provenance()) AS prob
FROM personnel
GROUP BY city
HAVING COUNT(*)=2 AND SUM(id)>4;

SELECT remove_provenance('result_having_boolean_connectors');
SELECT city, c, s, ROUND(prob::NUMERIC, 2)
FROM result_having_boolean_connectors
ORDER BY city;
DROP TABLE result_having_boolean_connectors;

CREATE TABLE result_having_boolean_connectors AS
SELECT
  city,
  c,
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting,
  probability_evaluate(provenance()) AS prob FROM (
    SELECT city, COUNT(*) AS c FROM personnel GROUP BY city
  ) AS t
WHERE city='Paris' AND (c>2 OR c=1);

SELECT remove_provenance('result_having_boolean_connectors');
SELECT city, c, formula, counting, ROUND(prob::NUMERIC, 2)
FROM result_having_boolean_connectors;
DROP TABLE result_having_boolean_connectors;

-- Several conditions over one group have as provenance one sum, over the
-- group's worlds, of the worlds where the whole predicate holds.  In a
-- semiring that is not exclusive with an idempotent product, that is not the
-- product (or sum) of the conditions taken one by one, and the conditions are
-- resolved together.  Values of an enumeration of the worlds of each group
-- (counting: tokens valued 2, 1, 3, 1 in group 1; Viterbi: 0.5, 0.7, 0.4, 0.9).
CREATE TABLE hbj(id int, g int, price int);
INSERT INTO hbj VALUES (1, 1, 1), (2, 1, 2), (3, 1, 3), (4, 1, 1),
                       (5, 2, 5), (6, 2, 1);
SELECT add_provenance('hbj');
CREATE TABLE hbj_c AS SELECT (CASE id WHEN 1 THEN 2 WHEN 2 THEN 1 WHEN 3 THEN 3
                                      WHEN 4 THEN 1 WHEN 5 THEN 2 ELSE 1 END)
                               AS value, provenance() AS provenance FROM hbj;
CREATE TABLE hbj_v AS SELECT (CASE id WHEN 1 THEN 0.5 WHEN 2 THEN 0.7
                                      WHEN 3 THEN 0.4 WHEN 4 THEN 0.9
                                      WHEN 5 THEN 0.6 ELSE 0.3 END)::float8
                               AS value, provenance() AS provenance FROM hbj;
-- Group 1: 0 and 0.315, 6 and 0.315, 6 and 0.63; group 2: 2 and 0.18.
CREATE TABLE hbj_r AS
  SELECT 'count>=3 AND sum<=4' AS q, g, sr_counting(provenance(), 'hbj_c') AS c,
         sr_viterbi(provenance(), 'hbj_v') AS v
  FROM hbj GROUP BY g HAVING count(*) >= 3 AND sum(price) <= 4
  UNION ALL
  SELECT 'count>2 AND count<5', g, sr_counting(provenance(), 'hbj_c'),
         sr_viterbi(provenance(), 'hbj_v')
  FROM hbj GROUP BY g HAVING count(*) > 2 AND count(*) < 5
  UNION ALL
  SELECT 'count>=2 AND (sum<=3 OR max>=3)', g, sr_counting(provenance(), 'hbj_c'),
         sr_viterbi(provenance(), 'hbj_v')
  FROM hbj GROUP BY g HAVING count(*) >= 2 AND (sum(price) <= 3 OR max(price) >= 3);
SELECT remove_provenance('hbj_r');
SELECT q, g, c, round(v::numeric, 6) AS v FROM hbj_r ORDER BY q, g;
DROP TABLE hbj_r, hbj_c, hbj_v;
SELECT remove_provenance('hbj');
DROP TABLE hbj;

-- A range on one count(*) -- a conjunction of bounds on it, or one bound --
-- resolves to S_C ⊖ S_{D+1}, in a semiring absorptive with ⊗ distributing
-- over ⊖: linear in the bound rather than an enumeration of the worlds,
-- which a group of 40 rows puts out of reach.  In Viterbi, with every value
-- below one, the best world of a range [C, D] is that of the C best rows.
CREATE TABLE hbr(id int, g int);
INSERT INTO hbr SELECT i, 1 FROM generate_series(1, 40) i;
SELECT add_provenance('hbr');
CREATE TABLE hbr_v AS SELECT (0.5 + id / 100.0)::float8 AS value,
                             provenance() AS provenance FROM hbr;
CREATE TABLE hbr_r AS
  SELECT '3 <= count <= 10' AS q, round(sr_viterbi(provenance(), 'hbr_v')::numeric, 6) AS v
  FROM hbr GROUP BY g HAVING count(*) >= 3 AND count(*) <= 10
  UNION ALL
  SELECT 'count <= 10', round(sr_viterbi(provenance(), 'hbr_v')::numeric, 6)
  FROM hbr GROUP BY g HAVING count(*) <= 10
  UNION ALL
  SELECT 'count = 2', round(sr_viterbi(provenance(), 'hbr_v')::numeric, 6)
  FROM hbr GROUP BY g HAVING count(*) = 2;
SELECT remove_provenance('hbr_r');
SELECT q, v FROM hbr_r ORDER BY q;
DROP TABLE hbr_r, hbr_v;
SELECT remove_provenance('hbr');
DROP TABLE hbr;

-- A disjunction of conditions on the aggregates of two different groups is
-- read as the sum of the provenances of its sides, where the semantics takes
-- one sum over the worlds of both groups: the two agree in the Boolean
-- semiring and that of Boolean functions, and the rewriting says so where
-- they need not.  No warning in
-- the Boolean provenance mode, nor for a disjunction over one group's
-- aggregates.
CREATE TABLE hbd1(k int);
CREATE TABLE hbd2(k int);
INSERT INTO hbd1 VALUES (1);
INSERT INTO hbd2 VALUES (1);
SELECT add_provenance('hbd1');
SELECT add_provenance('hbd2');
CREATE TABLE hbd_r AS
  SELECT a.k FROM (SELECT k, count(*) c FROM hbd1 GROUP BY k) a
  JOIN (SELECT k, count(*) d FROM hbd2 GROUP BY k) b ON a.k = b.k
  WHERE a.c >= 1 OR b.d >= 1;
DROP TABLE hbd_r;
SET provsql.provenance = 'boolean';
CREATE TABLE hbd_r AS
  SELECT a.k FROM (SELECT k, count(*) c FROM hbd1 GROUP BY k) a
  JOIN (SELECT k, count(*) d FROM hbd2 GROUP BY k) b ON a.k = b.k
  WHERE a.c >= 1 OR b.d >= 1;
DROP TABLE hbd_r;
RESET provsql.provenance;
CREATE TABLE hbd_r AS
  SELECT a.k FROM (SELECT k, count(*) c, sum(k) s FROM hbd1 GROUP BY k) a
  WHERE a.c >= 1 OR a.s >= 1;
DROP TABLE hbd_r;
SELECT remove_provenance('hbd1');
SELECT remove_provenance('hbd2');
DROP TABLE hbd1, hbd2;
