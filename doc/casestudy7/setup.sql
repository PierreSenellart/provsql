-- Case Study 7: Peer-Review Assignment (knowledge compilation)
-- Setup script -- load into a fresh PostgreSQL database:
--   createdb peer_review_demo
--   psql -d peer_review_demo -f setup.sql
--
-- A small conference-reviewing scenario whose probabilistic queries
-- exercise the knowledge-compilation surface: a SAFE (hierarchical)
-- query versus its UNSAFE (non-hierarchical, H0-shaped) sibling over the
-- SAME data, a HAVING COUNT(*) query that triggers the probability-side
-- pre-pass, and a repair_key table for the correlated case. No recursion
-- anywhere: the hardness comes from the join pattern, not a fixpoint.

SET client_encoding = 'UTF8';

CREATE EXTENSION IF NOT EXISTS "uuid-ossp" WITH SCHEMA public;
CREATE EXTENSION IF NOT EXISTS provsql WITH SCHEMA public;

SET search_path TO public, provsql;

DROP TABLE IF EXISTS bid CASCADE;
DROP TABLE IF EXISTS expertise CASCADE;
DROP TABLE IF EXISTS topic_of CASCADE;
DROP TABLE IF EXISTS assignment CASCADE;
DROP TABLE IF EXISTS reviewers CASCADE;
DROP TABLE IF EXISTS papers CASCADE;
DROP TABLE IF EXISTS topics CASCADE;
DROP TABLE IF EXISTS bid_label, expertise_label, topic_of_label;

-- Dimension tables (deterministic, no provenance).
CREATE TABLE reviewers (id TEXT PRIMARY KEY, name TEXT NOT NULL);
INSERT INTO reviewers VALUES
  ('r1', 'Alice'), ('r2', 'Bob'), ('r3', 'Carol'), ('r4', 'Dave');

CREATE TABLE papers (id TEXT PRIMARY KEY, title TEXT NOT NULL);
INSERT INTO papers VALUES
  ('p1', 'A Provenance Circuit Calculus'),
  ('p2', 'Treewidth Bounds for Safe Queries'),
  ('p3', 'Sampling Beats Compilation, Sometimes');

CREATE TABLE topics (id TEXT PRIMARY KEY, name TEXT NOT NULL);
INSERT INTO topics VALUES
  ('t1', 'databases'), ('t2', 'logic'), ('t3', 'systems');

-- bid(reviewer, paper): the reviewer offered to review the paper; the
-- confidence is how firm the bid is (availability, willingness). r1/r2/r3
-- all bid on p1, which is what makes p1's coverage interesting.
CREATE TABLE bid (
  reviewer TEXT NOT NULL REFERENCES reviewers(id),
  paper    TEXT NOT NULL REFERENCES papers(id),
  conf     DOUBLE PRECISION NOT NULL,
  lbl      TEXT,
  PRIMARY KEY (reviewer, paper)   -- a reviewer bids on a paper once
);
INSERT INTO bid(reviewer, paper, conf) VALUES
  ('r1', 'p1', 0.9), ('r2', 'p1', 0.8), ('r3', 'p1', 0.7),
  ('r1', 'p2', 0.6), ('r4', 'p2', 0.5),
  ('r2', 'p3', 0.7);
UPDATE bid SET lbl = format('bid(%s,%s)', reviewer, paper);
SELECT add_provenance('bid');
SELECT set_prob(provenance(), conf) FROM bid;

-- expertise(reviewer, topic): the reviewer's area of competence (one
-- per reviewer here, so the safe query's lineage stays read-once). r1
-- and r2 are BOTH expert in t1 -- the shared topic that, together with
-- p1 being about t1, makes p1's UNSAFE coverage non-read-once.
CREATE TABLE expertise (
  reviewer TEXT NOT NULL REFERENCES reviewers(id),
  topic    TEXT NOT NULL REFERENCES topics(id),
  conf     DOUBLE PRECISION NOT NULL,
  lbl      TEXT,
  -- KEY ON reviewer (not (reviewer, topic)): one area per reviewer. This
  -- functional dependency is exactly what makes the bid-expertise query
  -- safe (read-once lineage); widen it to (reviewer, topic) and that
  -- query's lineage stops being read-once.
  PRIMARY KEY (reviewer)
);
INSERT INTO expertise(reviewer, topic, conf) VALUES
  ('r1', 't1', 0.9), ('r2', 't1', 0.8), ('r3', 't2', 0.85), ('r4', 't1', 0.6);
