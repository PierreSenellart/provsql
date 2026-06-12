\set ECHO none
\pset format unaligned
-- One check probes that 'independent' rejects a non-read-once program; pin
-- the joint-width debug GUC off so its higher (tractable) fallback does
-- not mask that rejection.
SET provsql.joint_width = off;

-- Case Study 7: Peer-Review Assignment and Knowledge Compilation.
-- Backs doc/source/user/casestudy7.rst.  Exercises, over one dataset:
--   * a safe-by-shape (hierarchical) query -> read-once;
--   * a coverage query for ONE paper that is H0-shaped yet read-once
--     thanks to the PRIMARY KEY on expertise(reviewer) (the FD-grouped
--     safe plan, only under provsql.boolean_provenance);
--   * the same coverage over the WHOLE program -> genuinely #P-hard;
--   * a HAVING COUNT(*) pre-pass query;
--   * a repair_key (block-correlated) query;
--   * recursive reachability: acyclic (any semiring) and cyclic
--     (network reliability, only under provsql.boolean_provenance).
-- Recursive queries require PostgreSQL 15+ (this test lives in
-- schedule.15).  No external compiler is invoked (only the built-in
-- tree-decomposition / independent / possible-worlds methods).
--
-- Each probabilistic query is materialised into a table and stripped of
-- its auto-added provsql column (random input-gate UUIDs) so the
-- comparison is on the deterministic probabilities / formulas.

-- ---------------------------------------------------------------------
-- Setup: schema and seed, mirroring doc/casestudy7/setup.sql.
-- ---------------------------------------------------------------------
CREATE TABLE reviewers (id text PRIMARY KEY, name text NOT NULL);
INSERT INTO reviewers VALUES
  ('r1','Alice'),('r2','Bob'),('r3','Carol'),('r4','Dave'),
  ('r5','Eve'),('r6','Frank'),('r7','Grace'),('r8','Heidi'),
  ('r9','Ivan'),('r10','Judy'),('r11','Karl'),('r12','Lara'),
  ('r13','Mona'),('r14','Nick');

CREATE TABLE papers (id text PRIMARY KEY, title text NOT NULL);
INSERT INTO papers VALUES
  ('p1','A'),('p2','B'),('p3','C'),('p4','D'),('p5','E'),('p6','F'),('p7','G');

CREATE TABLE topics (id text PRIMARY KEY, name text NOT NULL);
INSERT INTO topics VALUES
  ('t1','databases'),('t2','logic'),('t3','systems'),('t4','theory');

CREATE TABLE bid (
  reviewer text NOT NULL REFERENCES reviewers(id),
  paper    text NOT NULL REFERENCES papers(id),
  conf     double precision NOT NULL,
  lbl      text,
  PRIMARY KEY (reviewer, paper)
);
INSERT INTO bid(reviewer, paper, conf) VALUES
  ('r1','p1',0.5),('r1','p4',0.35),
  ('r2','p1',0.45),('r2','p2',0.4),
  ('r5','p2',0.4),('r5','p4',0.3),
  ('r10','p1',0.4),
  ('r13','p2',0.35),('r13','p4',0.45),
  ('r3','p1',0.5),('r3','p3',0.35),('r3','p5',0.4),
  ('r6','p3',0.4),('r6','p5',0.35),
  ('r9','p7',0.45),('r9','p3',0.3),
  ('r4','p5',0.4),
  ('r7','p5',0.35),
  ('r11','p6',0.45),('r11','p7',0.35),
  ('r14','p6',0.4),
  ('r8','p4',0.35);
UPDATE bid SET lbl = format('bid(%s,%s)', reviewer, paper);
SELECT add_provenance('bid');
DO $$ BEGIN PERFORM set_prob(provenance(), conf) FROM bid; END $$;

CREATE TABLE expertise (
  reviewer text NOT NULL REFERENCES reviewers(id),
  topic    text NOT NULL REFERENCES topics(id),
  conf     double precision NOT NULL,
  lbl      text,
  PRIMARY KEY (reviewer)
);
INSERT INTO expertise(reviewer, topic, conf) VALUES
  ('r1','t1',0.55),('r2','t1',0.5),('r5','t1',0.5),('r10','t1',0.45),('r13','t1',0.45),
  ('r3','t2',0.55),('r6','t2',0.5),('r9','t2',0.45),
  ('r4','t3',0.5),('r7','t3',0.5),('r11','t3',0.45),('r14','t3',0.45),
  ('r8','t4',0.55),('r12','t4',0.5);
