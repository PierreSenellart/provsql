\set ECHO none
\pset format unaligned

-- ----------------------------------------------------------------------
-- A subquery condition whose body is a SET OPERATION.  The body moves into a
-- derived table (wrap_body_setop), where a union, an intersection or a
-- difference is tracked as it is anywhere else, and what is left outside is the
-- membership test the decorrelation already lowers.  Relational algebra inside
-- a semijoin, which is why it sits at the bottom of the fragment chain.
--
-- Only an uncorrelated body: an arm reading the block above would have to carry
-- that correlation into the derived table.  A correlated body is split into
-- tests of its arms where the answer allows it (at the end of this file).  And
-- only a membership test: an existence test's decorrelation wants base relations in
-- the body's FROM, so wrapping it there would name the derived table instead of
-- the set operation and fix nothing.  An existence test over an EXCEPT is not
-- wrapped either, but it is not refused: that one says "every row of A is a row
-- of B", so it is read as the nested antijoin it is (rewrite_nested_antijoin).
-- ----------------------------------------------------------------------

CREATE TABLE sop(id int);
CREATE TABLE sot(postid int, tagid text);
INSERT INTO sop VALUES (1),(2),(3);
INSERT INTO sot VALUES (1,'a'),(1,'b'),(2,'a'),(3,'b'),(1,'c');
SELECT add_provenance('sop');
SELECT add_provenance('sot');
DO $$ BEGIN
  PERFORM set_prob(provenance(), 0.5) FROM sop;
  PERFORM set_prob(provenance(), 0.5) FROM sot;
END $$;

-- INTERSECT of two arms: post 1 is the only answer, and it needs its own row
-- and both tag rows, so 1/8 with every row at one half.
CREATE TABLE so_r AS
  SELECT id, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM sop WHERE id IN (SELECT postid FROM sot WHERE tagid = 'a'
                        INTERSECT
                        SELECT postid FROM sot WHERE tagid = 'b');
SELECT remove_provenance('so_r');
SELECT id, p FROM so_r ORDER BY id;
DROP TABLE so_r;

-- Three arms, which is the corpus shape: the third tag narrows it to post 1
-- again, and its answer now needs three tag rows, 1/16.
CREATE TABLE so_r AS
  SELECT id, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM sop WHERE id IN (SELECT postid FROM sot WHERE tagid = 'a'
                        INTERSECT
                        SELECT postid FROM sot WHERE tagid = 'b'
                        INTERSECT
                        SELECT postid FROM sot WHERE tagid = 'c');
SELECT remove_provenance('so_r');
SELECT id, p FROM so_r ORDER BY id;
DROP TABLE so_r;

-- UNION: every post with either tag, each answer needing its own row and the
-- tag row that puts it there -- post 1 has two such rows, so 1/2 * (1 - 1/4).
CREATE TABLE so_r AS
  SELECT id, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM sop WHERE id IN (SELECT postid FROM sot WHERE tagid = 'a'
                        UNION
                        SELECT postid FROM sot WHERE tagid = 'b');
SELECT remove_provenance('so_r');
SELECT id, p FROM so_r ORDER BY id;
DROP TABLE so_r;

-- EXCEPT: on the data as it is only post 2 answers, and post 1 is kept as a row
-- whose annotation is the world where its 'b' row is absent -- the difference's
-- monus, 1/2 * 1/2 * 1/2 -- which is the documented visible-but-zero row.
CREATE TABLE so_r AS
  SELECT id, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM sop WHERE id IN (SELECT postid FROM sot WHERE tagid = 'a'
                        EXCEPT
                        SELECT postid FROM sot WHERE tagid = 'b');
SELECT remove_provenance('so_r');
SELECT id, p FROM so_r ORDER BY id;
DROP TABLE so_r;

-- The same body under an existence test.  "A EXCEPT B is empty" is "every row
-- of A is a row of B", the division a nested antijoin is, so it is read as one
-- rather than wrapped: every 'a' row present needs its 'b' row present, and
-- (2, 'a') has none, so the condition is (¬a1 ∨ b1) ∧ ¬a2 = 3/4 · 1/2, and each
-- row of sop carries its own half of that.  No row on the data as it is.
CREATE TABLE so_r AS
  SELECT id, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM sop p WHERE NOT EXISTS (SELECT postid FROM sot WHERE tagid = 'a'
                               EXCEPT
                               SELECT postid FROM sot WHERE tagid = 'b');
SELECT remove_provenance('so_r');
SELECT id, p FROM so_r ORDER BY id;
DROP TABLE so_r;

-- A correlated body is split into tests of its arms.  "x IN (A INTERSECT B)"
-- is "x IN A AND x IN B": post 1 needs its row and its 'a' row (any of its tag
-- rows then puts it in B), 1/4, and so does post 2.
CREATE TABLE so_r AS
  SELECT id, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM sop p WHERE id IN (SELECT postid FROM sot WHERE tagid = 'a'
                          INTERSECT
                          SELECT postid FROM sot WHERE postid = p.id);
SELECT remove_provenance('so_r');
SELECT id, p FROM so_r ORDER BY id;
DROP TABLE so_r;

-- "NOT EXISTS (A UNION B)" is "NOT EXISTS A AND NOT EXISTS B": a post with
-- neither an 'a' nor a 'c' tag row, 1/2 · 1/2 · 1/2 for post 1, which has both,
-- 1/4 for post 2 and 1/2 for post 3, which has neither.
CREATE TABLE so_r AS
  SELECT id, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM sop p WHERE NOT EXISTS (SELECT tagid FROM sot
                                WHERE postid = p.id AND tagid = 'a'
                               UNION
                               SELECT tagid FROM sot
                                WHERE postid = p.id AND tagid = 'c');
SELECT remove_provenance('so_r');
SELECT id, p FROM so_r ORDER BY id;
DROP TABLE so_r;

-- "x IN (A EXCEPT B)" is "x IN A AND NOT EXISTS (B matching x)": a post whose
-- 'a' tag the next post does not have.  Post 1 needs (1, 'a') and not (2, 'a'),
-- 1/8; post 2 needs (2, 'a'), post 3 having no tag row 'a', 1/4.
CREATE TABLE so_r AS
  SELECT id, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM sop p WHERE 'a' IN (SELECT tagid FROM sot WHERE postid = p.id
                           EXCEPT
                           SELECT tagid FROM sot WHERE postid = p.id + 1);
SELECT remove_provenance('so_r');
SELECT id, p FROM so_r ORDER BY id;
DROP TABLE so_r;

-- An existence test of a correlated union is a disjunction of two tests, which
-- the decorrelation does not read: refused, naming the set operation.
SELECT id FROM sop p WHERE EXISTS (SELECT tagid FROM sot
                                    WHERE postid = p.id AND tagid = 'a'
                                   UNION
                                   SELECT tagid FROM sot
                                    WHERE postid = p.id AND tagid = 'c');

SELECT remove_provenance('sop');
SELECT remove_provenance('sot');
DROP TABLE sop;
DROP TABLE sot;
