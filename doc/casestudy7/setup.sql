-- Case Study 7: Peer-Review Assignment (knowledge compilation)
-- Setup script -- load into a fresh PostgreSQL database:
--   createdb peer_review_demo
--   psql -d peer_review_demo -f setup.sql
--
-- A conference-reviewing scenario whose probabilistic queries exercise
-- the knowledge-compilation surface:
--   * a SAFE-by-shape (hierarchical) query;
--   * a coverage query that is genuinely #P-hard when asked over the
--     whole program (the free paper variable keeps it non-hierarchical),
--     but becomes read-once for a *single* paper precisely because
--     `expertise` is keyed on `reviewer` (the FD reviewer -> topic);
--   * a HAVING COUNT(*) query that triggers the probability-side pre-pass;
--   * a repair_key (block-correlated) table for the correlated case.
-- No recursion anywhere: the hardness comes from the join pattern and
-- the schema's keys, not a fixpoint.

SET client_encoding = 'UTF8';

CREATE EXTENSION IF NOT EXISTS "uuid-ossp" WITH SCHEMA public;
CREATE EXTENSION IF NOT EXISTS provsql WITH SCHEMA public;

SET search_path TO public, provsql;

DROP TABLE IF EXISTS bid CASCADE;
DROP TABLE IF EXISTS expertise CASCADE;
DROP TABLE IF EXISTS topic_of CASCADE;
DROP TABLE IF EXISTS extends CASCADE;
DROP TABLE IF EXISTS coreview CASCADE;
DROP TABLE IF EXISTS assignment CASCADE;
DROP TABLE IF EXISTS reviewers CASCADE;
DROP TABLE IF EXISTS papers CASCADE;
DROP TABLE IF EXISTS topics CASCADE;
DROP TABLE IF EXISTS bid_label, expertise_label, topic_of_label,
                     extends_label, coreview_label;

-- Dimension tables (deterministic, no provenance).
CREATE TABLE reviewers (id TEXT PRIMARY KEY, name TEXT NOT NULL);
INSERT INTO reviewers VALUES
  ('r1','Alice'), ('r2','Bob'),   ('r3','Carol'), ('r4','Dave'),
  ('r5','Eve'),   ('r6','Frank'), ('r7','Grace'), ('r8','Heidi'),
  ('r9','Ivan'),  ('r10','Judy'), ('r11','Karl'), ('r12','Lara'),
  ('r13','Mona'), ('r14','Nick');

CREATE TABLE papers (id TEXT PRIMARY KEY, title TEXT NOT NULL);
INSERT INTO papers VALUES
  ('p1', 'A Provenance Circuit Calculus'),
  ('p2', 'Treewidth Bounds for Safe Queries'),
  ('p3', 'Sampling Beats Compilation, Sometimes'),
  ('p4', 'Functional Dependencies and Read-Once Lineage'),
  ('p5', 'A Dichotomy for Conjunctive Coverage'),
  ('p6', 'Knowledge Compilation in Practice'),
  ('p7', 'Repair-Key Semantics for Assignments');

CREATE TABLE topics (id TEXT PRIMARY KEY, name TEXT NOT NULL);
INSERT INTO topics VALUES
  ('t1','databases'), ('t2','logic'), ('t3','systems'), ('t4','theory');

-- bid(reviewer, paper): the reviewer offered to review the paper; conf
-- is how firm the bid is.  Several databases-experts bid on p1, which is
-- what makes p1's coverage interesting.
CREATE TABLE bid (
  reviewer TEXT NOT NULL REFERENCES reviewers(id),
  paper    TEXT NOT NULL REFERENCES papers(id),
  conf     DOUBLE PRECISION NOT NULL,
  lbl      TEXT,
  PRIMARY KEY (reviewer, paper)   -- a reviewer bids on a paper once
);
INSERT INTO bid(reviewer, paper, conf) VALUES
  ('r1','p1',0.5), ('r1','p4',0.35),
  ('r2','p1',0.45),('r2','p2',0.4),
  ('r5','p2',0.4), ('r5','p4',0.3),
  ('r10','p1',0.4),
  ('r13','p2',0.35),('r13','p4',0.45),
  ('r3','p1',0.5), ('r3','p3',0.35),('r3','p5',0.4),
  ('r6','p3',0.4), ('r6','p5',0.35),
  ('r9','p7',0.45),('r9','p3',0.3),
  ('r4','p5',0.4),
  ('r7','p5',0.35),
  ('r11','p6',0.45),('r11','p7',0.35),
  ('r14','p6',0.4),
  ('r8','p4',0.35);
