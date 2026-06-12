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
DROP TABLE IF EXISTS lead_chair, urgent_sub, prescreen, score_pass, flag_pass CASCADE;
DROP TABLE IF EXISTS bid_label, expertise_label, topic_of_label,
                     extends_label, coreview_label,
                     lead_chair_label, urgent_sub_label, prescreen_label,
                     score_pass_label, flag_pass_label;

-- Dimension tables (deterministic, no provenance).
DROP TABLE IF EXISTS reviewers CASCADE;
CREATE TABLE reviewers (id TEXT PRIMARY KEY, name TEXT NOT NULL);
INSERT INTO reviewers VALUES
  ('r1','Alice'), ('r2','Bob'),   ('r3','Carol'), ('r4','Dave'),
  ('r5','Eve'),   ('r6','Frank'), ('r7','Grace'), ('r8','Heidi'),
  ('r9','Ivan'),  ('r10','Judy'), ('r11','Karl'), ('r12','Lara'),
  ('r13','Mona'), ('r14','Nick');

DROP TABLE IF EXISTS papers CASCADE;
CREATE TABLE papers (id TEXT PRIMARY KEY, title TEXT NOT NULL);
INSERT INTO papers VALUES
  ('p1', 'A Provenance Circuit Calculus'),
  ('p2', 'Treewidth Bounds for Safe Queries'),
  ('p3', 'Sampling Beats Compilation, Sometimes'),
  ('p4', 'Functional Dependencies and Read-Once Lineage'),
  ('p5', 'A Dichotomy for Conjunctive Coverage'),
  ('p6', 'Knowledge Compilation in Practice'),
  ('p7', 'Repair-Key Semantics for Assignments');

DROP TABLE IF EXISTS topics CASCADE;
CREATE TABLE topics (id TEXT PRIMARY KEY, name TEXT NOT NULL);
INSERT INTO topics VALUES
  ('t1','databases'), ('t2','logic'), ('t3','systems'), ('t4','theory');

-- bid(reviewer, paper): the reviewer offered to review the paper; conf
-- is how firm the bid is.  Several databases-experts bid on p1, which is
-- what makes p1's coverage interesting.
DROP TABLE IF EXISTS bid CASCADE;
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
DROP TABLE IF EXISTS expertise CASCADE;
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
DROP TABLE IF EXISTS topic_of CASCADE;
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
DROP TABLE IF EXISTS extends CASCADE;
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
DROP TABLE IF EXISTS coreview CASCADE;
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
DROP TABLE IF EXISTS assignment CASCADE;
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

-- Inversion-free demo fixture (Step 7).  A self-join witness that is NOT
-- read-once yet is inversion-free, so a single global variable order
-- compiles it in linear time while a generic compiler / tree decomposition
-- chokes.  Olga (r15) is a prolific bidder who skimmed a 24-paper
-- submission batch (q01..q24); `recommend` and `champion` are two
-- post-review signals (she recommended a paper for acceptance, and would
-- champion it at the PC meeting).  The witness query asks for a reviewer
-- whose bids overlap both a recommendation and a championing; grouping on
-- the reviewer shares the bid(r15,*) leaves between the two sides, so the
-- lineage is not read-once but stays inversion-free (root = reviewer, a
-- consistent-unification self-join on `bid`).
--
-- Kept isolated from the core instance so Steps 1-6 and 8 are unchanged:
-- Olga has no `expertise` row and the submission papers carry no
-- `topic_of`, so none of this data reaches the coverage / recursive
-- queries (which all join through expertise / topic_of / the graphs).
DELETE FROM bid WHERE reviewer = 'r15';
DELETE FROM papers WHERE id LIKE 'q%';
DELETE FROM reviewers WHERE id = 'r15';
INSERT INTO reviewers VALUES ('r15','Olga');
INSERT INTO papers SELECT 'q'||to_char(g,'FM00'), format('Submission %s', g)
  FROM generate_series(1,24) g;
INSERT INTO bid(reviewer, paper, conf, lbl)
  SELECT 'r15', p.id, 0.5, format('bid(r15,%s)', p.id)
  FROM papers p WHERE p.id LIKE 'q%';
SELECT set_prob(provenance(), 0.5) FROM bid WHERE reviewer = 'r15';

