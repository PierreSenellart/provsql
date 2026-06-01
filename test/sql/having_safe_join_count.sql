\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- ----------------------------------------------------------------------
-- Pin the safe-join COUNT marginal-vector pre-pass
-- (src/AggMarginalEvaluator.cpp) against the exact possible-worlds
-- enumeration.  With provsql.cmp_probability_evaluation off, a HAVING
-- COUNT(*) op C over a *join* (whose contributors share a base leaf and
-- so are not independent) falls through to provsql_having's exponential
-- enumerate_valid_worlds -- the exact baseline.  With it on, the
-- marginal-vector engine resolves the cmp by block mixture + convolution.
-- The two must agree to four decimals on every supported operator and on
-- the single-level fan-out, star, and degenerate flat shapes; and on the
-- non-hierarchical triangle (R(x),S(x,y),T(y)) the engine must decline
-- and still match the enumerator (it bails, never a wrong "safe").
-- ----------------------------------------------------------------------

-- Parity helper: run <q> with the pre-pass off then on, print the
-- per-group probabilities and their absolute difference side by side.
CREATE FUNCTION sj_parity(opname text, q text)
  RETURNS TABLE(shape text, g int, p_off numeric, p_on numeric, diff numeric)
AS $$
BEGIN
  EXECUTE 'SET provsql.cmp_probability_evaluation = off';
  EXECUTE format('CREATE TEMP TABLE sj_off AS %s', q);
  PERFORM remove_provenance('sj_off');
  EXECUTE 'SET provsql.cmp_probability_evaluation = on';
  EXECUTE format('CREATE TEMP TABLE sj_on AS %s', q);
  PERFORM remove_provenance('sj_on');
  RETURN QUERY
    SELECT opname, o.g,
           round(o.p::numeric, 4), round(n.p::numeric, 4),
           round(abs(o.p - n.p)::numeric, 6)
    FROM sj_off o JOIN sj_on n USING (g) ORDER BY o.g;
  DROP TABLE sj_off, sj_on;
END
$$ LANGUAGE plpgsql;

-- Single-level fan-out: R(k,a) JOIN S(a,b); each (k,a) of R pairs with
-- several b of S(a,b), so contributors of one group share the R(k,a)
-- leaf.  Probabilities derived deterministically from the key columns.
CREATE TABLE sj_r(k int, a int);
CREATE TABLE sj_s(a int, b int);
INSERT INTO sj_r VALUES (1,10),(1,20),(2,10);
INSERT INTO sj_s VALUES (10,100),(10,200),(10,300),(20,100),(20,200);
SELECT add_provenance('sj_r');
SELECT add_provenance('sj_s');
DO $$ BEGIN PERFORM set_prob(provenance(), (0.40 + 0.05*k + 0.013*a)) FROM sj_r; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), (0.30 + 0.011*a + 0.0007*b)) FROM sj_s; END $$;

SELECT * FROM sj_parity('fanout >= 2', 'SELECT k g, probability_evaluate(provenance()) p FROM sj_r JOIN sj_s ON sj_r.a=sj_s.a GROUP BY k HAVING count(*) >= 2');
SELECT * FROM sj_parity('fanout >= 3', 'SELECT k g, probability_evaluate(provenance()) p FROM sj_r JOIN sj_s ON sj_r.a=sj_s.a GROUP BY k HAVING count(*) >= 3');
SELECT * FROM sj_parity('fanout > 4',  'SELECT k g, probability_evaluate(provenance()) p FROM sj_r JOIN sj_s ON sj_r.a=sj_s.a GROUP BY k HAVING count(*) > 4');
SELECT * FROM sj_parity('fanout <= 3', 'SELECT k g, probability_evaluate(provenance()) p FROM sj_r JOIN sj_s ON sj_r.a=sj_s.a GROUP BY k HAVING count(*) <= 3');
SELECT * FROM sj_parity('fanout < 3',  'SELECT k g, probability_evaluate(provenance()) p FROM sj_r JOIN sj_s ON sj_r.a=sj_s.a GROUP BY k HAVING count(*) < 3');
SELECT * FROM sj_parity('fanout = 2',  'SELECT k g, probability_evaluate(provenance()) p FROM sj_r JOIN sj_s ON sj_r.a=sj_s.a GROUP BY k HAVING count(*) = 2');
SELECT * FROM sj_parity('fanout <> 2', 'SELECT k g, probability_evaluate(provenance()) p FROM sj_r JOIN sj_s ON sj_r.a=sj_s.a GROUP BY k HAVING count(*) <> 2');

