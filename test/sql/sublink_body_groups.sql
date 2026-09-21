\set ECHO none
\pset format unaligned

-- ----------------------------------------------------------------------
-- A subquery condition whose body groups rows of its own.  The body moves into
-- a derived table (wrap_body_grouping), where grouping is tracked as it is in
-- any FROM subquery -- one row per group, annotated by the group -- and what is
-- left outside is the existence test the decorrelation already lowers.
--
-- Sound only where the compared column is a grouping KEY.  Against an
-- aggregate result the comparison reads a value that is one per world, which
-- the semijoin's correlation does not, so those stay refused.
-- ----------------------------------------------------------------------

CREATE TABLE bgt(k int, x int);
CREATE TABLE bgu(k int, g int, v int);
INSERT INTO bgt VALUES (1,2),(2,3),(3,1);
INSERT INTO bgu VALUES (1,10,2),(1,10,5),(2,20,3),(2,20,3),(3,30,9);
SELECT add_provenance('bgt');
SELECT add_provenance('bgu');
DO $$ BEGIN
  PERFORM set_prob(provenance(), 0.5) FROM bgt;
  PERFORM set_prob(provenance(), 0.5) FROM bgu;
END $$;

-- IN over a grouped key list, with a HAVING of its own.  k is an answer where
-- its own row is present and two rows of bgu share its k, so 0.5 * 0.25 with
-- every row at 1/2; k=3 has only one bgu row and is no answer in any world.
CREATE TABLE bg_in AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM bgt WHERE k IN (SELECT bgu.k FROM bgu GROUP BY bgu.k HAVING count(*) > 1);
SELECT remove_provenance('bg_in');
SELECT * FROM bg_in ORDER BY k;
DROP TABLE bg_in;

-- The rows are SQL's: plain SQL answers 1 and 2 as well.
SET provsql.active = off;
SELECT k FROM bgt WHERE k IN (SELECT bgu.k FROM bgu GROUP BY bgu.k HAVING count(*) > 1)
ORDER BY k;
RESET provsql.active;

-- NOT IN keeps the antijoin's own reading (every row that is an answer in some
-- world, annotated with the monus), as it does over a body that groups nothing.
CREATE TABLE bg_notin AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM bgt WHERE k NOT IN (SELECT bgu.k FROM bgu GROUP BY bgu.k HAVING count(*) > 1);
SELECT remove_provenance('bg_notin');
SELECT * FROM bg_notin ORDER BY k;
DROP TABLE bg_notin;

-- A grouping that reads none of its groups is deduplication and nothing else,
-- so it is dropped and the condition lowered as it is without it, correlated or
-- not.  Both forms must give the same rows and the same probabilities: what the
-- grouping removes are duplicates, which a membership test does not read.
CREATE TABLE bg_dedup AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM bgt WHERE k IN (SELECT bgu.k FROM bgu WHERE bgu.v > 1 GROUP BY bgu.k);
SELECT remove_provenance('bg_dedup');
SELECT * FROM bg_dedup ORDER BY k;
DROP TABLE bg_dedup;
CREATE TABLE bg_nodedup AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM bgt WHERE k IN (SELECT bgu.k FROM bgu WHERE bgu.v > 1);
SELECT remove_provenance('bg_nodedup');
SELECT * FROM bg_nodedup ORDER BY k;
DROP TABLE bg_nodedup;

-- Correlated, and over a comparison of two columns at once: the groups of such
-- a body differ from one outer row to the next, which is why a grouping read by
-- a HAVING or an aggregate stays refused below; a grouping that only
-- deduplicates does not care.
CREATE TABLE bg_corr AS
  SELECT bgt.k, bgu2.g,
         round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM bgt, bgu AS bgu2
  WHERE (bgt.k, bgu2.g) IN (SELECT bgu.k, bgu.g FROM bgu
                            WHERE bgu.k = bgt.k GROUP BY bgu.k, bgu.g);
SELECT remove_provenance('bg_corr');
SELECT * FROM bg_corr ORDER BY k, g, p;
DROP TABLE bg_corr;

-- A body whose grouping IS read, by a HAVING of its own, and correlated: the
-- groups differ per outer row, so it stays refused, and the refusal names the
-- correlation (body-groups-correlated) rather than the grouping alone, that
-- being what an uncorrelated body of the same shape is carried in spite of.
SELECT k FROM bgt WHERE k IN (SELECT bgu.k FROM bgu WHERE bgu.k = bgt.k
                              GROUP BY bgu.k HAVING count(*) > 1);

-- Compared against an aggregate result: refused, and the message still names
-- the body's grouping rather than the derived table the wrap would have made.
SELECT k FROM bgt WHERE x IN (SELECT max(v) FROM bgu GROUP BY g);
-- An EXISTS body that groups: refused as before (its existence test has no
-- column to read, and the uncorrelated arm no key to count).
SELECT k FROM bgt WHERE EXISTS (SELECT 1 FROM bgu GROUP BY g HAVING count(*) > 1);

SELECT remove_provenance('bgt');
SELECT remove_provenance('bgu');
DROP TABLE bgt;
DROP TABLE bgu;
