\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- ----------------------------------------------------------------------
-- Pin the BID (repair_key / gate_mulinput) arm of the marginal-vector
-- pre-pass (src/AggMarginalEvaluator.cpp).  A HAVING COUNT / SUM / AVG over
-- a mutually-exclusive block is exact: each block is a categorical (at most
-- one alternative present, Σp_i ≤ 1, the null arm contributing 0), and
-- blocks are independent, so the engine convolves them with the TID part.
-- off (cmp_probability_evaluation off) is the exact enumerator, on is the
-- engine; they must agree to four decimals, and on must actually FIRE (the
-- value-0-present vs empty-group distinction is the having_sum_zero corner).
-- ----------------------------------------------------------------------

CREATE FUNCTION bid_parity(opname text, q text)
  RETURNS TABLE(shape text, p_off numeric, p_on numeric, diff numeric) AS $$
DECLARE tok uuid; off_p numeric; on_p numeric;
BEGIN
  EXECUTE q INTO tok;                         -- circuit token (GUC-independent)
  EXECUTE 'SET provsql.cmp_probability_evaluation = off';
  off_p := round(probability_evaluate(tok)::numeric, 4);
  EXECUTE 'SET provsql.cmp_probability_evaluation = on';
  on_p := round(probability_evaluate(tok)::numeric, 4);
  RETURN QUERY SELECT opname, off_p, on_p, round(abs(off_p - on_p), 6);
END $$ LANGUAGE plpgsql;

-- A single block X with P(X=0)=0.5, P(X=10)=0.5 (Σp=1, never empty).
CREATE TABLE bid_z(d text, val int, p float);
INSERT INTO bid_z VALUES ('d', 0, 0.5), ('d', 10, 0.5);
SELECT repair_key('bid_z', 'd');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM bid_z; END $$;

SELECT * FROM bid_parity('z SUM < 5',  'SELECT provenance() FROM bid_z GROUP BY d HAVING sum(val) < 5');
SELECT * FROM bid_parity('z SUM = 0',  'SELECT provenance() FROM bid_z GROUP BY d HAVING sum(val) = 0');
SELECT * FROM bid_parity('z SUM >= 5', 'SELECT provenance() FROM bid_z GROUP BY d HAVING sum(val) >= 5');
SELECT * FROM bid_parity('z AVG < 5',  'SELECT provenance() FROM bid_z GROUP BY d HAVING avg(val) < 5');
SELECT * FROM bid_parity('z COUNT < 2','SELECT provenance() FROM bid_z GROUP BY d HAVING count(*) < 2');
-- MIN / MAX over a BID block: each block is an independent categorical, so a
-- value-thresholded subset is all-absent w.p. 1-Σp over its matching
-- alternatives; the engine fires and matches the enumerator.
SELECT * FROM bid_parity('z MIN < 5',   'SELECT provenance() FROM bid_z GROUP BY d HAVING min(val) < 5');
SELECT * FROM bid_parity('z MAX >= 10', 'SELECT provenance() FROM bid_z GROUP BY d HAVING max(val) >= 10');
SELECT * FROM bid_parity('z MAX = 0',   'SELECT provenance() FROM bid_z GROUP BY d HAVING max(val) = 0');

-- A block with a null outcome: P(=0)=0.3, P(=10)=0.3, P(empty)=0.4.
CREATE TABLE bid_n(e text, val int, p float);
INSERT INTO bid_n VALUES ('e', 0, 0.3), ('e', 10, 0.3);
SELECT repair_key('bid_n', 'e');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM bid_n; END $$;

-- present-and-0 (0.3); the empty world (0.4) is excluded though its sum is 0.
SELECT * FROM bid_parity('n SUM < 5',   'SELECT provenance() FROM bid_n GROUP BY e HAVING sum(val) < 5');
SELECT * FROM bid_parity('n COUNT >= 1','SELECT provenance() FROM bid_n GROUP BY e HAVING count(*) >= 1');

-- Two independent blocks in one group: X in {0,10}, Y in {0,1}.
CREATE TABLE bid_xy(blk text, c int, val int, p float);
INSERT INTO bid_xy VALUES ('X',1,0,0.5),('X',1,10,0.5),('Y',1,0,0.5),('Y',1,1,0.5);
SELECT repair_key('bid_xy', 'blk');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM bid_xy; END $$;

SELECT * FROM bid_parity('xy SUM < 11', 'SELECT provenance() FROM bid_xy GROUP BY c HAVING sum(val) < 11');
SELECT * FROM bid_parity('xy SUM <= 1', 'SELECT provenance() FROM bid_xy GROUP BY c HAVING sum(val) <= 1');
SELECT * FROM bid_parity('xy MAX >= 10','SELECT provenance() FROM bid_xy GROUP BY c HAVING max(val) >= 10');
SELECT * FROM bid_parity('xy MIN >= 1', 'SELECT provenance() FROM bid_xy GROUP BY c HAVING min(val) >= 1');

-- Confirm the BID arm actually FIRES (pin the "safe-join aggregate" NOTICE):
-- once for a single block, once for the two-block convolution.
SET provsql.verbose_level = 5;
SELECT provenance() AS t1 FROM bid_z  GROUP BY d HAVING sum(val) < 5  \gset
SELECT round(probability_evaluate(:'t1'::uuid)::numeric, 4) AS z_fires;
SELECT provenance() AS t2 FROM bid_xy GROUP BY c HAVING sum(val) < 11 \gset
SELECT round(probability_evaluate(:'t2'::uuid)::numeric, 4) AS xy_fires;
SELECT provenance() AS t3 FROM bid_z  GROUP BY d HAVING max(val) >= 10 \gset
SELECT round(probability_evaluate(:'t3'::uuid)::numeric, 4) AS max_fires;
SET provsql.verbose_level = 0;

DROP FUNCTION bid_parity(text, text);
SELECT remove_provenance('bid_z');
SELECT remove_provenance('bid_n');
SELECT remove_provenance('bid_xy');
DROP TABLE bid_z;
DROP TABLE bid_n;
DROP TABLE bid_xy;
