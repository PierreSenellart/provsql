\set ECHO none
\pset format unaligned

-- ----------------------------------------------------------------------
-- HAVING aggregate comparisons over UNION (DISTINCT) and EXCEPT inputs.
-- A deduplicated UNION tuple has gate_plus lineage (r1 ⊕ r2); an EXCEPT
-- tuple has gate_monus lineage (r1 ⊖ r2).  When the contributors are
-- read-once and leaf-disjoint (the common case: the two branches do not
-- re-use the same base tuple across different output rows), the closed-
-- form pre-pass resolves them via contributorProb, which already handles
-- plus / monus.  off (enumeration) and on (closed-form) must agree.
--
-- UNION/EXCEPT over a *join* that re-uses a base tuple, e.g.
-- (R⋈S) UNION (R⋈T), makes each contributor non-read-once (the shared R is
-- repeated: (r∧s)∨(r∧t)).  When the contributors are still mutually
-- independent -- their footprints disjoint, the usual case -- each one's
-- exact marginal is computed (contributorExactMarginal, brute force over its
-- private leaves) and it is treated as an independent event; see the second
-- block below.  Only when a base tuple is shared *across* contributors of the
-- same group (genuinely #P-hard) does it fall back to enumeration.
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

-- Non-read-once contributors from a UNION/EXCEPT over a join sharing R.
-- ur(g,k) gives a distinct R tuple per (g,k), so within a group the union
-- tuples are leaf-disjoint (independent): each (r∧(s∨t)) / (r∧s)∖(r∧t) is
-- resolved exactly and off/on agree.
CREATE TABLE ur(g int, k int);
CREATE TABLE us(k int);
CREATE TABLE ut(k int);
INSERT INTO ur VALUES (1,10),(1,20),(2,30);
INSERT INTO us VALUES (10),(20),(30);
INSERT INTO ut VALUES (10),(20),(30);
SELECT add_provenance('ur');
SELECT add_provenance('us');
SELECT add_provenance('ut');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ur;
            PERFORM set_prob(provenance(), 0.5) FROM us;
            PERFORM set_prob(provenance(), 0.5) FROM ut; END $$;

SELECT * FROM u_par('join-union COUNT>=2', 'SELECT g, probability_evaluate(provenance()) p FROM (SELECT ur.g, us.k FROM ur JOIN us USING (k) UNION SELECT ur.g, ut.k FROM ur JOIN ut USING (k)) u GROUP BY g HAVING count(*) >= 2');
SELECT * FROM u_par('join-union COUNT>=1', 'SELECT g, probability_evaluate(provenance()) p FROM (SELECT ur.g, us.k FROM ur JOIN us USING (k) UNION SELECT ur.g, ut.k FROM ur JOIN ut USING (k)) u GROUP BY g HAVING count(*) >= 1');
SELECT * FROM u_par('join-except COUNT>=1', 'SELECT g, probability_evaluate(provenance()) p FROM (SELECT ur.g, us.k FROM ur JOIN us USING (k) EXCEPT SELECT ur.g, ut.k FROM ur JOIN ut USING (k)) u GROUP BY g HAVING count(*) >= 1');

-- Confirm the non-read-once contributor arm actually FIRES.
SET provsql.verbose_level = 5;
SELECT provenance() AS jt FROM (SELECT ur.g, us.k FROM ur JOIN us USING (k) WHERE ur.g = 1 UNION SELECT ur.g, ut.k FROM ur JOIN ut USING (k) WHERE ur.g = 1) u GROUP BY g HAVING count(*) >= 2 \gset
SELECT round(probability_evaluate(:'jt'::uuid)::numeric, 5) AS join_union_fires;
SET provsql.verbose_level = 0;

-- A base tuple shared *across* the group's contributors (ur1 has one R per g,
-- cross-joined): the contributors couple on the shared R, so the arm must
-- DECLINE and the on-path still matches the enumerator.
CREATE TABLE ur1(g int);
INSERT INTO ur1 VALUES (1);
SELECT add_provenance('ur1');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ur1; END $$;
SELECT * FROM u_par('shared-R COUNT>=2 (declines)', 'SELECT g, probability_evaluate(provenance()) p FROM (SELECT ur1.g, us.k FROM ur1, us UNION SELECT ur1.g, ut.k FROM ur1, ut) u GROUP BY g HAVING count(*) >= 2');

DROP FUNCTION u_par(text, text);
SELECT remove_provenance('ua');
SELECT remove_provenance('ub');
SELECT remove_provenance('ur');
SELECT remove_provenance('us');
SELECT remove_provenance('ut');
SELECT remove_provenance('ur1');
DROP TABLE ua, ub, ur, us, ut, ur1;
