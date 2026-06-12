\set ECHO none
\pset format unaligned

-- ----------------------------------------------------------------------
-- HAVING aggregate comparisons over non-integer value domains.  The
-- comparison domain is the aggregate's result type, stored in info2 of
-- the gate_agg (set by provenance_aggregate); the enumeration path reads
-- it and, for a numeric / finite-decimal float column, scales the values
-- and threshold to a common integer grid by their decimal text -- so
-- numeric(p,d) is evaluated exactly and fractional thresholds work.
-- These previously raised "does not support value gates" (numeric/float
-- value) or "text constant ... only for choose()" (fractional threshold).
-- Results are materialised and stripped of their (per-run random)
-- provenance token before display.
-- ----------------------------------------------------------------------

CREATE TABLE hn(g int, vn numeric(10,2), vf float8);
INSERT INTO hn VALUES (1, 2.50, 2.5), (1, 4.25, 4.25), (1, 3.00, 3.0), (2, 10.00, 10.0);
SELECT add_provenance('hn');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM hn; END $$;

CREATE FUNCTION hn_p(opname text, q text)
  RETURNS TABLE(shape text, g int, p numeric)
AS $$
BEGIN
  EXECUTE format('CREATE TEMP TABLE hn_r AS %s', q);
  PERFORM remove_provenance('hn_r');
  RETURN QUERY SELECT opname, r.g, round(r.p::numeric, 4) FROM hn_r r ORDER BY r.g;
  DROP TABLE hn_r;
END
$$ LANGUAGE plpgsql;

-- numeric(10,2): exact decimal scaling
SELECT * FROM hn_p('num SUM>=6',   'SELECT g, probability_evaluate(provenance()) p FROM hn GROUP BY g HAVING sum(vn) >= 6');
SELECT * FROM hn_p('num SUM>=6.5', 'SELECT g, probability_evaluate(provenance()) p FROM hn GROUP BY g HAVING sum(vn) >= 6.5');
SELECT * FROM hn_p('num MAX>=4',   'SELECT g, probability_evaluate(provenance()) p FROM hn GROUP BY g HAVING max(vn) >= 4');
SELECT * FROM hn_p('num MIN<=3',   'SELECT g, probability_evaluate(provenance()) p FROM hn GROUP BY g HAVING min(vn) <= 3');
SELECT * FROM hn_p('num AVG>=3.5', 'SELECT g, probability_evaluate(provenance()) p FROM hn GROUP BY g HAVING avg(vn) >= 3.5');
SELECT * FROM hn_p('num SUM=9.75', 'SELECT g, probability_evaluate(provenance()) p FROM hn GROUP BY g HAVING sum(vn) = 9.75');

-- float8: finite-decimal text, same scaling
SELECT * FROM hn_p('flt SUM>=6',   'SELECT g, probability_evaluate(provenance()) p FROM hn GROUP BY g HAVING sum(vf) >= 6');
SELECT * FROM hn_p('flt MAX>=4.25','SELECT g, probability_evaluate(provenance()) p FROM hn GROUP BY g HAVING max(vf) >= 4.25');

DROP FUNCTION hn_p(text, text);
SELECT remove_provenance('hn');
DROP TABLE hn;

-- Closed-form vs enumeration on a numeric safe-join (fan-out): the
-- marginal-vector engine fires for numeric just as for integer (values
-- scaled to a common integer grid), so off (enumeration) and on
-- (closed-form) must agree exactly.
CREATE TABLE hnr(k int, a int);
CREATE TABLE hns(a int, b numeric(10,2));
INSERT INTO hnr VALUES (1,10),(1,20),(2,10);
INSERT INTO hns VALUES (10,1.50),(10,2.50),(10,3.25),(20,4.00),(20,0.75);
SELECT add_provenance('hnr');
SELECT add_provenance('hns');
DO $$ BEGIN PERFORM set_prob(provenance(), (0.40 + 0.05*k + 0.013*a)) FROM hnr; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), (0.35 + 0.02*(b*4)::int)) FROM hns; END $$;

CREATE FUNCTION hn_par(opname text, q text)
  RETURNS TABLE(shape text, g int, p_off numeric, p_on numeric, diff numeric)
AS $$
BEGIN
  EXECUTE 'SET provsql.cmp_probability_evaluation = off';
  EXECUTE format('CREATE TEMP TABLE hn_o AS %s', q);
  PERFORM remove_provenance('hn_o');
  EXECUTE 'SET provsql.cmp_probability_evaluation = on';
  EXECUTE format('CREATE TEMP TABLE hn_n AS %s', q);
  PERFORM remove_provenance('hn_n');
  RETURN QUERY
    SELECT opname, o.g, round(o.p::numeric, 4), round(n.p::numeric, 4),
           round(abs(o.p - n.p)::numeric, 6)
    FROM hn_o o JOIN hn_n n USING (g) ORDER BY o.g;
  DROP TABLE hn_o, hn_n;
END
$$ LANGUAGE plpgsql;

SELECT * FROM hn_par('join SUM(b)>=6',   'SELECT k g, probability_evaluate(provenance()) p FROM hnr JOIN hns ON hnr.a=hns.a GROUP BY k HAVING sum(b) >= 6');
SELECT * FROM hn_par('join SUM(b)>=5.5', 'SELECT k g, probability_evaluate(provenance()) p FROM hnr JOIN hns ON hnr.a=hns.a GROUP BY k HAVING sum(b) >= 5.5');
SELECT * FROM hn_par('join MAX(b)>=3.25','SELECT k g, probability_evaluate(provenance()) p FROM hnr JOIN hns ON hnr.a=hns.a GROUP BY k HAVING max(b) >= 3.25');
SELECT * FROM hn_par('join AVG(b)>=2.5', 'SELECT k g, probability_evaluate(provenance()) p FROM hnr JOIN hns ON hnr.a=hns.a GROUP BY k HAVING avg(b) >= 2.5');

DROP FUNCTION hn_par(text, text);
SELECT remove_provenance('hnr');
SELECT remove_provenance('hns');
DROP TABLE hnr, hns;
