\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Two fixes exercised together:
--  (1) provenance() inside a SubLink that is NOT a simple inert fetch
--      (here: an aggregated scalar subselect, and an EXISTS) is
--      unsupported -- ProvSQL raises a clear error.  A plain read of the
--      provsql column inside a SubLink stays legal.  (The simple
--      `SELECT provenance() FROM R WHERE ...` scalar fetch is now an inert
--      token read -- see inert_provenance_sublink.)
--  (2) INSERT ... SELECT whose source SELECT uses provenance() / HAVING is now
--      rewritten (HAVING lifted into provenance) even when the target table is
--      not provenance-tracked, so the SELECT returns the right rows (it used to
--      leave HAVING on the physical rows and insert nothing).

CREATE TABLE isq(d text, val int, p float);
INSERT INTO isq VALUES ('d',0,0.5),('d',10,0.5);
SELECT repair_key('isq','d');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM isq; END $$;

-- (1) ERROR: provenance() in a scalar subquery in the SELECT clause.
SELECT (SELECT provenance() FROM isq GROUP BY d HAVING sum(val) < 5) AS bad;
-- (1) ERROR: provenance() in an EXISTS subquery.
SELECT 1 AS bad WHERE EXISTS (SELECT provenance() FROM isq GROUP BY d);
-- (1) OK: reading the provsql column inside a SubLink is allowed (no rewrite).
SELECT (SELECT count(*) FROM isq WHERE provsql IS NOT NULL) AS reads_col;

-- (2) INSERT ... SELECT into a NON-tracked target: HAVING is lifted, the row
--     comes back; provenance is (warned) not propagated.
CREATE TABLE res(pred text, p numeric);
INSERT INTO res SELECT 'sum<5', probability_evaluate(provenance())
  FROM isq GROUP BY d HAVING sum(val) < 5;

-- (2) INSERT ... SELECT into a TRACKED target: provenance propagates.
CREATE TABLE tgt(x int);
SELECT add_provenance('tgt');
INSERT INTO tgt SELECT val FROM isq;

-- Read the results with the rewriter off so the auto-added provsql column does
-- not appear (and the stored values are read directly).
SET provsql.active = off;
SELECT pred, round(p,3) AS p FROM res;
SELECT x, round(probability_evaluate(provsql)::numeric,3) AS p FROM tgt ORDER BY x;
RESET provsql.active;

SELECT remove_provenance('isq');
SELECT remove_provenance('tgt');
DROP TABLE isq;
DROP TABLE res;
DROP TABLE tgt;
