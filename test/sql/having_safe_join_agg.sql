\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- ----------------------------------------------------------------------
-- Pin the safe-join SUM / MIN / MAX arms of the marginal-vector pre-pass
-- (src/AggMarginalEvaluator.cpp) against exact possible-worlds
-- enumeration, the companion of having_safe_join_count.sql (which covers
-- COUNT).  SUM uses the weighted-sum distribution; MIN / MAX reduce to a
-- few "all of a value-thresholded subset absent" probabilities over the
-- same hierarchical recursion.  off (cmp_probability_evaluation off) is
-- the exact enumerator; on is the engine; they must agree to four
-- decimals on the fan-out and depth-2 shapes, and on the non-hierarchical
-- triangle the engine must decline and still match.
-- ----------------------------------------------------------------------

CREATE FUNCTION sja_parity(opname text, q text)
  RETURNS TABLE(shape text, g int, p_off numeric, p_on numeric, diff numeric)
AS $$
BEGIN
  EXECUTE 'SET provsql.cmp_probability_evaluation = off';
  EXECUTE format('CREATE TEMP TABLE sja_off AS %s', q);
  PERFORM remove_provenance('sja_off');
  EXECUTE 'SET provsql.cmp_probability_evaluation = on';
  EXECUTE format('CREATE TEMP TABLE sja_on AS %s', q);
  PERFORM remove_provenance('sja_on');
  RETURN QUERY
    SELECT opname, o.g,
           round(o.p::numeric, 4), round(n.p::numeric, 4),
           round(abs(o.p - n.p)::numeric, 6)
    FROM sja_off o JOIN sja_on n USING (g) ORDER BY o.g;
  DROP TABLE sja_off, sja_on;
END
$$ LANGUAGE plpgsql;

-- Fan-out R(k,a) JOIN S(a,b): the aggregated value is b, carried per join
-- tuple; contributors of a group share the R(k,a) leaf.
CREATE TABLE sja_r(k int, a int);
CREATE TABLE sja_s(a int, b int);
INSERT INTO sja_r VALUES (1,10),(1,20),(2,10);
INSERT INTO sja_s VALUES (10,3),(10,7),(10,5),(20,4),(20,9);
SELECT add_provenance('sja_r');
SELECT add_provenance('sja_s');
DO $$ BEGIN PERFORM set_prob(provenance(), (0.40 + 0.05*k + 0.013*a)) FROM sja_r; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), (0.35 + 0.02*b)) FROM sja_s; END $$;

SELECT * FROM sja_parity('SUM >= 15', 'SELECT k g, probability_evaluate(provenance()) p FROM sja_r JOIN sja_s ON sja_r.a=sja_s.a GROUP BY k HAVING sum(b) >= 15');
SELECT * FROM sja_parity('SUM <= 10', 'SELECT k g, probability_evaluate(provenance()) p FROM sja_r JOIN sja_s ON sja_r.a=sja_s.a GROUP BY k HAVING sum(b) <= 10');
SELECT * FROM sja_parity('SUM = 7',   'SELECT k g, probability_evaluate(provenance()) p FROM sja_r JOIN sja_s ON sja_r.a=sja_s.a GROUP BY k HAVING sum(b) = 7');
SELECT * FROM sja_parity('MAX >= 7',  'SELECT k g, probability_evaluate(provenance()) p FROM sja_r JOIN sja_s ON sja_r.a=sja_s.a GROUP BY k HAVING max(b) >= 7');
SELECT * FROM sja_parity('MAX <= 5',  'SELECT k g, probability_evaluate(provenance()) p FROM sja_r JOIN sja_s ON sja_r.a=sja_s.a GROUP BY k HAVING max(b) <= 5');
SELECT * FROM sja_parity('MAX = 7',   'SELECT k g, probability_evaluate(provenance()) p FROM sja_r JOIN sja_s ON sja_r.a=sja_s.a GROUP BY k HAVING max(b) = 7');
SELECT * FROM sja_parity('MIN <= 4',  'SELECT k g, probability_evaluate(provenance()) p FROM sja_r JOIN sja_s ON sja_r.a=sja_s.a GROUP BY k HAVING min(b) <= 4');
SELECT * FROM sja_parity('MIN >= 5',  'SELECT k g, probability_evaluate(provenance()) p FROM sja_r JOIN sja_s ON sja_r.a=sja_s.a GROUP BY k HAVING min(b) >= 5');
SELECT * FROM sja_parity('MIN <> 3',  'SELECT k g, probability_evaluate(provenance()) p FROM sja_r JOIN sja_s ON sja_r.a=sja_s.a GROUP BY k HAVING min(b) <> 3');