-- Three-way star: F(k,a) JOIN D1(a) JOIN D2(a); the two dimension leaves
-- common to a group's members form the shared root, the fact leaves are
-- private.
CREATE TABLE sj_f(k int, a int);
CREATE TABLE sj_d1(a int);
CREATE TABLE sj_d2(a int);
INSERT INTO sj_f VALUES (1,10),(1,20),(1,10),(2,20);
INSERT INTO sj_d1 VALUES (10),(20);
INSERT INTO sj_d2 VALUES (10),(20);
SELECT add_provenance('sj_f');
SELECT add_provenance('sj_d1');
SELECT add_provenance('sj_d2');
DO $$ BEGIN PERFORM set_prob(provenance(), (0.40 + 0.03*k + 0.007*a)) FROM sj_f; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), (0.60 + 0.001*a)) FROM sj_d1; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), (0.50 + 0.002*a)) FROM sj_d2; END $$;

SELECT * FROM sj_parity('star >= 2', 'SELECT k g, probability_evaluate(provenance()) p FROM sj_f,sj_d1,sj_d2 WHERE sj_f.a=sj_d1.a AND sj_f.a=sj_d2.a GROUP BY k HAVING count(*) >= 2');
SELECT * FROM sj_parity('star = 1',  'SELECT k g, probability_evaluate(provenance()) p FROM sj_f,sj_d1,sj_d2 WHERE sj_f.a=sj_d1.a AND sj_f.a=sj_d2.a GROUP BY k HAVING count(*) = 1');

-- Non-hierarchical triangle R(x),S(x,y),T(y): the engine must DECLINE
-- (no leaf common to all members of the block) and fall back to exact
-- enumeration, so off and on still agree.
CREATE TABLE sj_rr(x int);
CREATE TABLE sj_ss(x int, y int);
CREATE TABLE sj_tt(y int);
INSERT INTO sj_rr VALUES (1),(2);
INSERT INTO sj_ss VALUES (1,10),(1,20),(2,10);
INSERT INTO sj_tt VALUES (10),(20);
SELECT add_provenance('sj_rr');
SELECT add_provenance('sj_ss');
SELECT add_provenance('sj_tt');
DO $$ BEGIN PERFORM set_prob(provenance(), (0.30 + 0.10*x)) FROM sj_rr; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), (0.40 + 0.01*x + 0.005*y)) FROM sj_ss; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), (0.50 + 0.002*y)) FROM sj_tt; END $$;

SELECT * FROM sj_parity('triangle >= 2', 'SELECT 1 g, probability_evaluate(provenance()) p FROM sj_rr,sj_ss,sj_tt WHERE sj_rr.x=sj_ss.x AND sj_ss.y=sj_tt.y GROUP BY 1 HAVING count(*) >= 2');
SELECT * FROM sj_parity('triangle >= 3', 'SELECT 1 g, probability_evaluate(provenance()) p FROM sj_rr,sj_ss,sj_tt WHERE sj_rr.x=sj_ss.x AND sj_ss.y=sj_tt.y GROUP BY 1 HAVING count(*) >= 3');

-- Degenerate flat single-table: the flat Poisson-binomial pre-pass
-- (runCountCmpEvaluator) handles it first; the safe-join engine must be
-- a no-op and the result still matches.
CREATE TABLE sj_flat(c int, v int);
INSERT INTO sj_flat VALUES (1,1),(1,2),(1,3),(2,1);
SELECT add_provenance('sj_flat');
DO $$ BEGIN PERFORM set_prob(provenance(), (0.30 + 0.10*v)) FROM sj_flat; END $$;

SELECT * FROM sj_parity('flat >= 2', 'SELECT c g, probability_evaluate(provenance()) p FROM sj_flat GROUP BY c HAVING count(*) >= 2');

DROP FUNCTION sj_parity(text, text);
SELECT remove_provenance('sj_r');
SELECT remove_provenance('sj_s');
SELECT remove_provenance('sj_f');
SELECT remove_provenance('sj_d1');
SELECT remove_provenance('sj_d2');
SELECT remove_provenance('sj_rr');
SELECT remove_provenance('sj_ss');
SELECT remove_provenance('sj_tt');
SELECT remove_provenance('sj_flat');
DROP TABLE sj_r, sj_s, sj_f, sj_d1, sj_d2, sj_rr, sj_ss, sj_tt, sj_flat;