UPDATE expertise SET lbl = format('exp(%s,%s)', reviewer, topic);
SELECT add_provenance('expertise');
DO $$ BEGIN PERFORM set_prob(provenance(), conf) FROM expertise; END $$;

CREATE TABLE topic_of (
  paper text NOT NULL REFERENCES papers(id),
  topic text NOT NULL REFERENCES topics(id),
  conf  double precision NOT NULL,
  lbl   text,
  PRIMARY KEY (paper, topic)
);
INSERT INTO topic_of(paper, topic, conf) VALUES
  ('p1','t1',0.6),('p1','t2',0.55),
  ('p2','t1',0.55),('p2','t3',0.5),
  ('p3','t2',0.55),('p3','t4',0.45),
  ('p4','t1',0.5),('p4','t4',0.5),
  ('p5','t3',0.55),('p5','t2',0.45),
  ('p6','t1',0.55),('p6','t3',0.5),('p6','t4',0.4),
  ('p7','t2',0.5),('p7','t3',0.45);
UPDATE topic_of SET lbl = format('topof(%s,%s)', paper, topic);
SELECT add_provenance('topic_of');
DO $$ BEGIN PERFORM set_prob(provenance(), conf) FROM topic_of; END $$;

CREATE TABLE extends (
  citing text NOT NULL REFERENCES papers(id),
  cited  text NOT NULL REFERENCES papers(id),
  conf   double precision NOT NULL,
  lbl    text,
  PRIMARY KEY (citing, cited)
);
INSERT INTO extends(citing, cited, conf) VALUES
  ('p4','p1',0.8),('p5','p2',0.7),('p5','p3',0.6),
  ('p6','p4',0.9),('p6','p5',0.7),('p7','p3',0.6);
UPDATE extends SET lbl = format('ext(%s,%s)', citing, cited);
SELECT add_provenance('extends');
DO $$ BEGIN PERFORM set_prob(provenance(), conf) FROM extends; END $$;

CREATE TABLE coreview (
  a    text NOT NULL REFERENCES reviewers(id),
  b    text NOT NULL REFERENCES reviewers(id),
  conf double precision NOT NULL,
  lbl  text,
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
DO $$ BEGIN PERFORM set_prob(provenance(), conf) FROM coreview; END $$;

CREATE TABLE assignment (
  reviewer text NOT NULL REFERENCES reviewers(id),
  paper    text NOT NULL REFERENCES papers(id)
);
INSERT INTO assignment(reviewer, paper) VALUES
  ('r1','p1'),('r1','p4'),
  ('r2','p1'),('r2','p2'),
  ('r3','p1'),('r3','p3'),
  ('r4','p2'),('r4','p5');
SELECT repair_key('assignment', 'reviewer');

-- External-review pool for the Möbius (safe-by-cancellation) step: the
-- canonical q9 / QW witness as four area chairs each running three
-- assessment passes over four embargoed submissions, COMPLETE bipartite.
-- Dense on purpose: on sparse data the joint-width route would bound it and
-- mask the point; dense, its joint width exceeds the cap so that route
-- declines and Möbius -- reading the structure off the query -- is the only
-- exact one.  All tuples at probability 0.1 (keeps the answer off
-- saturation).  Isolated (own c*/e* domain, no foreign keys).
CREATE TABLE lead_chair (chair text PRIMARY KEY);
CREATE TABLE urgent_sub (sub   text PRIMARY KEY);
CREATE TABLE prescreen  (chair text, sub text, PRIMARY KEY (chair, sub));
CREATE TABLE score_pass (chair text, sub text, PRIMARY KEY (chair, sub));
CREATE TABLE flag_pass  (chair text, sub text, PRIMARY KEY (chair, sub));
INSERT INTO lead_chair SELECT 'c'||g FROM generate_series(1,4) g;
INSERT INTO urgent_sub SELECT 'e'||g FROM generate_series(1,4) g;
INSERT INTO prescreen  SELECT 'c'||i, 'e'||j FROM generate_series(1,4) i, generate_series(1,4) j;
INSERT INTO score_pass SELECT 'c'||i, 'e'||j FROM generate_series(1,4) i, generate_series(1,4) j;
INSERT INTO flag_pass  SELECT 'c'||i, 'e'||j FROM generate_series(1,4) i, generate_series(1,4) j;
SELECT add_provenance('lead_chair'); SELECT add_provenance('urgent_sub');
SELECT add_provenance('prescreen');  SELECT add_provenance('score_pass');
SELECT add_provenance('flag_pass');
DO $$ BEGIN
  PERFORM set_prob(provenance(), 0.1) FROM lead_chair;
  PERFORM set_prob(provenance(), 0.1) FROM urgent_sub;
  PERFORM set_prob(provenance(), 0.1) FROM prescreen;
  PERFORM set_prob(provenance(), 0.1) FROM score_pass;
  PERFORM set_prob(provenance(), 0.1) FROM flag_pass;
END $$;

SELECT create_provenance_mapping('extends_label',  'extends',  'lbl');

ALTER TABLE bid       DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE expertise DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE topic_of  DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE extends   DROP COLUMN conf, DROP COLUMN lbl;
ALTER TABLE coreview  DROP COLUMN conf, DROP COLUMN lbl;

-- ---------------------------------------------------------------------
-- Step 1: safe by shape (hierarchical, read-once for any keys).
-- ---------------------------------------------------------------------
SET provsql.provenance = 'semiring';
CREATE TABLE cs7_safe AS
  SELECT b.paper, round(probability_evaluate(provenance(),'independent')::numeric,6) AS p_indep
  FROM bid b, expertise e WHERE b.reviewer = e.reviewer GROUP BY b.paper;
SELECT remove_provenance('cs7_safe');
SELECT paper, p_indep FROM cs7_safe ORDER BY paper;
DROP TABLE cs7_safe;

-- ---------------------------------------------------------------------
-- Step 2: safe by a key.  Coverage of p1 is H0-shaped; the literal
-- circuit (boolean_provenance off) is NOT read-once -> independent
-- errors.  Under boolean_provenance the FD reviewer->topic makes the
-- rewrite read-once -> independent matches the exact baseline.
-- ---------------------------------------------------------------------
DO $$
DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'independent')
      FROM (SELECT DISTINCT 1 FROM bid b, expertise e, topic_of t
            WHERE b.reviewer=e.reviewer AND e.topic=t.topic
              AND b.paper='p1' AND t.paper='p1') q;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected literal coverage(p1) to reject independent';
  END IF;
