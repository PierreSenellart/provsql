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

-- A body whose grouping IS read, by a HAVING of its own, and correlated by an
-- equality: the correlated column becomes a key of the grouping and a column
-- compared, which leaves the uncorrelated test above.  The group of (g, k) holds
-- the rows the group g holds for the outer row of that k, so the HAVING reads
-- the same rows.  Same answer as the uncorrelated form: 0.5 * 0.25 for k=1, 2.
CREATE TABLE bg_r AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM bgt WHERE k IN (SELECT bgu.k FROM bgu WHERE bgu.k = bgt.k
                       GROUP BY bgu.k HAVING count(*) > 1);
SELECT remove_provenance('bg_r');
SELECT * FROM bg_r ORDER BY k;
DROP TABLE bg_r;
-- EXISTS, grouped by another column than the correlated one.  k=1: the group
-- g=10 has v 2 and 5, a sum of 5 needs the second, 0.5 * 0.5; k=2: v 3 and 3
-- need both, 0.5 * 0.25; k=3: v 9 alone, 0.5 * 0.5.
CREATE TABLE bg_r AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM bgt WHERE EXISTS (SELECT 1 FROM bgu WHERE bgu.k = bgt.k
                         GROUP BY bgu.g HAVING sum(bgu.v) >= 5);
SELECT remove_provenance('bg_r');
SELECT * FROM bg_r ORDER BY k;
DROP TABLE bg_r;
-- Without a GROUP BY the body is one group even over no rows, where the keyed
-- body has none: the same only because count(*) >= 2 fails over no rows.
CREATE TABLE bg_r AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM bgt WHERE EXISTS (SELECT 1 FROM bgu WHERE bgu.k = bgt.k
                         HAVING count(*) >= 2);
SELECT remove_provenance('bg_r');
SELECT * FROM bg_r ORDER BY k;
DROP TABLE bg_r;
-- count(*) < 2 holds over no rows, so every outer row without a match answers:
-- the keyed body cannot say that, and the test stays refused.
SELECT k FROM bgt WHERE EXISTS (SELECT 1 FROM bgu WHERE bgu.k = bgt.k
                                HAVING count(*) < 2);
-- An EXISTS whose grouping nothing reads asks only for a row: the grouping
-- goes.  Each k needs its row and one of its bgu rows: 0.5 * 0.75 for k=1, 2,
-- 0.5 * 0.5 for k=3.
CREATE TABLE bg_r AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM bgt WHERE EXISTS (SELECT bgu.g, max(bgu.v) FROM bgu
                         WHERE bgu.k = bgt.k GROUP BY bgu.g);
SELECT remove_provenance('bg_r');
SELECT * FROM bg_r ORDER BY k;
DROP TABLE bg_r;
-- A scalar subquery grouped by the column its correlation fixes has at most one
-- group, the rows that match: the aggregate over them without the grouping,
-- NULL over no rows either way for a max.  k=1: x=2 < 5 needs the row of v 5,
-- 0.5 * 0.5; k=2: max 3 is never above 3; k=3: v 9, 0.5 * 0.5.
CREATE TABLE bg_r AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM bgt WHERE x < (SELECT max(bgu.v) FROM bgu WHERE bgu.k = bgt.k
                      GROUP BY bgu.k);
SELECT remove_provenance('bg_r');
SELECT * FROM bg_r ORDER BY k;
DROP TABLE bg_r;
-- A grouped body reading a subquery of its FROM, as it does a view: it moves
-- into the derived table with the rest.  Only k=2 has two rows above 2,
-- 0.5 * 0.25.
CREATE TABLE bg_r AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM bgt WHERE k IN (SELECT w.k FROM (SELECT * FROM bgu WHERE v > 2) w
                       GROUP BY w.k HAVING count(*) > 1);
SELECT remove_provenance('bg_r');
SELECT * FROM bg_r ORDER BY k;
DROP TABLE bg_r;

-- Compared against an aggregate result: refused, and the message still names
-- the body's grouping rather than the derived table the wrap would have made.
SELECT k FROM bgt WHERE x IN (SELECT max(v) FROM bgu GROUP BY g);
-- An uncorrelated EXISTS body whose groups a HAVING reads: refused as before
-- (its existence test has no column to read, and no correlation to key by).
SELECT k FROM bgt WHERE EXISTS (SELECT 1 FROM bgu GROUP BY g HAVING count(*) > 1);

SELECT remove_provenance('bgt');
SELECT remove_provenance('bgu');
DROP TABLE bgt;
DROP TABLE bgu;