DROP TABLE IF EXISTS recommend CASCADE;
CREATE TABLE recommend (
  reviewer TEXT NOT NULL REFERENCES reviewers(id),
  paper    TEXT NOT NULL REFERENCES papers(id),
  lbl      TEXT,
  PRIMARY KEY (reviewer, paper)
);
INSERT INTO recommend(reviewer, paper, lbl)
  SELECT 'r15', p.id, format('rec(r15,%s)', p.id)
  FROM papers p WHERE p.id LIKE 'q%';
SELECT add_provenance('recommend');
SELECT set_prob(provenance(), 0.4) FROM recommend;
SELECT create_provenance_mapping('recommend_label', 'recommend', 'lbl');
ALTER TABLE recommend DROP COLUMN lbl;

DROP TABLE IF EXISTS champion CASCADE;
CREATE TABLE champion (
  reviewer TEXT NOT NULL REFERENCES reviewers(id),
  paper    TEXT NOT NULL REFERENCES papers(id),
  lbl      TEXT,
  PRIMARY KEY (reviewer, paper)
);
INSERT INTO champion(reviewer, paper, lbl)
  SELECT 'r15', p.id, format('champ(r15,%s)', p.id)
  FROM papers p WHERE p.id LIKE 'q%';
SELECT add_provenance('champion');
SELECT set_prob(provenance(), 0.3) FROM champion;
SELECT create_provenance_mapping('champion_label', 'champion', 'lbl');
ALTER TABLE champion DROP COLUMN lbl;

-- Möbius-cancellation demo fixture (Part A, "safe by cancellation").  The
-- canonical Dalvi-Suciu witness q9 / QW: a UNION of conjunctive queries
-- that is SAFE -- PTIME data complexity -- yet only because the #P-hard
-- term of its inclusion-exclusion expansion carries a zero Möbius
-- coefficient and cancels.  No circuit-level method keeps up (q9 has no
-- polynomial-size OBDD / FBDD / d-DNNF), and on dense data its *joint*
-- treewidth is unbounded too, so the joint-width route of Part C declines
-- and the Möbius route -- which reads the structure off the QUERY, not the
-- data -- is the only exact one.  That is why this fixture must be DENSE:
-- on a sparse instance the joint width is bounded and Part C's compiler
-- would handle it, hiding the route this step exists to show.
--
-- Dressed as a second-tier external review pool, isolated from the core
-- instance (its own c*/e* domain, no foreign keys, so it reaches none of
-- the coverage / recursive queries): four area chairs (c1..c4) each run
-- three independent assessment passes -- a prescreen, a score and a flag --
-- over four embargoed submissions (e1..e4), COMPLETE bipartite so the joint
-- width genuinely grows; `lead_chair` marks the senior chairs and
-- `urgent_sub` the time-critical submissions.  Mapped onto q9's
-- R(x), S1(x,y), S2(x,y), S3(x,y), T(y) as
-- lead_chair / prescreen / score_pass / flag_pass / urgent_sub.  The union
-- query has no tidy English gloss -- its safety is purely structural -- and
-- that is exactly the point.  All tuples carry probability 0.1, low enough
-- that the exact answer stays away from saturation.
DROP TABLE IF EXISTS lead_chair, urgent_sub, prescreen, score_pass, flag_pass CASCADE;
CREATE TABLE lead_chair (chair TEXT NOT NULL, conf DOUBLE PRECISION, lbl TEXT, PRIMARY KEY(chair));
CREATE TABLE urgent_sub (sub   TEXT NOT NULL, conf DOUBLE PRECISION, lbl TEXT, PRIMARY KEY(sub));
CREATE TABLE prescreen  (chair TEXT, sub TEXT, conf DOUBLE PRECISION, lbl TEXT, PRIMARY KEY(chair, sub));
CREATE TABLE score_pass (chair TEXT, sub TEXT, conf DOUBLE PRECISION, lbl TEXT, PRIMARY KEY(chair, sub));
CREATE TABLE flag_pass  (chair TEXT, sub TEXT, conf DOUBLE PRECISION, lbl TEXT, PRIMARY KEY(chair, sub));
INSERT INTO lead_chair(chair, conf) SELECT 'c'||g, 0.1 FROM generate_series(1,4) g;
INSERT INTO urgent_sub(sub,   conf) SELECT 'e'||g, 0.1 FROM generate_series(1,4) g;
INSERT INTO prescreen(chair, sub, conf)
  SELECT 'c'||i, 'e'||j, 0.1 FROM generate_series(1,4) i, generate_series(1,4) j;