END $$;

SET provsql.provenance = 'boolean';
CREATE TABLE cs7_cov_p1 AS
  SELECT round(probability_evaluate(provenance(),'independent')::numeric,6)        AS p1_indep,
         round(probability_evaluate(provenance(),'tree-decomposition')::numeric,6) AS p1_exact
  FROM (SELECT DISTINCT 1 FROM bid b, expertise e, topic_of t
        WHERE b.reviewer=e.reviewer AND e.topic=t.topic
          AND b.paper='p1' AND t.paper='p1') q;
SELECT remove_provenance('cs7_cov_p1');
SELECT p1_indep, p1_exact FROM cs7_cov_p1;
DROP TABLE cs7_cov_p1;

-- ---------------------------------------------------------------------
-- Step 3: genuinely hard.  Whole-program coverage (paper free) stays
-- non-hierarchical even with the FD -> #P-hard.  independent errors
-- even under boolean_provenance; the exact methods succeed.
-- ---------------------------------------------------------------------
DO $$
DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'independent')
      FROM (SELECT DISTINCT 1 FROM bid b, expertise e, topic_of t
            WHERE b.reviewer=e.reviewer AND e.topic=t.topic AND t.paper=b.paper) q;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected whole-program coverage to reject independent';
  END IF;
END $$;
SET provsql.provenance = 'semiring';

CREATE TABLE cs7_hard AS
  SELECT round(probability_evaluate(provenance(),'tree-decomposition')::numeric,6) AS hard_exact
  FROM (SELECT DISTINCT 1 FROM bid b, expertise e, topic_of t
        WHERE b.reviewer=e.reviewer AND e.topic=t.topic AND t.paper=b.paper) q;
SELECT remove_provenance('cs7_hard');
SELECT hard_exact FROM cs7_hard;
DROP TABLE cs7_hard;

