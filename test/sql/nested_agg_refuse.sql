\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;
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

-- Case A: the crashing shape -- outer sum() of probability_evaluate(provenance())
-- over an inner GROUP BY.  Must raise a clear error, not segfault.
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

SELECT remove_provenance('l_nested');
SELECT remove_provenance('r_nested');
DROP TABLE l_nested;
DROP TABLE r_nested;