-- ----------------------------------------------------------------------
-- A test NESTED inside a subquery body, whose own conjuncts read only the
-- levels above it.  Such a conjunct takes the same value for every row the
-- inner body scans, so it can be evaluated one level up:
--
--   x IN (SELECT a FROM Q WHERE t IN (SELECT b FROM R WHERE C))    C outer-only
--   x IN (SELECT a FROM Q WHERE C AND t IN (SELECT b FROM R))
--
-- What that buys is the inner test becoming UNCORRELATED, which the membership
-- rewriting lowers, leaving the outer body plain for the decorrelation.  Before
-- it the outer test was refused with body-nested-subquery: difftest's
-- sede/f42f1b3271, the last F1 query of its kind.  The lift is the mirror of
-- wrap_body_sublinks, which moves the conjuncts that read nothing OUTSIDE a body
-- into a derived table of it.
-- Three rows at one half.  Row 1 qualifies because row 2 carries the same tags
-- with an accepted answer, so it needs both: 0.25.  The emitted circuit is the
-- one the hand-lifted query builds, down to the same provenance uuid.
CREATE TABLE nsp(id int, tags text, kind int, answers int, accepted int);
INSERT INTO nsp VALUES (1,'<a>',1,0,NULL), (2,'<a>',1,2,7), (3,'<b>',1,0,NULL);
SELECT add_provenance('nsp');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM nsp; END $$;
CREATE TABLE nsp_r AS
  SELECT p.id, probability(provenance()) AS pr FROM nsp AS p
   WHERE p.tags IN (SELECT tags FROM nsp AS p2
                     WHERE id IN (SELECT id FROM nsp
                                   WHERE p2.kind = 1 AND p2.answers > 0
                                     AND p2.accepted IS NOT NULL
                                     AND p2.tags = p.tags))
     AND p.kind = 1 AND p.answers = 0;
SELECT remove_provenance('nsp_r');
SELECT id, round(pr::numeric, 6) AS pr FROM nsp_r ORDER BY id;
DROP TABLE nsp_r;
-- The same query with the conjuncts lifted by hand: the same answer, which is
-- what the rewriting has to reproduce.
CREATE TABLE nsp_r AS
  SELECT p.id, probability(provenance()) AS pr FROM nsp AS p
   WHERE p.tags IN (SELECT tags FROM nsp AS p2
                     WHERE p2.kind = 1 AND p2.answers > 0
                       AND p2.accepted IS NOT NULL AND p2.tags = p.tags
                       AND p2.id IN (SELECT id FROM nsp))
     AND p.kind = 1 AND p.answers = 0;
SELECT remove_provenance('nsp_r');
SELECT id, round(pr::numeric, 6) AS pr FROM nsp_r ORDER BY id;
DROP TABLE nsp_r;
-- And the row set plain SQL gives, which both must match.
SET provsql.active = off;
SELECT p.id FROM nsp AS p
 WHERE p.tags IN (SELECT tags FROM nsp AS p2
                   WHERE id IN (SELECT id FROM nsp
                                 WHERE p2.kind = 1 AND p2.answers > 0
                                   AND p2.accepted IS NOT NULL
                                   AND p2.tags = p.tags))
   AND p.kind = 1 AND p.answers = 0 ORDER BY 1;
SET provsql.active = on;
-- The lift DECLINES a negated inner test, on a NULL condition the body being
-- empty either way: the positive test is then false either way and a WHERE drops
-- the row, while under a NOT one reading is true and the other unknown.  It is
-- answered all the same, by the antijoin path, and correctly -- which is why the
-- decline costs nothing here.  Rows 1 and 3 have no answers, so their inner test
-- is over an empty set and they qualify; row 2 has answers, so the inner set
-- holds every present id and excludes it.  The body therefore offers '<a>' from
-- row 1 and '<b>' from row 3: row 1 needs itself (0.5), row 2 needs row 1 for
-- the tag and itself for the row (0.25), row 3 needs itself (0.5).
CREATE TABLE nsp_r AS
  SELECT p.id, probability(provenance()) AS pr FROM nsp AS p
   WHERE p.tags IN (SELECT tags FROM nsp AS p2
                     WHERE id NOT IN (SELECT id FROM nsp WHERE p2.answers > 0));
SELECT remove_provenance('nsp_r');
SELECT id, round(pr::numeric, 6) AS pr FROM nsp_r ORDER BY id;
DROP TABLE nsp_r;
SET provsql.active = off;
SELECT p.id FROM nsp AS p
 WHERE p.tags IN (SELECT tags FROM nsp AS p2
                   WHERE id NOT IN (SELECT id FROM nsp WHERE p2.answers > 0))
 ORDER BY 1;
SET provsql.active = on;
-- An inner body that AGGREGATES without grouping yields a row over no input at
-- all, so it answers over the count 0 where the condition is false; lifting the
-- condition out would make it answer nothing instead.
SELECT p.id FROM nsp AS p
 WHERE p.answers IN (SELECT count(*) FROM nsp AS p3 WHERE p3.id IN
                      (SELECT count(*) FROM nsp WHERE p.kind = 99));
-- An inner body with nothing to lift, its WHERE a conjunction of conditions on
-- its own relation: left as it is.  The list of that conjunction is the body's
-- own, and freeing it after finding nothing to move corrupted the query,
-- "stack depth limit exceeded" (difftest's sede/61fe6a7cda).  Only row 2 is
-- accepted, of kind 1: each row of kind 1 is there with its own row and row 2,
-- 0.25, and row 2 alone, 0.5.
CREATE TABLE nsp_r AS
  SELECT p.id, round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM nsp AS p
  WHERE p.kind IN (SELECT kind FROM nsp AS p2
                    WHERE p2.id IN (SELECT p3.id FROM nsp AS p3
                                     WHERE p3.kind = 1
                                       AND p3.accepted IS NOT NULL));
SELECT remove_provenance('nsp_r');
SELECT id, pr FROM nsp_r ORDER BY id;
DROP TABLE nsp_r;
SELECT remove_provenance('nsp');
DROP TABLE nsp;
