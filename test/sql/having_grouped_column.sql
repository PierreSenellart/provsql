\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- ----------------------------------------------------------------------
-- HAVING that compares an aggregate to a bare GROUP BY column (a per-group
-- VARIABLE, not a literal).  having_OpExpr_to_provenance_cmp wraps such a
-- column exactly like a Const -- provenance_semimod(col, gate_one()) -- since
-- a non-agg_token Var in HAVING is necessarily a grouping key, hence constant
-- within its group.  Before this it bailed with "cannot handle complex HAVING
-- expressions".  Both operand orders (agg OP col / col OP agg) are exercised.
--
-- Results are materialised and stripped of their per-run random provenance
-- token before display, so only the stable probability prints.
-- ----------------------------------------------------------------------

CREATE TABLE hgcol(k int, x int);
-- group k has exactly k rows, so count(*) = k iff all k rows are present.
INSERT INTO hgcol VALUES (1,10),(2,10),(2,20),(3,10),(3,20),(3,30);
SELECT add_provenance('hgcol');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM hgcol; END $$;

-- HAVING count(*) = k : k=1 -> 0.5, k=2 -> 0.25 (both rows), k=3 -> 0.125 (all).
CREATE TEMP TABLE r1 AS
  SELECT k, probability_evaluate(provenance()) AS p
  FROM hgcol GROUP BY k HAVING count(*) = k;
SELECT remove_provenance('r1');
SELECT 'count=k' AS shape, k, round(p::numeric, 4) AS p FROM r1 ORDER BY k;
DROP TABLE r1;

-- Same with the grouped column on the LEFT of the comparison.
CREATE TEMP TABLE r2 AS
  SELECT k, probability_evaluate(provenance()) AS p
  FROM hgcol GROUP BY k HAVING k = count(*);
SELECT remove_provenance('r2');
SELECT 'k=count' AS shape, k, round(p::numeric, 4) AS p FROM r2 ORDER BY k;
DROP TABLE r2;

-- A different aggregate compared to the grouped column: max(x) = k is never
-- satisfiable here (x >= 10 > k), so every group has probability 0.
CREATE TEMP TABLE r3 AS
  SELECT k, probability_evaluate(provenance()) AS p
  FROM hgcol GROUP BY k HAVING max(x) = k;
SELECT remove_provenance('r3');
SELECT 'max=k' AS shape, k, round(p::numeric, 4) AS p FROM r3 ORDER BY k;
DROP TABLE r3;

SELECT remove_provenance('hgcol');
DROP TABLE hgcol;
