\set ECHO none
\pset format unaligned

CREATE TABLE distinct_result AS
  SELECT *, sr_formula(provenance(),'personnel_name') AS formula
  FROM (
    SELECT DISTINCT classification FROM personnel
  ) t;

SELECT remove_provenance('distinct_result');
SELECT classification,replace(formula,'Paul ⊕ Nancy','Nancy ⊕ Paul') AS formula FROM distinct_result ORDER BY classification;
DROP TABLE distinct_result;

CREATE TABLE distinct_result AS
  SELECT COUNT(DISTINCT name)
  FROM personnel
  GROUP BY city;
SELECT remove_provenance('distinct_result');
SELECT * FROM distinct_result ORDER BY count::numeric;
DROP TABLE distinct_result;

-- DISTINCT over a window value: the window is computed in a subquery, the
-- DISTINCT over it (a window value cannot be a grouping key).
CREATE TABLE distinct_result AS
  SELECT DISTINCT city, first_value(name) OVER (PARTITION BY city ORDER BY id
    ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS first
  FROM personnel;
SELECT remove_provenance('distinct_result');
SELECT city, first FROM distinct_result ORDER BY city;
DROP TABLE distinct_result;

-- DISTINCT over a GROUP BY that reads none of its groups.  With no aggregate
-- anywhere the grouping is duplicate elimination, and the DISTINCT eliminates
-- the same duplicates -- SQL requires every selected column to be a grouping
-- key, so the DISTINCT is over a subset of them -- which makes the grouping
-- redundant and the query the one the DISTINCT alone means.  It used to be
-- refused as an inconsistent DISTINCT, because a grouping key that is not
-- selected carries a junk target entry and the consistency check counted it as
-- a column.  Both forms give the same rows AND the same tokens, so the second
-- query is the check: it is the first with the grouping written out.
CREATE TABLE dgb(k int, v text);
INSERT INTO dgb VALUES (1,'a'),(1,'b'),(2,'c');
SELECT add_provenance('dgb');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM dgb; END $$;
CREATE TABLE distinct_result AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM (SELECT DISTINCT k FROM dgb GROUP BY v, k) g;
SELECT remove_provenance('distinct_result');
SELECT k, p FROM distinct_result ORDER BY k;
DROP TABLE distinct_result;
CREATE TABLE distinct_result AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM (SELECT DISTINCT k FROM dgb) g;
SELECT remove_provenance('distinct_result');
SELECT k, p FROM distinct_result ORDER BY k;
DROP TABLE distinct_result;
-- An aggregate read by the DISTINCT is not refused either, but for another
-- reason and by another route: its value is one per world, and the explosion
-- gives one row per value the count takes (see explode_agg_value), so the
-- answer here is the two counts the groups have.
-- The counts are 1 and 2 on the data as it is; as VALUES, 1 is the count of a
-- group in every world but the one holding both rows of the first key (3/4),
-- and 2 only in that one (1/4).
CREATE TABLE distinct_result AS
  SELECT n, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM (SELECT DISTINCT count(*) AS n FROM dgb GROUP BY k) g;
SELECT remove_provenance('distinct_result');
SELECT n::text AS n, p FROM distinct_result ORDER BY 1;
DROP TABLE distinct_result;
-- A constant select list over a grouping of its own, which used to be read as
-- an inconsistent DISTINCT: one row, present wherever any row of the table is,
-- so 1 - (1/2)³ over the three rows at one half.
CREATE TABLE distinct_result AS
  SELECT round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM (SELECT DISTINCT 1 AS one FROM dgb GROUP BY v) g;
SELECT remove_provenance('distinct_result');
SELECT p FROM distinct_result;
DROP TABLE distinct_result;
SELECT remove_provenance('dgb');
DROP TABLE dgb;