-- Depth-2 nesting acct(u), ord(u,o), item(u,o,i); aggregated value is i.
CREATE TABLE sja_acct(u int);
CREATE TABLE sja_ord(u int, o int);
CREATE TABLE sja_item(u int, o int, i int);
INSERT INTO sja_acct VALUES (1),(2);
INSERT INTO sja_ord VALUES (1,10),(1,20),(2,30);
INSERT INTO sja_item VALUES (1,10,3),(1,10,8),(1,20,5),(2,30,4),(2,30,9);
SELECT add_provenance('sja_acct');
SELECT add_provenance('sja_ord');
SELECT add_provenance('sja_item');
DO $$ BEGIN PERFORM set_prob(provenance(), (0.40 + 0.10*u)) FROM sja_acct; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), (0.30 + 0.01*o)) FROM sja_ord; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), (0.30 + 0.02*i)) FROM sja_item; END $$;

SELECT * FROM sja_parity('d2 SUM >= 12', 'SELECT a.u g, probability_evaluate(provenance()) p FROM sja_acct a, sja_ord o, sja_item t WHERE a.u=o.u AND o.u=t.u AND o.o=t.o GROUP BY a.u HAVING sum(i) >= 12');
SELECT * FROM sja_parity('d2 MAX >= 8',  'SELECT a.u g, probability_evaluate(provenance()) p FROM sja_acct a, sja_ord o, sja_item t WHERE a.u=o.u AND o.u=t.u AND o.o=t.o GROUP BY a.u HAVING max(i) >= 8');
SELECT * FROM sja_parity('d2 MIN <= 4',  'SELECT a.u g, probability_evaluate(provenance()) p FROM sja_acct a, sja_ord o, sja_item t WHERE a.u=o.u AND o.u=t.u AND o.o=t.o GROUP BY a.u HAVING min(i) <= 4');

-- Non-hierarchical triangle: the SUM / MAX arms must decline and fall
-- back to exact enumeration (off and on still agree).
CREATE TABLE sja_tr(x int);
CREATE TABLE sja_ts(x int, y int);
CREATE TABLE sja_tt(y int);
INSERT INTO sja_tr VALUES (1),(2);
INSERT INTO sja_ts VALUES (1,10),(1,20),(2,10);
INSERT INTO sja_tt VALUES (10),(20);
SELECT add_provenance('sja_tr');
SELECT add_provenance('sja_ts');
SELECT add_provenance('sja_tt');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sja_tr; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sja_ts; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sja_tt; END $$;

SELECT * FROM sja_parity('tri SUM >= 20', 'SELECT 1 g, probability_evaluate(provenance()) p FROM sja_tr, sja_ts, sja_tt WHERE sja_tr.x=sja_ts.x AND sja_ts.y=sja_tt.y GROUP BY 1 HAVING sum(sja_ts.y) >= 20');
SELECT * FROM sja_parity('tri MAX >= 15', 'SELECT 1 g, probability_evaluate(provenance()) p FROM sja_tr, sja_ts, sja_tt WHERE sja_tr.x=sja_ts.x AND sja_ts.y=sja_tt.y GROUP BY 1 HAVING max(sja_ts.y) >= 15');

-- Cross-product / product-join R(a),S(a,b),T(a,c) with the aggregated
-- value on ONE branch: SUM = S_b · N_T (count-product of the other
-- factor); MIN / MAX reduce to pAllAbsent over the product (a
-- value-thresholded subset on a single branch is itself a sub-product).
-- These fire.  A value spanning both branches (b+c) couples the factors;
-- SUM bails (and MIN/MAX bail unless the threshold subset happens to be a
-- product) -- either way off and on agree.
CREATE TABLE sja_pr(a int);
CREATE TABLE sja_ps(a int, b int);
CREATE TABLE sja_pt(a int, c int);
INSERT INTO sja_pr VALUES (1),(2);
INSERT INTO sja_ps VALUES (1,3),(1,7),(2,5);
INSERT INTO sja_pt VALUES (1,4),(1,9),(2,6);
SELECT add_provenance('sja_pr');
SELECT add_provenance('sja_ps');
SELECT add_provenance('sja_pt');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sja_pr; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sja_ps; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sja_pt; END $$;

