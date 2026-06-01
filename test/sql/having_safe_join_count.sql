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

-- Depth-2 hierarchical nesting: acct(u), ord(u,o), item(u,o,i) -- the
-- aggregated rows nest items inside orders inside a user.  A group's
-- contributors share acct(u) at the top level and ord(u,o) one level
-- down, so the engine must recurse (mixture over the user root, then a
-- nested mixture/convolution over orders and items) rather than treat the
-- order leaves as flat private fan-out.
CREATE TABLE sj_acct(u int);
CREATE TABLE sj_ord(u int, o int);
CREATE TABLE sj_item(u int, o int, i int);
INSERT INTO sj_acct VALUES (1),(2);
INSERT INTO sj_ord VALUES (1,10),(1,20),(2,30);
INSERT INTO sj_item VALUES (1,10,100),(1,10,200),(1,20,300),(2,30,400),(2,30,500);
SELECT add_provenance('sj_acct');
SELECT add_provenance('sj_ord');
SELECT add_provenance('sj_item');
DO $$ BEGIN PERFORM set_prob(provenance(), (0.40 + 0.10*u)) FROM sj_acct; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), (0.30 + 0.01*o)) FROM sj_ord; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), (0.30 + 0.0005*i)) FROM sj_item; END $$;

SELECT * FROM sj_parity('depth2 >= 2', 'SELECT a.u g, probability_evaluate(provenance()) p FROM sj_acct a, sj_ord o, sj_item t WHERE a.u=o.u AND o.u=t.u AND o.o=t.o GROUP BY a.u HAVING count(*) >= 2');
SELECT * FROM sj_parity('depth2 = 2',  'SELECT a.u g, probability_evaluate(provenance()) p FROM sj_acct a, sj_ord o, sj_item t WHERE a.u=o.u AND o.u=t.u AND o.o=t.o GROUP BY a.u HAVING count(*) = 2');
SELECT * FROM sj_parity('depth2 <= 2', 'SELECT a.u g, probability_evaluate(provenance()) p FROM sj_acct a, sj_ord o, sj_item t WHERE a.u=o.u AND o.u=t.u AND o.o=t.o GROUP BY a.u HAVING count(*) <= 2');

-- Nested gate_times via an SPJ subquery: the inner (sj_r JOIN sj_s) tuple
-- provenance feeds the outer join with sj_jt as times(times(r,s), t).
-- parseProductContributor must flatten the nesting (times is AND on the
-- probability path) so the laminar chain is still recognised -- a flat
-- comma join and this subquery form must give the same probability.
CREATE TABLE sj_jt(b int, c int);
INSERT INTO sj_jt VALUES (100,1),(200,1),(300,1);
SELECT add_provenance('sj_jt');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sj_jt; END $$;

SELECT * FROM sj_parity('subquery >= 2', 'SELECT k g, probability_evaluate(provenance()) p FROM (SELECT sj_r.k, sj_s.b FROM sj_r JOIN sj_s ON sj_r.a=sj_s.a) sub JOIN sj_jt ON sub.b=sj_jt.b GROUP BY k HAVING count(*) >= 2');
SELECT * FROM sj_parity('subquery <= 3', 'SELECT k g, probability_evaluate(provenance()) p FROM (SELECT sj_r.k, sj_s.b FROM sj_r JOIN sj_s ON sj_r.a=sj_s.a) sub JOIN sj_jt ON sub.b=sj_jt.b GROUP BY k HAVING count(*) <= 3');

-- Cross-product / product-join R(a),S(a,b),T(a,c): safe but NOT laminar --
-- after factoring the shared R(a), the residuals form a complete
-- leaf-disjoint product, so count = N_S * N_T.  The engine must recognise
-- this on the circuit (no S-S-style middle leaf links the two branches)
-- and fire, unlike the h0 case below.  sj_pu adds a third independent
-- branch (count = N_S * N_T * N_U).
CREATE TABLE sj_pr(a int);
CREATE TABLE sj_ps(a int, b int);
CREATE TABLE sj_pt(a int, c int);
CREATE TABLE sj_pu(a int, d int);
INSERT INTO sj_pr VALUES (1),(2);
INSERT INTO sj_ps VALUES (1,10),(1,20),(2,30);
INSERT INTO sj_pt VALUES (1,100),(1,200),(2,300);
INSERT INTO sj_pu VALUES (1,7),(1,8),(2,9);
SELECT add_provenance('sj_pr');
SELECT add_provenance('sj_ps');
SELECT add_provenance('sj_pt');
SELECT add_provenance('sj_pu');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sj_pr; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sj_ps; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sj_pt; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sj_pu; END $$;

SELECT * FROM sj_parity('xprod >= 2', 'SELECT sj_pr.a g, probability_evaluate(provenance()) p FROM sj_pr, sj_ps, sj_pt WHERE sj_pr.a=sj_ps.a AND sj_pr.a=sj_pt.a GROUP BY sj_pr.a HAVING count(*) >= 2');
SELECT * FROM sj_parity('xprod = 4',  'SELECT sj_pr.a g, probability_evaluate(provenance()) p FROM sj_pr, sj_ps, sj_pt WHERE sj_pr.a=sj_ps.a AND sj_pr.a=sj_pt.a GROUP BY sj_pr.a HAVING count(*) = 4');
SELECT * FROM sj_parity('xprod <= 2', 'SELECT sj_pr.a g, probability_evaluate(provenance()) p FROM sj_pr, sj_ps, sj_pt WHERE sj_pr.a=sj_ps.a AND sj_pr.a=sj_pt.a GROUP BY sj_pr.a HAVING count(*) <= 2');
SELECT * FROM sj_parity('3factor >= 4', 'SELECT sj_pr.a g, probability_evaluate(provenance()) p FROM sj_pr, sj_ps, sj_pt, sj_pu WHERE sj_pr.a=sj_ps.a AND sj_pr.a=sj_pt.a AND sj_pr.a=sj_pu.a GROUP BY sj_pr.a HAVING count(*) >= 4');

-- Incomplete product: a predicate that drops one (b,c) combination links
-- the two branches, so the contributors are NOT a complete product (the
-- #P-hard bipartite case).  The completeness check must fail and the
-- engine decline -- off and on still agree (via enumeration).
SELECT * FROM sj_parity('xprod filtered', 'SELECT sj_pr.a g, probability_evaluate(provenance()) p FROM sj_pr, sj_ps, sj_pt WHERE sj_pr.a=sj_ps.a AND sj_pr.a=sj_pt.a AND NOT (sj_ps.b = 10 AND sj_pt.c = 100) GROUP BY sj_pr.a HAVING count(*) >= 2');

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
SELECT remove_provenance('sj_acct');
SELECT remove_provenance('sj_ord');
SELECT remove_provenance('sj_item');
SELECT remove_provenance('sj_jt');
SELECT remove_provenance('sj_pr');
SELECT remove_provenance('sj_ps');
SELECT remove_provenance('sj_pt');
SELECT remove_provenance('sj_pu');
SELECT remove_provenance('sj_rr');
SELECT remove_provenance('sj_ss');
SELECT remove_provenance('sj_tt');
SELECT remove_provenance('sj_flat');
DROP TABLE sj_r, sj_s, sj_f, sj_d1, sj_d2, sj_acct, sj_ord, sj_item, sj_jt, sj_pr, sj_ps, sj_pt, sj_pu, sj_rr, sj_ss, sj_tt, sj_flat;