UPDATE bid SET lbl = format('bid(%s,%s)', reviewer, paper);
SELECT add_provenance('bid');
SELECT set_prob(provenance(), conf) FROM bid;

-- expertise(reviewer, topic): the reviewer's area of competence.
-- KEY ON reviewer (not (reviewer, topic)): one area per reviewer.  This
-- functional dependency reviewer -> topic is exactly what makes a single
-- paper's coverage query read-once (the safe plan groups on the topic and
-- projects the reviewer out); widen the key to (reviewer, topic) and that
-- query stops being safe.  Several reviewers share each area on purpose
-- (five in databases), so a paper's coverage lineage is genuinely
-- entangled (the same topic_of leaf is shared across co-experts).
CREATE TABLE expertise (
  reviewer TEXT NOT NULL REFERENCES reviewers(id),
  topic    TEXT NOT NULL REFERENCES topics(id),
  conf     DOUBLE PRECISION NOT NULL,
  lbl      TEXT,
  PRIMARY KEY (reviewer)
);
INSERT INTO expertise(reviewer, topic, conf) VALUES
  ('r1','t1',0.55),('r2','t1',0.5), ('r5','t1',0.5), ('r10','t1',0.45),('r13','t1',0.45),
  ('r3','t2',0.55),('r6','t2',0.5), ('r9','t2',0.45),
  ('r4','t3',0.5), ('r7','t3',0.5), ('r11','t3',0.45),('r14','t3',0.45),
  ('r8','t4',0.55),('r12','t4',0.5);
UPDATE expertise SET lbl = format('exp(%s,%s)', reviewer, topic);
SELECT add_provenance('expertise');
SELECT set_prob(provenance(), conf) FROM expertise;

-- topic_of(paper, topic): the paper is about the topic.  Papers overlap
-- on topics (p1, p2, p4, p6 all touch databases), which is what couples
-- their coverage when the paper variable is left free.
CREATE TABLE topic_of (
  paper TEXT NOT NULL REFERENCES papers(id),
  topic TEXT NOT NULL REFERENCES topics(id),
  conf  DOUBLE PRECISION NOT NULL,
  lbl   TEXT,
  PRIMARY KEY (paper, topic)      -- a paper lists a topic once
);
INSERT INTO topic_of(paper, topic, conf) VALUES
  ('p1','t1',0.6), ('p1','t2',0.55),
  ('p2','t1',0.55),('p2','t3',0.5),
  ('p3','t2',0.55),('p3','t4',0.45),
  ('p4','t1',0.5), ('p4','t4',0.5),
  ('p5','t3',0.55),('p5','t2',0.45),
  ('p6','t1',0.55),('p6','t3',0.5), ('p6','t4',0.4),
  ('p7','t2',0.5), ('p7','t3',0.45);
UPDATE topic_of SET lbl = format('topof(%s,%s)', paper, topic);
SELECT add_provenance('topic_of');
SELECT set_prob(provenance(), conf) FROM topic_of;

-- extends(citing, cited): paper `citing` builds on the earlier paper
-- `cited`.  ACYCLIC by construction (a paper only extends older ones), so
-- the recursive "what does p transitively build on?" query is read-once
-- per ancestor and works for any semiring (used in the recursive section
-- with sr_formula and probability).  Requires PostgreSQL 15+ to query.
CREATE TABLE extends (
  citing TEXT NOT NULL REFERENCES papers(id),
  cited  TEXT NOT NULL REFERENCES papers(id),
  conf   DOUBLE PRECISION NOT NULL,
  lbl    TEXT,
  PRIMARY KEY (citing, cited)
);
INSERT INTO extends(citing, cited, conf) VALUES
  ('p4','p1',0.8), ('p5','p2',0.7), ('p5','p3',0.6),
  ('p6','p4',0.9), ('p6','p5',0.7), ('p7','p3',0.6);
UPDATE extends SET lbl = format('ext(%s,%s)', citing, cited);
SELECT add_provenance('extends');
SELECT set_prob(provenance(), conf) FROM extends;