-- ---------------------------------------------------------------------
-- Part A, safe by cancellation (Möbius).  The dense q9 / QW external pool:
-- a safe UCQ that is neither hierarchical nor inversion-free, tractable only
-- because its #P-hard term carries a zero Möbius coefficient.  Under the
-- DEFAULT routing (joint-width on, Möbius on) the dense data pushes the
-- joint width past the cap so the joint route declines and Möbius fires:
-- the root is a gate_mobius and the exact answer is 0.056923.  The literal
-- lineage is carried on the gate, so other evaluations still pass through.
-- ---------------------------------------------------------------------
SET provsql.provenance = 'boolean';
SET provsql.joint_width = on;
SET provsql.mobius = on;
CREATE TABLE cs7_mobius AS
  SELECT provenance() AS tok FROM (
    SELECT 1 FROM lead_chair r, prescreen a1, flag_pass a3, urgent_sub t3
      WHERE r.chair = a1.chair AND a3.sub = t3.sub
    UNION
    SELECT 1 FROM prescreen b1, score_pass b2, flag_pass b3, urgent_sub tb
      WHERE b1.chair = b2.chair AND b1.sub = b2.sub AND b3.sub = tb.sub
    UNION
    SELECT 1 FROM score_pass c2, flag_pass c3, flag_pass c3b, urgent_sub tc
      WHERE c2.chair = c3.chair AND c2.sub = c3.sub AND c3b.sub = tc.sub
    UNION
    SELECT 1 FROM lead_chair d, prescreen d1, prescreen d1b,
                  score_pass d2, score_pass d2b, flag_pass d3
      WHERE d.chair = d1.chair AND d1b.chair = d2.chair AND d1b.sub = d2.sub
        AND d2b.chair = d3.chair AND d2b.sub = d3.sub) q;
SELECT remove_provenance('cs7_mobius');
SET provsql.joint_width = off;
SELECT get_gate_type(tok) AS mobius_root,
       round(probability_evaluate(tok)::numeric, 6) AS mobius_prob
  FROM cs7_mobius;
DROP TABLE cs7_mobius;

-- Routing guard (the case study's premise): under the DEFAULT routing every
-- question fires its INTENDED method and no route pre-empts another.  Möbius
-- has precedence among the hard-UCQ routes, so guard that it fires ONLY for
-- the safe-by-cancellation q9 pool: the genuinely-hard coverage queries (a
-- single non-hierarchical CQ, and a correlated one) are NOT safe by
-- cancellation, so Möbius must decline them and the joint-width compiler must
-- take over.
SET provsql.provenance = 'boolean';
SET provsql.joint_width = on;
SET provsql.mobius = on;
DO $$
DECLARE g text;
BEGIN
  SELECT get_gate_type(provenance()) INTO g
    FROM (SELECT DISTINCT 1 FROM bid b, expertise e, topic_of t
          WHERE b.reviewer=e.reviewer AND e.topic=t.topic AND t.paper=b.paper) q;
  IF g = 'mobius' THEN
    RAISE EXCEPTION 'Mobius wrongly pre-empted the hard coverage (root %)', g;
  END IF;
  SELECT get_gate_type(provenance()) INTO g
    FROM (SELECT DISTINCT 1 FROM assignment a, expertise e, topic_of t
          WHERE a.reviewer=e.reviewer AND e.topic=t.topic AND t.paper=a.paper) q;
  IF g = 'mobius' THEN
    RAISE EXCEPTION 'Mobius wrongly pre-empted the correlated coverage (root %)', g;
  END IF;
END $$;
SET provsql.joint_width = off;

-- ---------------------------------------------------------------------
-- Step 6: HAVING count(*) pre-pass.
-- ---------------------------------------------------------------------
CREATE TABLE cs7_having AS
  SELECT b.paper, round(probability_evaluate(provenance())::numeric,6) AS p_having
  FROM bid b, expertise e WHERE b.reviewer=e.reviewer GROUP BY b.paper HAVING count(*) >= 2;
SELECT remove_provenance('cs7_having');
SELECT paper, p_having FROM cs7_having ORDER BY paper;
DROP TABLE cs7_having;

-- ---------------------------------------------------------------------
-- Step 7 (inversion-free): a prolific bidder's bid/recommend/champion
-- self-join.  The literal circuit is NOT read-once (independent rejects),
-- yet the query is inversion-free, so the structured-d-DNNF builder
-- evaluates it exactly -- cross-checked here against possible-worlds at a
-- small size, and matched by the default chain (which takes the
-- inversion-free rung).  doc/casestudy7/setup.sql ships the same shape
-- scaled to 24 papers, where the generic compilers / tree decomposition
-- blow up while the inversion-free path stays linear.
-- ---------------------------------------------------------------------
INSERT INTO reviewers VALUES ('r15','Olga');
INSERT INTO papers SELECT 'q'||g, format('Submission %s', g) FROM generate_series(1,4) g;
INSERT INTO bid(reviewer, paper) SELECT 'r15', 'q'||g FROM generate_series(1,4) g;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM bid WHERE reviewer = 'r15'; END $$;
CREATE TABLE recommend (
  reviewer text NOT NULL REFERENCES reviewers(id),
  paper    text NOT NULL REFERENCES papers(id),
  PRIMARY KEY (reviewer, paper)
);
INSERT INTO recommend SELECT 'r15', 'q'||g FROM generate_series(1,4) g;
SELECT add_provenance('recommend');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.4) FROM recommend; END $$;
CREATE TABLE champion (
  reviewer text NOT NULL REFERENCES reviewers(id),
  paper    text NOT NULL REFERENCES papers(id),
  PRIMARY KEY (reviewer, paper)
);
INSERT INTO champion SELECT 'r15', 'q'||g FROM generate_series(1,4) g;
SELECT add_provenance('champion');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.3) FROM champion; END $$;

