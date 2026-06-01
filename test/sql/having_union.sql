\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- ----------------------------------------------------------------------
-- HAVING aggregate comparisons over UNION (DISTINCT) and EXCEPT inputs.
-- A deduplicated UNION tuple has gate_plus lineage (r1 ⊕ r2); an EXCEPT
-- tuple has gate_monus lineage (r1 ⊖ r2).  When the contributors are
-- read-once and leaf-disjoint (the common case: the two branches do not
-- re-use the same base tuple across different output rows), the closed-
-- form pre-pass resolves them via contributorProb, which already handles
-- plus / monus.  off (enumeration) and on (closed-form) must agree.
--
-- (The remaining open case is UNION/EXCEPT over a *join* that re-uses a
-- base tuple, e.g. (R⋈S) UNION (R⋈T): the shared R makes the contributor
-- non-read-once, so it falls back to exact enumeration.)
-- ----------------------------------------------------------------------

CREATE TABLE ua(k int, x int);
CREATE TABLE ub(k int, x int);
INSERT INTO ua VALUES (1,10),(1,20),(1,30),(2,40),(2,50);
INSERT INTO ub VALUES (1,20),(1,30),(1,60),(2,40);   -- overlap: (1,20),(1,30),(2,40)
SELECT add_provenance('ua');
SELECT add_provenance('ub');
DO $$ BEGIN PERFORM set_prob(provenance(), (0.30 + 0.005*x)) FROM ua; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), (0.40 + 0.004*x)) FROM ub; END $$;

CREATE FUNCTION u_par(opname text, q text)
  RETURNS TABLE(shape text, g int, p_off numeric, p_on numeric, diff numeric)
AS $$
BEGIN
  EXECUTE 'SET provsql.cmp_probability_evaluation = off';
  EXECUTE format('CREATE TEMP TABLE u_o AS %s', q);
  PERFORM remove_provenance('u_o');
  EXECUTE 'SET provsql.cmp_probability_evaluation = on';
  EXECUTE format('CREATE TEMP TABLE u_n AS %s', q);
  PERFORM remove_provenance('u_n');
  RETURN QUERY
    SELECT opname, o.g, round(o.p::numeric, 4), round(n.p::numeric, 4),
           round(abs(o.p - n.p)::numeric, 6)
    FROM u_o o JOIN u_n n USING (g) ORDER BY o.g;
  DROP TABLE u_o, u_n;
END
$$ LANGUAGE plpgsql;

-- UNION (DISTINCT): plus lineage on the overlapping tuples.
SELECT * FROM u_par('union COUNT>=2', 'SELECT k g, probability_evaluate(provenance()) p FROM (SELECT k,x FROM ua UNION SELECT k,x FROM ub) u GROUP BY k HAVING count(*) >= 2');
SELECT * FROM u_par('union COUNT>=4', 'SELECT k g, probability_evaluate(provenance()) p FROM (SELECT k,x FROM ua UNION SELECT k,x FROM ub) u GROUP BY k HAVING count(*) >= 4');
SELECT * FROM u_par('union SUM>=80',  'SELECT k g, probability_evaluate(provenance()) p FROM (SELECT k,x FROM ua UNION SELECT k,x FROM ub) u GROUP BY k HAVING sum(x) >= 80');
SELECT * FROM u_par('union MAX>=50',  'SELECT k g, probability_evaluate(provenance()) p FROM (SELECT k,x FROM ua UNION SELECT k,x FROM ub) u GROUP BY k HAVING max(x) >= 50');
SELECT * FROM u_par('union MIN<=20',  'SELECT k g, probability_evaluate(provenance()) p FROM (SELECT k,x FROM ua UNION SELECT k,x FROM ub) u GROUP BY k HAVING min(x) <= 20');

-- EXCEPT: monus lineage (present in ua, absent from ub).
SELECT * FROM u_par('except COUNT>=1', 'SELECT k g, probability_evaluate(provenance()) p FROM (SELECT k,x FROM ua EXCEPT SELECT k,x FROM ub) u GROUP BY k HAVING count(*) >= 1');
SELECT * FROM u_par('except COUNT>=2', 'SELECT k g, probability_evaluate(provenance()) p FROM (SELECT k,x FROM ua EXCEPT SELECT k,x FROM ub) u GROUP BY k HAVING count(*) >= 2');
SELECT * FROM u_par('except SUM>=60',  'SELECT k g, probability_evaluate(provenance()) p FROM (SELECT k,x FROM ua EXCEPT SELECT k,x FROM ub) u GROUP BY k HAVING sum(x) >= 60');
SELECT * FROM u_par('except MAX>=50',  'SELECT k g, probability_evaluate(provenance()) p FROM (SELECT k,x FROM ua EXCEPT SELECT k,x FROM ub) u GROUP BY k HAVING max(x) >= 50');

DROP FUNCTION u_par(text, text);
SELECT remove_provenance('ua');
SELECT remove_provenance('ub');
DROP TABLE ua, ub;
