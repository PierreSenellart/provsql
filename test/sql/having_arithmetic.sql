\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Constant arithmetic over an aggregate inside a HAVING comparison is folded
-- into the threshold (sum(x)+1 > 16 == sum(x) > 15), so every arithmetic form
-- below yields the same per-group probabilities as its plain equivalent shown
-- alongside it.

CREATE TABLE ha(g text, x int);
SELECT add_provenance('ha');
INSERT INTO ha VALUES ('A',10),('A',20),('B',5),('B',30);
DO $$ BEGIN PERFORM provsql.set_prob(provsql.provenance(), 0.5) FROM provsql_test.ha; END $$;

-- Evaluate one HAVING predicate and print its per-group probabilities.
CREATE OR REPLACE FUNCTION ha_probs(pred text) RETURNS void AS $$
BEGIN
  EXECUTE format(
    'CREATE TABLE provsql_test.ha_r AS SELECT g, provsql.probability_evaluate(provsql.provenance()) p '
    'FROM provsql_test.ha GROUP BY g HAVING %s', pred);
  PERFORM provsql.remove_provenance('provsql_test.ha_r');
END $$ LANGUAGE plpgsql;

\echo '# plain sum(x) > 15  /  >= 15'
SELECT ha_probs('sum(x) > 15');  SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT ha_probs('sum(x) >= 15'); SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;

\echo '# additive forms (== sum(x) > 15, sum(x) >= 15)'
SELECT ha_probs('sum(x)+1 > 16');   SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT ha_probs('1+sum(x) > 16');   SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT ha_probs('sum(x)-5 >= 10');  SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;

\echo '# multiplicative forms (positive, negative-with-flip, division)'
SELECT ha_probs('sum(x)*2 > 30');     SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT ha_probs('2*sum(x) > 30');     SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT ha_probs('sum(x)*(-1) < -15'); SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT ha_probs('sum(x)/2 >= 5');     SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;

\echo '# unary minus (with flip) and a nested combination, both == sum(x) > 15'
SELECT ha_probs('-sum(x) < -15');     SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT ha_probs('(sum(x)+1)*2 > 32'); SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;

\echo '# non-exact / non-terminating multipliers (fractional thresholds)'
SELECT ha_probs('sum(x)*2 > 31');  SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT ha_probs('sum(x) > 15.5');  SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT ha_probs('sum(x)*3 > 10');  SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;

\echo '# threshold robustness: trailing-zero high scale, and a large magnitude'
\echo '#   sum(x) > 15.0000000000000000  ==  sum(x) > 15  (scale trimmed)'
SELECT ha_probs('sum(x) > 15.0000000000000000'); SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
\echo '#   sum(x) > 1000000000  (large threshold -> exact-enumeration fallback)'
SELECT ha_probs('sum(x) > 1000000000'); SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;

\echo '# min/max arithmetic (different evaluator) folds the same way'
SELECT ha_probs('max(x)+5 > 35');   SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT ha_probs('max(x) > 30');     SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT ha_probs('min(x)*2 <= 20');  SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT ha_probs('min(x) <= 10');    SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;

\echo '# count(*) arithmetic'
SELECT ha_probs('count(*) >= 2');  SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT ha_probs('count(*)+1 >= 3'); SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT ha_probs('count(*)*10 >= 20'); SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;

\echo '# distributive arithmetic is pushed into the aggregate: sum(x)*2 is a clean'
\echo '# agg gate (not arith), so a materialised column stays comparable later'
CREATE TABLE har AS SELECT g, sum(x) AS s, count(*) AS c, sum(x)*2 AS s2 FROM ha GROUP BY g;
\echo '#   gate type of the pushed sum(x)*2 column (expect agg, not arith)'
SET provsql.active = off;
SELECT g, get_gate_type(s2::uuid) AS gate FROM har ORDER BY g;
SET provsql.active = on;
\echo '#   WHERE s2 > 30 over the materialised pushed column (== sum(x) > 15)'
CREATE TABLE ha_r AS SELECT g, probability_evaluate(provenance()) p FROM har WHERE s2 > 30;
SELECT remove_provenance('ha_r'); SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
CREATE OR REPLACE FUNCTION har_probs(pred text) RETURNS void AS $$
BEGIN
  EXECUTE format(
    'CREATE TABLE provsql_test.ha_r AS SELECT g, provsql.probability_evaluate(provsql.provenance()) p '
    'FROM provsql_test.har WHERE %s', pred);
  PERFORM provsql.remove_provenance('provsql_test.ha_r');
END $$ LANGUAGE plpgsql;
SELECT har_probs('s+1 > 16');   SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT har_probs('s > 15');     SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT har_probs('s*2 > 30');   SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
SELECT har_probs('c*10 >= 20'); SELECT g, round(p::numeric,3) FROM ha_r ORDER BY g; DROP TABLE ha_r;
DROP FUNCTION har_probs(text);
DROP TABLE har;

DROP FUNCTION ha_probs(text);
DROP TABLE ha;

-- ---------------------------------------------------------------------
-- Comparisons that do not reduce to a single aggregate vs a constant
-- (agg vs agg, products of aggregates, constant / aggregate) are resolved
-- by the general possible-worlds enumeration, for any m-semiring.
-- ---------------------------------------------------------------------
CREATE TABLE tt(id text, g text, x int, y int);
SELECT add_provenance('tt');
INSERT INTO tt VALUES ('t1','A',10,5),('t2','A',20,30);
DO $$ BEGIN PERFORM provsql.set_prob(provsql.provenance(), 0.5) FROM provsql_test.tt; END $$;
SELECT create_provenance_mapping('ttm','tt','id');

\echo '# probabilities by exact possible-worlds enumeration'
\echo '#   sum(x) > sum(y)  (only world {t1}: 10>5)  -> 0.25'
CREATE TABLE tt_r AS SELECT probability_evaluate(provenance()) p FROM tt GROUP BY g HAVING sum(x) > sum(y);
SELECT remove_provenance('tt_r'); SELECT round(p::numeric,4) FROM tt_r; DROP TABLE tt_r;
\echo '#   sum(x)*sum(y) > 200  (worlds {t2},{t1,t2})  -> 0.5'
CREATE TABLE tt_r AS SELECT probability_evaluate(provenance()) p FROM tt GROUP BY g HAVING sum(x)*sum(y) > 200;
SELECT remove_provenance('tt_r'); SELECT round(p::numeric,4) FROM tt_r; DROP TABLE tt_r;
\echo '#   100/sum(x) > 5  (only world {t1}: sum 10 < 20)  -> 0.25'
CREATE TABLE tt_r AS SELECT probability_evaluate(provenance()) p FROM tt GROUP BY g HAVING 100/sum(x) > 5;
SELECT remove_provenance('tt_r'); SELECT round(p::numeric,4) FROM tt_r; DROP TABLE tt_r;

\echo '# m-semiring genericity: the formula semiring gives the valid-world expression'
\echo '#   sum(x) > sum(y): the single world {t1} -> t1 with t2 absent'
CREATE TABLE tt_r AS SELECT sr_formula(provenance(),'ttm') AS f FROM tt GROUP BY g HAVING sum(x) > sum(y);
SELECT remove_provenance('tt_r'); SELECT f FROM tt_r; DROP TABLE tt_r;

DROP TABLE tt;