INSERT INTO score_pass(chair, sub, conf)
  SELECT 'c'||i, 'e'||j, 0.1 FROM generate_series(1,4) i, generate_series(1,4) j;
INSERT INTO flag_pass(chair, sub, conf)
  SELECT 'c'||i, 'e'||j, 0.1 FROM generate_series(1,4) i, generate_series(1,4) j;
UPDATE lead_chair SET lbl = format('lead(%s)', chair);
UPDATE urgent_sub SET lbl = format('urgent(%s)', sub);
UPDATE prescreen  SET lbl = format('pre(%s,%s)', chair, sub);
UPDATE score_pass SET lbl = format('score(%s,%s)', chair, sub);
UPDATE flag_pass  SET lbl = format('flag(%s,%s)', chair, sub);
SELECT add_provenance('lead_chair'); SELECT add_provenance('urgent_sub');
SELECT add_provenance('prescreen');  SELECT add_provenance('score_pass');
SELECT add_provenance('flag_pass');
SELECT set_prob(provenance(), conf) FROM lead_chair;
SELECT set_prob(provenance(), conf) FROM urgent_sub;
SELECT set_prob(provenance(), conf) FROM prescreen;
SELECT set_prob(provenance(), conf) FROM score_pass;
SELECT set_prob(provenance(), conf) FROM flag_pass;
SELECT create_provenance_mapping('lead_chair_label', 'lead_chair', 'lbl');
SELECT create_provenance_mapping('urgent_sub_label', 'urgent_sub', 'lbl');
SELECT create_provenance_mapping('prescreen_label',  'prescreen',  'lbl');
SELECT create_provenance_mapping('score_pass_label', 'score_pass', 'lbl');
SELECT create_provenance_mapping('flag_pass_label',  'flag_pass',  'lbl');

-- Label mappings so the Studio eval-strip's sr_formula / sr_why / sr_how
-- and PROV-XML export name the leaves instead of showing raw UUIDs. The
-- `lbl` columns exist only to feed create_provenance_mapping, which copies
-- their values into standalone (value, provenance) tables.
SELECT create_provenance_mapping('bid_label',      'bid',      'lbl');
SELECT create_provenance_mapping('expertise_label','expertise','lbl');
SELECT create_provenance_mapping('topic_of_label', 'topic_of', 'lbl');
SELECT create_provenance_mapping('extends_label',  'extends',  'lbl');
SELECT create_provenance_mapping('coreview_label', 'coreview', 'lbl');

-- A single `label` mapping: the union of every per-tuple label above, so a
-- text-based semiring (sr_formula / sr_why / sr_how) can name the leaves of
-- a query spanning several relations through one mapping argument, rather
-- than picking the relation-specific mapping each time.
DROP TABLE IF EXISTS label CASCADE;
CREATE TABLE label AS
            SELECT value, provenance FROM bid_label
  UNION ALL SELECT value, provenance FROM expertise_label
  UNION ALL SELECT value, provenance FROM topic_of_label
  UNION ALL SELECT value, provenance FROM extends_label
  UNION ALL SELECT value, provenance FROM coreview_label
  UNION ALL SELECT value, provenance FROM recommend_label
  UNION ALL SELECT value, provenance FROM champion_label
  UNION ALL SELECT value, provenance FROM lead_chair_label
  UNION ALL SELECT value, provenance FROM urgent_sub_label
  UNION ALL SELECT value, provenance FROM prescreen_label
  UNION ALL SELECT value, provenance FROM score_pass_label
  UNION ALL SELECT value, provenance FROM flag_pass_label;
CREATE INDEX ON label(provenance);

-- `conf` only fed set_prob and `lbl` only fed create_provenance_mapping;
-- both jobs are now done, so drop the artifact columns from the base tables.
ALTER TABLE bid       DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE expertise DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE topic_of  DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE extends   DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE coreview  DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE lead_chair DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE urgent_sub DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE prescreen  DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE score_pass DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE flag_pass  DROP COLUMN conf, DROP COLUMN lbl;

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
