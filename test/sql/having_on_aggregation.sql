\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_having_on_count AS SELECT
  city,
  COUNT(*),
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting
FROM personnel
GROUP BY city
HAVING count(*) > 2;

SELECT remove_provenance('result_having_on_count');
SELECT city, formula, counting
FROM result_having_on_count
ORDER BY city;

DROP TABLE result_having_on_count;

CREATE TABLE result_having_on_sum AS SELECT
  city,
  SUM(id),
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting
FROM personnel
GROUP BY city
HAVING SUM(id) > 2;

SELECT remove_provenance('result_having_on_sum');
SELECT city, formula, counting
FROM result_having_on_sum
ORDER BY city;

DROP TABLE result_having_on_sum;

CREATE TABLE result_having_why_count_leq3 AS SELECT
  city,
  COUNT(*),
  sr_why(provenance(), 'personnel_name') AS formula
FROM personnel
GROUP BY city
HAVING count(*) <= 3;

SELECT remove_provenance('result_having_why_count_leq3');
SELECT city, formula
FROM result_having_why_count_leq3
ORDER BY city;

DROP TABLE result_having_why_count_leq3;

CREATE TABLE result_having_why_sum_geq7 AS SELECT
  city,
  SUM(id),
  sr_why(provenance(), 'personnel_name') AS formula
FROM personnel
GROUP BY city
HAVING SUM(id) >= 7;

SELECT remove_provenance('result_having_why_sum_geq7');
SELECT city, formula
FROM result_having_why_sum_geq7
ORDER BY city;

DROP TABLE result_having_why_sum_geq7;

-- Collapsing several groups into a single aggregate (GROUP BY 1) flattens
-- every group's contribution into one top-level ⊕ sum, whose term order
-- mirrors the aggregate's group-emission order -- plan- and system-dependent
-- (hash-aggregate bucket order, parallelism, PostgreSQL version).  ⊕ is
-- commutative, so canonicalise by sorting the depth-0 ⊕ terms before
-- comparison (paren-aware: inner ⊕ inside "𝟙 ⊖ (a ⊕ b)" must not be split).
CREATE FUNCTION sort_sum_terms(f text) RETURNS text AS $$
DECLARE
  sep   text := ' ' || chr(8853) || ' ';   -- ' ⊕ '
  depth int := 0;
  i     int := 1;
  n     int := length(f);
  ch    text;
  buf   text := '';
  terms text[] := ARRAY[]::text[];
BEGIN
  WHILE i <= n LOOP
    IF depth = 0 AND substr(f, i, 3) = sep THEN
      terms := terms || buf;
      buf := '';
      i := i + 3;
      CONTINUE;
    END IF;
    ch := substr(f, i, 1);
    IF    ch = '(' THEN depth := depth + 1;
    ELSIF ch = ')' THEN depth := depth - 1;
    END IF;
    buf := buf || ch;
    i := i + 1;
  END LOOP;
  terms := terms || buf;
  RETURN array_to_string(ARRAY(SELECT unnest(terms) AS t ORDER BY t), sep);
END
$$ LANGUAGE plpgsql IMMUTABLE STRICT;

CREATE TABLE result_nested_having_formula AS
SELECT 1, sr_formula(provenance(), 'personnel_name') AS formula
FROM (
  SELECT city, count(*)
  FROM personnel
  GROUP BY city
  HAVING count(*) < 3
) AS tmp
GROUP BY 1;

SELECT remove_provenance('result_nested_having_formula');
SELECT sort_sum_terms(formula) AS formula
FROM result_nested_having_formula;

DROP TABLE result_nested_having_formula;
DROP FUNCTION sort_sum_terms(text);

CREATE TABLE result_nested_having_why AS
SELECT 1, sr_why(provenance(), 'personnel_name') AS formula
FROM (
  SELECT city, count(*)
  FROM personnel
  GROUP BY city
  HAVING count(*) < 3
) AS tmp
GROUP BY 1;

SELECT remove_provenance('result_nested_having_why');
SELECT formula
FROM result_nested_having_why;

DROP TABLE result_nested_having_why;

CREATE TABLE result_nested_having_count AS
SELECT 1, sr_counting(provenance(), 'personnel_count') AS counting
FROM (
  SELECT city, count(*)
  FROM personnel
  GROUP BY city
  HAVING count(*) < 3
) AS tmp
GROUP BY 1;

SELECT remove_provenance('result_nested_having_count');
SELECT counting
FROM result_nested_having_count;

DROP TABLE result_nested_having_count;

CREATE TABLE result_complex_having AS
SELECT city, sr_formula(provenance(), 'personnel_name') AS formula
FROM (
  SELECT city, count(*)
  FROM personnel
  GROUP BY city
  HAVING count(*) < 3 AND sum(id)>5
) AS tmp;