SELECT * FROM sja_parity('xprod SUM(b) >= 10', 'SELECT sja_pr.a g, probability_evaluate(provenance()) p FROM sja_pr, sja_ps, sja_pt WHERE sja_pr.a=sja_ps.a AND sja_pr.a=sja_pt.a GROUP BY sja_pr.a HAVING sum(sja_ps.b) >= 10');
SELECT * FROM sja_parity('xprod MAX(b) >= 7', 'SELECT sja_pr.a g, probability_evaluate(provenance()) p FROM sja_pr, sja_ps, sja_pt WHERE sja_pr.a=sja_ps.a AND sja_pr.a=sja_pt.a GROUP BY sja_pr.a HAVING max(sja_ps.b) >= 7');
SELECT * FROM sja_parity('xprod MIN(b) <= 3', 'SELECT sja_pr.a g, probability_evaluate(provenance()) p FROM sja_pr, sja_ps, sja_pt WHERE sja_pr.a=sja_ps.a AND sja_pr.a=sja_pt.a GROUP BY sja_pr.a HAVING min(sja_ps.b) <= 3');
SELECT * FROM sja_parity('xprod MAX(c) >= 9', 'SELECT sja_pr.a g, probability_evaluate(provenance()) p FROM sja_pr, sja_ps, sja_pt WHERE sja_pr.a=sja_ps.a AND sja_pr.a=sja_pt.a GROUP BY sja_pr.a HAVING max(sja_pt.c) >= 9');
SELECT * FROM sja_parity('xprod SUM(b+c) span', 'SELECT sja_pr.a g, probability_evaluate(provenance()) p FROM sja_pr, sja_ps, sja_pt WHERE sja_pr.a=sja_ps.a AND sja_pr.a=sja_pt.a GROUP BY sja_pr.a HAVING sum(sja_ps.b + sja_pt.c) >= 20');

-- AVG: reduces to SUM(v - C) op 0 (empty group excluded), so it inherits
-- the whole laminar / product machinery -- and is closed-form even on the
-- flat single table, which no prior pre-pass covered.  Only integer
-- thresholds reach here (a fractional HAVING-AVG constant is rejected
-- upstream).  fan-out, depth-2 nesting, and the cross-product all fire.
SELECT * FROM sja_parity('fanout AVG(b) >= 5', 'SELECT k g, probability_evaluate(provenance()) p FROM sja_r JOIN sja_s ON sja_r.a=sja_s.a GROUP BY k HAVING avg(b) >= 5');
SELECT * FROM sja_parity('fanout AVG(b) <= 5', 'SELECT k g, probability_evaluate(provenance()) p FROM sja_r JOIN sja_s ON sja_r.a=sja_s.a GROUP BY k HAVING avg(b) <= 5');
SELECT * FROM sja_parity('depth2 AVG(i) >= 4', 'SELECT a.u g, probability_evaluate(provenance()) p FROM sja_acct a, sja_ord o, sja_item t WHERE a.u=o.u AND o.u=t.u AND o.o=t.o GROUP BY a.u HAVING avg(i) >= 4');
SELECT * FROM sja_parity('xprod AVG(b) >= 5', 'SELECT sja_pr.a g, probability_evaluate(provenance()) p FROM sja_pr, sja_ps, sja_pt WHERE sja_pr.a=sja_ps.a AND sja_pr.a=sja_pt.a GROUP BY sja_pr.a HAVING avg(sja_ps.b) >= 5');

DROP FUNCTION sja_parity(text, text);
SELECT remove_provenance('sja_r');
SELECT remove_provenance('sja_s');
SELECT remove_provenance('sja_acct');
SELECT remove_provenance('sja_ord');
SELECT remove_provenance('sja_item');
SELECT remove_provenance('sja_tr');
SELECT remove_provenance('sja_ts');
SELECT remove_provenance('sja_tt');
SELECT remove_provenance('sja_pr');
SELECT remove_provenance('sja_ps');
SELECT remove_provenance('sja_pt');
DROP TABLE sja_r, sja_s, sja_acct, sja_ord, sja_item, sja_tr, sja_ts, sja_tt, sja_pr, sja_ps, sja_pt;
