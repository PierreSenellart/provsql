\set ECHO none
\pset format unaligned
SET client_min_messages TO WARNING;

-- Regression for the segfault described in the
-- "Outer-aggregation crash on per-group provenance circuits" report.
-- When the outer query wraps probability_evaluate(provenance()) in an
-- SQL aggregate while the inner subquery already aggregates
-- provenance-tracked rows, the planner-hook rewrite would substitute
-- provenance() with an Aggref-containing expression INSIDE another
-- Aggref's argument tree.  PostgreSQL's preprocess_aggrefs_walker
-- (the parse-time check having been bypassed by the planner-hook)
-- does not recurse through Aggref boundaries, so the freshly-injected
-- array_agg's aggno stays at the ProvSQL -1 sentinel and
-- ExecInterpExpr later dereferences ecxt_aggvalues[-1].
-- provenance_mutator must refuse this substitution upfront.

CREATE TABLE l_nested(id int);
CREATE TABLE r_nested(id int);
INSERT INTO l_nested SELECT i/4 FROM generate_series(0, 7) i;  -- 2 ids, 4 dups
INSERT INTO r_nested SELECT i/4 FROM generate_series(0, 7) i;
SELECT add_provenance('l_nested');
SELECT add_provenance('r_nested');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM l_nested;
  PERFORM set_prob(provsql, 0.4) FROM r_nested;
END $$;

-- Case A: outer sum() of probability_evaluate(provenance()) over an inner
-- GROUP BY.  The OUTER is a *scalar* aggregation (no GROUP BY), so its row always
-- exists and its provenance() is gate_one (the scalar-existence fix) -- a
-- constant, not an aggregate expression.  So provenance() substitutes to
-- gate_one, no nested Aggref is produced, and the query is well-defined:
-- sum(probability_evaluate(gate_one)) = sum(1.0) over the inner rows = their
-- count.  (Previously this raised the nested-aggregate error.)
SELECT count(*) AS rows,
       sum(probability_evaluate(provenance())) AS sum_prob
  FROM (SELECT a.id, probability_evaluate(provenance()) AS p
          FROM l_nested a, r_nested b WHERE a.id = b.id GROUP BY a.id) t;

-- Case B: aggregating a *scalar* derived from the inner subquery is
-- fine.  sum(p) where p is the inner-projected double precision
-- produces no nested Aggref.  Round to absorb FP noise.
CREATE TABLE result_b AS
  SELECT count(*) AS rows, round(sum(p)::numeric, 6) AS sum_p
    FROM (SELECT a.id, probability_evaluate(provenance()) AS p
            FROM l_nested a, r_nested b WHERE a.id = b.id GROUP BY a.id) t;
SELECT remove_provenance('result_b');
SELECT * FROM result_b;
DROP TABLE result_b;

-- Case C: dropping the sum() wrapper is the intended way to ask for
-- the probability of the outer group's union circuit.  Round to
-- absorb FP noise; assert > 0.
CREATE TABLE result_c AS
  SELECT count(*) AS rows,
         round(probability_evaluate(provenance())::numeric, 6) > 0 AS prob_positive
    FROM (SELECT a.id, probability_evaluate(provenance()) AS p
            FROM l_nested a, r_nested b WHERE a.id = b.id GROUP BY a.id) t;
SELECT remove_provenance('result_c');
SELECT * FROM result_c;
DROP TABLE result_c;

-- Case D: the inner subquery alone still works.  Sanity check that
-- the new mutator branch did not regress the single-level path.
CREATE TABLE result_d AS
  SELECT a.id, round(probability_evaluate(provenance())::numeric, 6) AS p
    FROM l_nested a, r_nested b WHERE a.id = b.id
    GROUP BY a.id;
SELECT remove_provenance('result_d');
SELECT * FROM result_d ORDER BY id;
DROP TABLE result_d;

-- Case E: aggregating an aggregate result of the inner subquery (max of
-- a count, a HAVING over it) is tracked: the contribution of each row carries
-- the inner aggregate's own gate, so the outer value is read in every world.
-- An aggregate of the same kind (sum of a count) is instead that of the rows
-- of the groups, and a count of a count counts the groups (see reaggregation).
SELECT max(c) FROM (SELECT id, count(*) AS c FROM l_nested GROUP BY id) t;
SELECT sum(c) FROM (SELECT id, count(*) AS c FROM l_nested GROUP BY id) t;
SELECT count(c) FROM (SELECT id, count(*) AS c FROM l_nested GROUP BY id) t;
CREATE TABLE result_e AS
  SELECT count(*) AS groups_kept
    FROM (SELECT id, count(*) AS c FROM l_nested GROUP BY id) t
    HAVING max(c) > 1;
SELECT remove_provenance('result_e');
SELECT * FROM result_e;
DROP TABLE result_e;
-- An outer aggregate that reads no aggregate column is still fine.
SELECT count(*) AS groups
  FROM (SELECT id, count(*) AS c FROM l_nested GROUP BY id) t;

-- Case F: arithmetic over such a re-aggregation.  The value the outer
-- aggregate reads is the inner aggregate's, and the constant arithmetic around
-- it must read that value too: left as it is, the token itself would be read as
-- a number of the aggregate's own type, which is not one (the value was in the
-- billions, it changed from one execution to the next, and a sum of the product
-- read a varlena that is not one at all -- "compressed pglz data is corrupt").
-- The constant stays outside the aggregate here, where the gate_arith carries
-- it over the aggregate's gate: pushing it inside would make the per-row value
-- numeric while the aggregate PostgreSQL resolved still reads bigints.  Every
-- column is compared with what plain SQL answers on the data as it is.
CREATE TABLE result_f AS
  SELECT sum(c) * 1000.0 AS sum_times, sum(c * 1000.0) AS sum_of_times,
         avg(c) * 2 AS avg_times, max(c) + 1 AS max_plus, min(c) - 1 AS min_minus
    FROM (SELECT id, count(*) AS c FROM l_nested GROUP BY id) t;
SELECT remove_provenance('result_f');
SELECT sum_times::text, sum_of_times::text, avg_times::text, max_plus::text,
       min_minus::text FROM result_f;
DROP TABLE result_f;
-- The same on the data as it is, which the values above must equal.
SET provsql.active = off;
SELECT sum(c) * 1000.0 AS sum_times, sum(c * 1000.0) AS sum_of_times,
       avg(c) * 2 AS avg_times, max(c) + 1 AS max_plus, min(c) - 1 AS min_minus
  FROM (SELECT id, count(*) AS c FROM l_nested GROUP BY id) t;
RESET provsql.active;
-- Read in every world, not only in the one the data gives: the eight rows are
-- each present with probability one half, so the sum of the two inner counts is
-- the number of them that are there, of expectation 4.  expected() conditions on
-- the value being defined, and it is undefined only in the single world where no
-- row at all is there, so what it gives is 4 * 256/255 = 4.015686.  The scaled
-- column has to be exactly a thousand times that, which is the invariant the
-- arithmetic must keep: 4015.686275.
CREATE TABLE result_f2 AS
  SELECT round(expected(sum(c) * 1000.0)::numeric, 6) AS e_times,
         round(expected(sum(c))::numeric, 6) AS e_sum
    FROM (SELECT id, count(*) AS c FROM l_nested GROUP BY id) t;
SELECT remove_provenance('result_f2');
SELECT * FROM result_f2;
DROP TABLE result_f2;

SELECT remove_provenance('l_nested');
SELECT remove_provenance('r_nested');
DROP TABLE l_nested;
DROP TABLE r_nested;
