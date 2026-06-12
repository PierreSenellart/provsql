\set ECHO none
\pset format unaligned

-- Whole-tuple output conditioning: a given(c) term (equivalently the prefix
-- | c operator) in the select list is a consumed marker -- the rewriter
-- STRIPS it from the visible projection and conditions each output row's
-- provenance on c, deriving a new conditioned relation (no stored provenance
-- is mutated).  c is per-row and may correlate with the row's columns.

CREATE TABLE pa(k int, p float);
INSERT INTO pa VALUES (1,0.4),(2,0.6);
SELECT add_provenance('pa');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM pa; END $$;

CREATE TABLE pb(k int, p float);
INSERT INTO pb VALUES (1,0.5),(2,0.25);
SELECT add_provenance('pb');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM pb; END $$;

-- given() conditions each row on its own pb neighbour.  pa and pb are
-- independent inputs, so P(a|b) = P(a); the given(...) term is stripped, so
-- the only visible column is k (plus the auto-added provsql).
CREATE TABLE g1 AS
  SELECT a.k, given((SELECT provenance() FROM pb b WHERE b.k = a.k))
  FROM pa a;
SELECT string_agg(attname, ',' ORDER BY attnum) AS visible_columns
  FROM pg_attribute
  WHERE attrelid = 'g1'::regclass AND attnum > 0 AND NOT attisdropped;
SET provsql.active = off;
SELECT k, get_gate_type(provsql) AS gate,
       round(probability_evaluate(provsql)::numeric,4) AS p_cond
FROM g1 ORDER BY k;
RESET provsql.active;
DROP TABLE g1;

-- Prefix | operator form, with correlated (self) evidence: conditioning each
-- row on itself shares the input gate, so the joint is the row itself and
-- P(a|a) = 1 -- correlation-aware.
CREATE TABLE g2 AS
  SELECT a.k, | (SELECT provenance() FROM pa b WHERE b.k = a.k)
  FROM pa a;
SET provsql.active = off;
SELECT k, round(probability_evaluate(provsql)::numeric,4) AS p_self
FROM g2 ORDER BY k;
RESET provsql.active;
DROP TABLE g2;

-- A given() marker is meaningful only for a per-row projection: an
-- aggregated / grouped / DISTINCT / set-operation query is rejected.
SELECT count(*), given((SELECT provenance() FROM pb LIMIT 1)) FROM pa;

SELECT remove_provenance('pa');
SELECT remove_provenance('pb');
DROP TABLE pa;
DROP TABLE pb;
