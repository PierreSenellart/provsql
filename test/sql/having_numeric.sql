\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

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