-- coreview(a, b): reviewers a and b have served on a committee together.
-- The relation is SYMMETRIC (both directions are stored), so the
-- collaboration graph is CYCLIC -- the recursive "who is reviewer r
-- connected to?" query only terminates under provsql.boolean_provenance,
-- where it computes connection *reliability* (a network-reliability /
-- #P-hard flavour).  Requires PostgreSQL 15+ to query.
CREATE TABLE coreview (
  a    TEXT NOT NULL REFERENCES reviewers(id),
  b    TEXT NOT NULL REFERENCES reviewers(id),
  conf DOUBLE PRECISION NOT NULL,
  lbl  TEXT,
  PRIMARY KEY (a, b)
);
INSERT INTO coreview(a, b, conf) VALUES
  ('r1','r2',0.8),('r2','r1',0.8),
  ('r2','r3',0.7),('r3','r2',0.7),
  ('r1','r3',0.5),('r3','r1',0.5),
  ('r3','r4',0.6),('r4','r3',0.6),
  ('r4','r5',0.7),('r5','r4',0.7),
  ('r2','r5',0.4),('r5','r2',0.4);
UPDATE coreview SET lbl = format('co(%s,%s)', a, b);
SELECT add_provenance('coreview');
SELECT set_prob(provenance(), conf) FROM coreview;

-- assignment(reviewer, paper): the candidate papers a reviewer could be
-- assigned to.  repair_key on `reviewer` makes the rows for one reviewer
-- mutually exclusive (each reviewer ends up on exactly one paper), so a
-- query over this table carries correlated provenance.
CREATE TABLE assignment (
  reviewer TEXT NOT NULL REFERENCES reviewers(id),
  paper    TEXT NOT NULL REFERENCES papers(id)
);
INSERT INTO assignment(reviewer, paper) VALUES
  ('r1','p1'), ('r1','p4'),
  ('r2','p1'), ('r2','p2'),
  ('r3','p1'), ('r3','p3'),
  ('r4','p2'), ('r4','p5');
SELECT repair_key('assignment', 'reviewer');

-- Label mappings so the Studio eval-strip's sr_formula / sr_why / sr_how
-- and PROV-XML export name the leaves instead of showing raw UUIDs. The
-- `lbl` columns exist only to feed create_provenance_mapping, which copies
-- their values into standalone (value, provenance) tables.
SELECT create_provenance_mapping('bid_label',      'bid',      'lbl');
SELECT create_provenance_mapping('expertise_label','expertise','lbl');
SELECT create_provenance_mapping('topic_of_label', 'topic_of', 'lbl');
SELECT create_provenance_mapping('extends_label',  'extends',  'lbl');
SELECT create_provenance_mapping('coreview_label', 'coreview', 'lbl');

-- `conf` only fed set_prob and `lbl` only fed create_provenance_mapping;
-- both jobs are now done, so drop the artifact columns from the base tables.
ALTER TABLE bid       DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE expertise DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE topic_of  DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE extends   DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE coreview  DROP COLUMN conf, DROP COLUMN lbl;

\echo
\echo '----------------------------------------------------------------------'
\echo 'peer_review_demo loaded.  Point Studio at it and try the queries in'
\echo 'the case-study chapter.  Three queries over the SAME data:'
\echo
\echo '-- SAFE by shape: a reviewer who bid on the paper and has some'
\echo '--   expertise.  Hierarchical regardless of keys -> read-once.'
\echo 'SELECT b.paper FROM bid b, expertise e'
\echo ' WHERE b.reviewer = e.reviewer GROUP BY b.paper;'
\echo
\echo '-- SAFE by a key: is paper p1 competently covered (a reviewer who'
\echo '--   bid on it AND is expert in one of its topics)?  Read-once'
\echo '--   thanks to the PRIMARY KEY on expertise(reviewer).'
\echo 'SELECT 1 FROM bid b, expertise e, topic_of t'
\echo ' WHERE b.reviewer=e.reviewer AND e.topic=t.topic'
\echo "   AND b.paper='p1' AND t.paper='p1';"
\echo
\echo '-- HARD: is the whole program covered?  The free paper variable'
\echo '--   keeps this non-hierarchical -> #P-hard -> needs a compiler.'
\echo 'SELECT 1 FROM bid b, expertise e, topic_of t'
\echo ' WHERE b.reviewer=e.reviewer AND e.topic=t.topic AND t.paper=b.paper;'
\echo '----------------------------------------------------------------------'
\echo
