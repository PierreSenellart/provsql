\set ECHO none
\pset format unaligned

-- A block with no tracked relation of its own reads tracked relations only
-- through its sublinks: a FROM-less SELECT whose condition is an EXISTS, a
-- constant left side filtered by a NOT EXISTS.  The semantics has such a
-- query -- its condition is a semijoin or an antijoin over the bodies, whose
-- provenance is the answer's -- but the rewriting hangs the provenance column
-- on a range-table entry, and there is none to hang it on: the bodies are
-- therefore lifted into a FROM of the block first, and the tracked path taken
-- only if that leaves no sublink behind.

CREATE TABLE snr_t(val int);
INSERT INTO snr_t VALUES (1), (2);
SELECT add_provenance('snr_t');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM snr_t; END $$;
SELECT create_provenance_mapping('snr_m', 'snr_t', 'val');
CREATE TABLE snr_u(id int);
INSERT INTO snr_u VALUES (4), (7);
SELECT add_provenance('snr_u');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM snr_u; END $$;
CREATE TABLE snr_plain(id int);
INSERT INTO snr_plain VALUES (4), (5), (6);

-- Two uncorrelated conditions over the same relation: the row is there in the
-- worlds holding the row of 1 and not the row of 2, so 1 (x) (one (-) 2) and a
-- probability of 0.25.  The row is kept although the condition is false on the
-- data as it is, as every lifted condition is.
CREATE TABLE snr_r AS
  SELECT 'yes' AS found
  WHERE EXISTS (SELECT * FROM snr_t WHERE val = 1)
    AND NOT EXISTS (SELECT * FROM snr_t WHERE val <> 1);
SET provsql.active = off;
SELECT found, round(probability(provsql)::numeric, 6) AS p,
       sr_formula(provsql, 'snr_m') AS formula
FROM snr_r;
SET provsql.active = on;
DROP TABLE snr_r;

-- A correlated NOT EXISTS whose left side is an untracked relation: each of
-- its rows survives in the worlds where no row matches it, so 4 keeps the
-- worlds without the row of 4 (0.5) and 5 and 6 are certain.
CREATE TABLE snr_r AS
  SELECT p.id FROM snr_plain p
  WHERE NOT EXISTS (SELECT * FROM snr_u u WHERE u.id = p.id);
SET provsql.active = off;
SELECT id, round(probability(provsql)::numeric, 6) AS p FROM snr_r ORDER BY id;
SET provsql.active = on;
DROP TABLE snr_r;

-- Uncorrelated scalar subqueries over different relations are lifted too, one
-- FROM entry each, and their counts are aggregates of those relations.
SELECT (SELECT count(*) FROM snr_t) AS a, (SELECT count(*) FROM snr_u) AS b;

-- Declined, and evaluated by plain SQL with the warning, as before: a body
-- that READS the provenance column, which is a fetch of tokens and not of
-- data; and a constant left side, whose rows the decorrelation does not group
-- by (the lift leaves the sublink behind, so the block stays as it was rather
-- than reaching a refusal).
SELECT (SELECT count(*) FROM snr_t WHERE provsql IS NOT NULL) AS reads_token;
SELECT * FROM (VALUES (4),(5),(6)) AS v(id)
WHERE NOT EXISTS (SELECT * FROM snr_u u WHERE u.id = v.id) ORDER BY 1;

-- A WITH read only inside such a sublink.  has_provenance is true of the block
-- (it walks the WITH), so the lift used to be skipped, and the block answered
-- with NO provenance column and no warning at all -- the one shape of this
-- family that was silent.  The body is a one-row WITH entry, a scalar
-- aggregation, so it moves into the FROM as it stands: no choose(), no HAVING
-- gating the worlds with more than one row, since it has exactly one in every
-- world.  It must give what the same query written with the body in the FROM
-- gives, value and annotation alike.
-- Only a sublink that IS a target entry moves: one nested inside an expression
-- would have its agg_token coerced to a scalar on the way, so it stays where it
-- is (move_uncorrelated_sublinks_to_from).
CREATE TABLE snr_cte AS
  WITH c AS (SELECT sum(val) AS n FROM snr_t)
  SELECT (SELECT n FROM c) AS n,
         round(probability_evaluate(provenance())::numeric, 6) AS pr;
SELECT remove_provenance('snr_cte');
SELECT n::text AS n, pr FROM snr_cte;
DROP TABLE snr_cte;
CREATE TABLE snr_cte AS
  WITH c AS (SELECT sum(val) AS n FROM snr_t)
  SELECT s.n, round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM (SELECT n FROM c) s;
SELECT remove_provenance('snr_cte');
SELECT n::text AS n, pr FROM snr_cte;
DROP TABLE snr_cte;

-- The WITH entry needs not be the one-row one: what the route needs is a body
-- that is one row in every world, so a body which AGGREGATES a many-row entry
-- qualifies as well (difftest dba/137185, two such bodies in one block).  Over
-- snr_t = {1, 2} at one half each, and its duplicate-free copy, the count of
-- the rows below 2 is 1 in the worlds holding the first row, so its expectation
-- is one half, and the count of all rows has expectation 1.
CREATE TABLE snr_cte AS
  WITH d AS (SELECT DISTINCT val FROM snr_t)
  SELECT (SELECT count(*) FROM d WHERE val < 2) AS below,
         (SELECT count(*) FROM d) AS total;
SET provsql.active = off;
SELECT below::text AS below, round(expected(below, provsql)::numeric, 6) AS e_below,
       total::text AS total, round(expected(total, provsql)::numeric, 6) AS e_total
FROM snr_cte;
SET provsql.active = on;
DROP TABLE snr_cte;

-- A WITH that reads no tracked relation stays plain SQL, with no provenance
-- column: there is nothing to track.
WITH u AS (SELECT 1 AS n) SELECT (SELECT n FROM u) AS n;

DROP TABLE snr_t, snr_u, snr_plain;
DROP TABLE snr_m;