SELECT remove_provenance('result_complex_having');
SELECT city, formula
FROM result_complex_having
ORDER BY city;

DROP TABLE result_complex_having;

-- Scalar (no GROUP BY) count: count(*) < 4 is true on the empty group (0 < 4),
-- and a scalar aggregation always produces one row, so the empty world (no
-- same-city pairs present) is a real witness -- the empty set {} -- alongside the
-- non-empty <4-pair worlds.  (A grouped count would instead drop the empty group.)
CREATE TABLE result_join_having_why_count_lt4 AS
SELECT
  COUNT(*),
  sr_why(provenance(), 'personnel_name') AS formula
FROM (
  SELECT *
  FROM personnel p1, personnel p2
  WHERE p1.city = p2.city AND p1.id < p2.id
) AS tmp
HAVING count(*) < 4;

SELECT remove_provenance('result_join_having_why_count_lt4');
SELECT formula
FROM result_join_having_why_count_lt4;

DROP TABLE result_join_having_why_count_lt4;

-- HAVING without provsql in search_path (regression test for
-- missing search_path on provenance_cmp)
SET search_path TO provsql_test;

CREATE TABLE result_having_no_searchpath AS SELECT
  city,
  COUNT(*)
FROM personnel
GROUP BY city
HAVING count(*) > 2;

SELECT provsql.remove_provenance('result_having_no_searchpath');
SELECT * FROM result_having_no_searchpath ORDER BY city;

DROP TABLE result_having_no_searchpath;


SET search_path TO provsql_test,provsql;

-- A HAVING comparing two aggregates that read the same rows and contribute the
-- same value on each: count(v) and count(w) over a group both count its rows,
-- so the circuit, which is hash-consed, has ONE gate on both sides of the
-- comparison and the operator settles it whatever the data.  Settling it true
-- must not give the constant one: a tautology over a group still says that the
-- group is there, and the constant would credit the world where the group is
-- empty and SQL returns no row at all.  Three rows at one half in the first
-- group, two in the second, so a group is there with probability 1 - 1/8 and
-- 1 - 1/4; the reflexive-false operators hold in no world at all.
CREATE TABLE hre(g int, v int, w int);
INSERT INTO hre VALUES (1, 1, 9), (1, 2, 9), (1, 2, 8), (2, 5, 9), (2, 6, 9);
SELECT add_provenance('hre');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM hre; END $$;
CREATE TABLE hre_r AS SELECT g, probability(provenance()) AS p FROM hre
  GROUP BY g HAVING count(v) = count(w);
SELECT remove_provenance('hre_r');
SELECT g, round(p::numeric, 6) AS p FROM hre_r ORDER BY g;
DROP TABLE hre_r;
CREATE TABLE hre_r AS SELECT g, probability(provenance()) AS p FROM hre
  GROUP BY g HAVING count(v) >= count(w);
SELECT remove_provenance('hre_r');
SELECT g, round(p::numeric, 6) AS p FROM hre_r ORDER BY g;
DROP TABLE hre_r;
CREATE TABLE hre_r AS SELECT g, probability(provenance()) AS p FROM hre
  GROUP BY g HAVING count(v) <> count(w);
SELECT remove_provenance('hre_r');
SELECT g, round(p::numeric, 6) AS p FROM hre_r ORDER BY g;
DROP TABLE hre_r;
-- Without a GROUP BY the single result row is there in every world, the empty
-- one included, where count reads 0 on both sides: the tautology is certain.
CREATE TABLE hre_r AS SELECT probability(provenance()) AS p FROM hre
  HAVING count(v) = count(w);
SELECT remove_provenance('hre_r');
SELECT round(p::numeric, 6) AS p FROM hre_r;
DROP TABLE hre_r;
-- Comparisons the fix must leave alone: a threshold, and two aggregates that
-- are NOT the same gate (count(*) counts rows, count(v) counts values).
CREATE TABLE hre_r AS SELECT g, probability(provenance()) AS p FROM hre
  GROUP BY g HAVING count(*) = count(v);
SELECT remove_provenance('hre_r');
SELECT g, round(p::numeric, 6) AS p FROM hre_r ORDER BY g;
DROP TABLE hre_r;
CREATE TABLE hre_r AS SELECT g, probability(provenance()) AS p FROM hre
  GROUP BY g HAVING count(v) = 3;
SELECT remove_provenance('hre_r');
SELECT g, round(p::numeric, 6) AS p FROM hre_r ORDER BY g;
DROP TABLE hre_r;
DROP TABLE hre;
