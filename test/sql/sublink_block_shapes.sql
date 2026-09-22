\set ECHO none
\pset format unaligned

-- Two shapes of BLOCK that a subquery condition used to be declined beside.
-- Neither is about the body: what the decorrelation wanted was of the block --
-- a provenance to read on its own rows, and base relations to group them by.
SET search_path TO sublink_block_test,provsql;
CREATE SCHEMA sublink_block_test;

-- 1. A block with no tracked relation of its own.  The answer has a provenance
-- all the same -- each row is there unless a matching image is -- and the only
-- thing missing was something to carry it.  Every row of a VALUES list is there
-- in every world, so a certain provenance column says exactly that.
CREATE TABLE ubi(id int);
INSERT INTO ubi VALUES (4);
SELECT add_provenance('ubi');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ubi; END $$;
-- Plain SQL keeps 5 and 6: only 4 has its image.
SET provsql.active = off;
SELECT v.id FROM (VALUES (4),(5),(6)) v(id)
 WHERE NOT EXISTS (SELECT * FROM ubi i WHERE i.id = v.id) ORDER BY v.id;
SET provsql.active = on;
-- 4 is there exactly when its image is not, one half; 5 and 6 have no image to
-- lose, so they are certain -- and carry the same token, being the same one.
CREATE TABLE ub_r AS
  SELECT v.id, probability(provenance()) AS p FROM (VALUES (4),(5),(6)) v(id)
   WHERE NOT EXISTS (SELECT * FROM ubi i WHERE i.id = v.id);
SELECT remove_provenance('ub_r');
SELECT id, round(p::numeric, 6) AS p FROM ub_r ORDER BY id;
DROP TABLE ub_r;

-- 2. A membership test beside something in the FROM that is not a base
-- relation.  The test is UNCORRELATED, so it needs no grouping of the block at
-- all: read as a join against the deduplicated body, nothing about the rest of
-- the FROM matters.  The count-predicate route would have made the body
-- correlated and then wanted base relations to group by.
CREATE TABLE ubp(id int, parentid int);
INSERT INTO ubp VALUES (1, 10), (2, 10), (3, 11);
CREATE TABLE ubv(postid int);
INSERT INTO ubv VALUES (10), (10), (11);
CREATE TABLE ubu(lim int);   -- untracked on purpose
INSERT INTO ubu VALUES (99);
SELECT add_provenance('ubp');
SELECT add_provenance('ubv');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ubp; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ubv; END $$;
SET provsql.active = off;
SELECT p.id FROM ubp p, (SELECT max(lim) AS d FROM ubu) r
 WHERE p.parentid IN (SELECT postid FROM ubv WHERE postid > 0)
   AND r.d > 0 ORDER BY p.id;
SET provsql.active = on;
-- Each post is there with its own half, times one of its parent's vote rows
-- being there: parent 10 has two, 1 - 1/4, and parent 11 has one, 1/2.
CREATE TABLE ub_r AS
  SELECT p.id, probability(provenance()) AS p FROM ubp p,
         (SELECT max(lim) AS d FROM ubu) r
   WHERE p.parentid IN (SELECT postid FROM ubv WHERE postid > 0) AND r.d > 0;
SELECT remove_provenance('ub_r');
SELECT id, round(p::numeric, 6) AS p FROM ub_r ORDER BY id;
DROP TABLE ub_r;
-- The same test without the subquery in the FROM keeps the lowering it had.
CREATE TABLE ub_r AS
  SELECT p.id, probability(provenance()) AS p FROM ubp p
   WHERE p.parentid IN (SELECT postid FROM ubv WHERE postid > 0);
SELECT remove_provenance('ub_r');
SELECT id, round(p::numeric, 6) AS p FROM ub_r ORDER BY id;
DROP TABLE ub_r;

DROP TABLE ubi, ubp, ubv, ubu;
DROP SCHEMA sublink_block_test CASCADE;