-- Build the witness as a top-level query so the planner attaches the
-- inversion-free certificate to its per-row root.
CREATE TABLE cs7_if_w AS
  SELECT b1.reviewer AS reviewer, provenance() AS p
  FROM bid b1, recommend a, bid b2, champion c
  WHERE b1.reviewer = a.reviewer AND b1.paper = a.paper
    AND b1.reviewer = b2.reviewer
    AND b2.reviewer = c.reviewer AND b2.paper = c.paper
  GROUP BY b1.reviewer;
SELECT remove_provenance('cs7_if_w');

DO $$
DECLARE tok uuid; raised boolean := false;
BEGIN
  SELECT p INTO tok FROM cs7_if_w;
  BEGIN PERFORM probability_evaluate(tok, 'independent');
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected inversion-free witness to reject independent (not read-once)';
  END IF;
END $$;

CREATE TABLE cs7_if AS
  SELECT round(probability_evaluate(p, 'inversion-free')::numeric,6)  AS p_if,
         round(probability_evaluate(p, 'possible-worlds')::numeric,6) AS p_pw,
         round(probability_evaluate(p)::numeric,6)                    AS p_default
  FROM cs7_if_w;
SELECT p_if, p_pw, p_default FROM cs7_if;
DROP TABLE cs7_if;
DROP TABLE cs7_if_w;

-- ---------------------------------------------------------------------
-- Step 7: correlation via repair_key.  Exact methods are correct;
-- independent would be wrong (correlated by construction).
-- ---------------------------------------------------------------------
-- independent handles the mulinput block correlation correctly (it sums
-- mutually exclusive siblings within a block), so it matches the exact
-- methods rather than over/under-counting.
CREATE TABLE cs7_assign AS
  SELECT paper,
         round(probability_evaluate(provenance(),'independent')::numeric,6)        AS p_indep,
         round(probability_evaluate(provenance(),'tree-decomposition')::numeric,6) AS p_exact
  FROM assignment GROUP BY paper;
SELECT remove_provenance('cs7_assign');
SELECT paper, p_indep, p_exact FROM cs7_assign ORDER BY paper;
DROP TABLE cs7_assign;

-- ---------------------------------------------------------------------
-- Step 9: hard *and* correlated -- the joint-width route.  The #P-hard
-- cyclic coverage of Step 3, recognised at planning time under the
-- 'boolean' class and compiled along a tree decomposition of the data,
-- agrees per paper with the circuit compiler (joint_width off); and over
-- the repair_key-correlated assignment it is the only exact and tractable
-- route -- independent rejects the literal circuit.  The route is part of
-- the Boolean machinery, so it needs provsql.provenance = 'boolean' (the
-- joint_width GUC merely switches the recognition on or off within it).
-- ---------------------------------------------------------------------
SET provsql.provenance = 'boolean';
SET provsql.joint_width = on;
CREATE TABLE cs7_jw_on AS
  SELECT t.paper AS paper, probability_evaluate(provenance()) AS p
    FROM bid b, expertise e, topic_of t
    WHERE b.reviewer = e.reviewer AND e.topic = t.topic AND t.paper = b.paper
    GROUP BY t.paper;
SELECT remove_provenance('cs7_jw_on');
SET provsql.joint_width = off;
CREATE TABLE cs7_jw_off AS
  SELECT t.paper AS paper, probability_evaluate(provenance()) AS p
    FROM bid b, expertise e, topic_of t
    WHERE b.reviewer = e.reviewer AND e.topic = t.topic AND t.paper = b.paper
    GROUP BY t.paper;