UPDATE expertise SET lbl = format('exp(%s,%s)', reviewer, topic);
SELECT add_provenance('expertise');
SELECT set_prob(provenance(), conf) FROM expertise;

-- topic_of(paper, topic): the paper is about the topic. p1 spans t1 and
-- t2; topic_of(p1,t1) is shared by the r1- and r2-via-t1 disjuncts.
CREATE TABLE topic_of (
  paper TEXT NOT NULL REFERENCES papers(id),
  topic TEXT NOT NULL REFERENCES topics(id),
  conf  DOUBLE PRECISION NOT NULL,
  lbl   TEXT,
  PRIMARY KEY (paper, topic)      -- a paper lists a topic once
);
INSERT INTO topic_of(paper, topic, conf) VALUES
  ('p1', 't1', 0.95), ('p1', 't2', 0.9),
  ('p2', 't1', 0.8),  ('p2', 't3', 0.7),
  ('p3', 't2', 0.6);
UPDATE topic_of SET lbl = format('topof(%s,%s)', paper, topic);
SELECT add_provenance('topic_of');
SELECT set_prob(provenance(), conf) FROM topic_of;

-- assignment(reviewer, paper): the candidate papers a reviewer could be
-- assigned to. repair_key on `reviewer` makes the rows for one reviewer
-- mutually exclusive (each reviewer ends up on exactly one paper), so a
-- query over this table carries correlated provenance.
CREATE TABLE assignment (
  reviewer TEXT NOT NULL REFERENCES reviewers(id),
  paper    TEXT NOT NULL REFERENCES papers(id)
);
INSERT INTO assignment(reviewer, paper) VALUES
  ('r1', 'p1'), ('r1', 'p2'),
  ('r2', 'p1'), ('r2', 'p3'),
  ('r3', 'p1');
SELECT repair_key('assignment', 'reviewer');

-- Label mappings so the Studio eval-strip's sr_formula / sr_why / sr_how
-- and PROV-XML export name the leaves instead of showing raw UUIDs. The
-- `lbl` columns exist only to feed create_provenance_mapping, which copies
-- their values into standalone (value, provenance) tables.
SELECT create_provenance_mapping('bid_label',      'bid',      'lbl');
SELECT create_provenance_mapping('expertise_label','expertise','lbl');
SELECT create_provenance_mapping('topic_of_label', 'topic_of', 'lbl');

-- `conf` only fed set_prob and `lbl` only fed create_provenance_mapping;
-- both jobs are now done, so drop the artifact columns from the base tables.
ALTER TABLE bid       DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE expertise DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE topic_of  DROP COLUMN conf, DROP COLUMN lbl;

\echo
\echo '----------------------------------------------------------------------'
\echo 'peer_review_demo loaded.  Point Studio at it and try the queries in'
\echo 'the case-study chapter.  The headline pair (same data, different shape):'
\echo
\echo '-- SAFE (hierarchical): a reviewer who bid on the paper and has some'
\echo '--   expertise.  Read-once lineage, tiny d-DNNF, treewidth 1.'
\echo 'SELECT b.paper'
\echo '  FROM bid b, expertise e'
\echo ' WHERE b.reviewer = e.reviewer'
\echo ' GROUP BY b.paper;'
\echo
\echo '-- UNSAFE (H0 / non-hierarchical): the paper is covered by a reviewer'
\echo '--   who bid on it AND is expert in one of its topics.  Shared tuples,'
\echo '--   higher treewidth, larger d-DNNF.'
\echo 'SELECT b.paper'
\echo '  FROM bid b, expertise e, topic_of t'
\echo ' WHERE b.reviewer = e.reviewer'
\echo '   AND e.topic    = t.topic'
\echo '   AND t.paper    = b.paper'
\echo ' GROUP BY b.paper;'
\echo
\echo '-- HAVING COUNT(*): papers with at least two bidding experts.  The'
\echo '--   probability-side pre-pass folds the count comparison.'
\echo 'SELECT b.paper'
\echo '  FROM bid b, expertise e'
\echo ' WHERE b.reviewer = e.reviewer'
\echo ' GROUP BY b.paper'
\echo ' HAVING count(*) >= 2;'
\echo '----------------------------------------------------------------------'
\echo
