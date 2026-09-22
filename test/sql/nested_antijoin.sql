\set ECHO none
\pset format unaligned

-- A nested antijoin: "every P of this R has a matching C", which is relational
-- division.  It is read as the antijoin of R against the projection of its bad
-- pairs -- R ⋉̄ π_R((R ⋈ P) ⋉̄ C) -- so nothing here aggregates, and the
-- decorrelation sees only shapes it already covers: an ordinary single-level
-- antijoin inside the pair block, and an uncorrelated derived table outside.
SET search_path TO nested_antijoin_test,provsql;
CREATE SCHEMA nested_antijoin_test;

CREATE TABLE nau(id int);
INSERT INTO nau VALUES (100), (101), (102);
CREATE TABLE nap(id int, owner int);
INSERT INTO nap VALUES (1, 100), (2, 101), (3, 100);
CREATE TABLE nac(postid int, userid int);
INSERT INTO nac VALUES (1, 100), (2, 101);
SELECT add_provenance('nau');
SELECT add_provenance('nap');
SELECT add_provenance('nac');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM nau; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM nap; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM nac; END $$;

-- The rows plain SQL returns: 101, whose only post has its comment, and 102,
-- which has no post at all -- every one of its zero posts has a comment.  100
-- is out, its post 3 having none.
SET provsql.active = off;
SELECT u.id FROM nau u
 WHERE NOT EXISTS (SELECT * FROM nap p WHERE p.owner = u.id
     AND NOT EXISTS (SELECT * FROM nac c WHERE c.postid = p.id AND c.userid = u.id))
 ORDER BY u.id;
SET provsql.active = on;

-- The provenance, by hand at one half each.  100: its posts are 1 and 3, post 1
-- spoiling the answer when present without its comment and post 3 whenever
-- present at all, so u ∧ (¬p1 ∨ c1) ∧ ¬p3 = 1/2 · 3/4 · 1/2.  101: u ∧ ¬(p2 ∧
-- ¬c2) = 1/2 − 1/8.  102: no bad pair carries it, so its own 1/2 -- which needs
-- no case of its own in the lowering.
CREATE TABLE na_r AS SELECT u.id, probability(provenance()) AS p FROM nau u
 WHERE NOT EXISTS (SELECT * FROM nap p WHERE p.owner = u.id
     AND NOT EXISTS (SELECT * FROM nac c WHERE c.postid = p.id AND c.userid = u.id));
SELECT remove_provenance('na_r');
SELECT id, round(p::numeric, 6) AS p FROM na_r ORDER BY id;
DROP TABLE na_r;

-- Two P rows that agree on what the outer correlation reads but differ in what
-- the inner one does: the answer has to keep them apart, and a lowering that
-- counted distinct VALUES of P rather than its rows would conflate them.
CREATE TABLE nbu(id int);
INSERT INTO nbu VALUES (1);
CREATE TABLE nbp(id int, tag text, owner int);
INSERT INTO nbp VALUES (7, 'a', 1), (7, 'b', 1);
CREATE TABLE nbc(postid int, tag text);
INSERT INTO nbc VALUES (7, 'a');
SELECT add_provenance('nbu');
SELECT add_provenance('nbp');
SELECT add_provenance('nbc');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM nbu; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM nbp; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM nbc; END $$;
-- No row on this data: (7, 'b') has no comment.  u ∧ (¬pa ∨ ca) ∧ ¬pb.
SET provsql.active = off;
SELECT u.id FROM nbu u
 WHERE NOT EXISTS (SELECT * FROM nbp p WHERE p.owner = u.id
     AND NOT EXISTS (SELECT * FROM nbc c WHERE c.postid = p.id AND c.tag = p.tag));
SET provsql.active = on;
CREATE TABLE na_r AS SELECT u.id, probability(provenance()) AS p FROM nbu u
 WHERE NOT EXISTS (SELECT * FROM nbp p WHERE p.owner = u.id
     AND NOT EXISTS (SELECT * FROM nbc c WHERE c.postid = p.id AND c.tag = p.tag));
SELECT remove_provenance('na_r');
SELECT id, round(p::numeric, 6) AS p FROM na_r ORDER BY id;
DROP TABLE na_r;

DROP TABLE nau, nap, nac, nbu, nbp, nbc;
DROP SCHEMA nested_antijoin_test CASCADE;