SELECT remove_provenance('cs7_jw_off');
SELECT count(*) AS n_papers,
       max(abs(a.p - b.p)) < 1e-9 AS agree
  FROM cs7_jw_on a JOIN cs7_jw_off b USING (paper);
DROP TABLE cs7_jw_on; DROP TABLE cs7_jw_off;

-- Hard + correlated: cyclic coverage over the repair_key assignment.  The
-- joint-width value (on) matches the circuit compiler (off) exactly.
SET provsql.joint_width = on;
CREATE TABLE cs7_jw_corr_on AS
  SELECT round(probability_evaluate(provenance())::numeric, 6) AS jw_correlated
  FROM (SELECT DISTINCT 1 FROM assignment a, expertise e, topic_of t
        WHERE a.reviewer = e.reviewer AND e.topic = t.topic
          AND t.paper = a.paper) q;
SELECT remove_provenance('cs7_jw_corr_on');
SET provsql.joint_width = off;
CREATE TABLE cs7_jw_corr_off AS
  SELECT round(probability_evaluate(provenance())::numeric, 6) AS circuit_compiler
  FROM (SELECT DISTINCT 1 FROM assignment a, expertise e, topic_of t
        WHERE a.reviewer = e.reviewer AND e.topic = t.topic
          AND t.paper = a.paper) q;
SELECT remove_provenance('cs7_jw_corr_off');
SELECT a.jw_correlated, b.circuit_compiler
  FROM cs7_jw_corr_on a, cs7_jw_corr_off b;
DROP TABLE cs7_jw_corr_on, cs7_jw_corr_off;
-- With the literal circuit (joint_width off) independent rejects it.
DO $$
DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'independent')
      FROM (SELECT DISTINCT 1 FROM assignment a, expertise e, topic_of t
            WHERE a.reviewer = e.reviewer AND e.topic = t.topic
              AND t.paper = a.paper) q;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected independent to reject the correlated cyclic coverage';
  END IF;
END $$;
SET provsql.joint_width = on;
SET provsql.provenance = 'semiring';

-- ---------------------------------------------------------------------
-- Step 10: recursive lineage (PostgreSQL 15+).
-- ---------------------------------------------------------------------
-- Acyclic: what does p6 transitively build on?  Read-once per ancestor,
-- any semiring (Boolean formula + possible-worlds probability).
CREATE TABLE cs7_anc AS
  WITH RECURSIVE anc(paper) AS (
      SELECT 'p6'
    UNION
      SELECT e.cited FROM extends e JOIN anc a ON e.citing = a.paper
  )
  SELECT paper,
         sr_formula(provenance(),'extends_label') AS lineage,
         round(probability_evaluate(provenance(),'possible-worlds')::numeric,6) AS prob
  FROM anc WHERE paper <> 'p6';
SELECT remove_provenance('cs7_anc');
SELECT paper, lineage, prob FROM cs7_anc ORDER BY paper;
DROP TABLE cs7_anc;

-- Cyclic without boolean_provenance: the fixpoint never stabilises.
SET provsql.provenance = 'semiring';
DO $$
DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM node FROM (
      WITH RECURSIVE conn(node) AS (
          SELECT 'r1'
        UNION
          SELECT e.b FROM coreview e JOIN conn c ON e.a = c.node
      ) SELECT node FROM conn) s;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected cyclic reachability to fail without boolean_provenance';
  END IF;
END $$;

-- Cyclic under boolean_provenance: reachability converges; the
-- probability is connection reliability.
SET provsql.provenance = 'boolean';
CREATE TABLE cs7_conn AS
  WITH RECURSIVE conn(node) AS (
      SELECT 'r1'
    UNION
      SELECT e.b FROM coreview e JOIN conn c ON e.a = c.node
  )
  SELECT node, round(probability_evaluate(provenance())::numeric,6) AS reliability
  FROM conn WHERE node <> 'r1';
SELECT remove_provenance('cs7_conn');
SELECT node, reliability FROM cs7_conn ORDER BY node;
DROP TABLE cs7_conn;
SET provsql.provenance = 'semiring';

DROP TABLE bid, expertise, topic_of, extends, coreview, assignment,
           reviewers, papers, topics, extends_label,
           lead_chair, urgent_sub, prescreen, score_pass, flag_pass CASCADE;
