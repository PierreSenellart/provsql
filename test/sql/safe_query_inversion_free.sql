\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Inversion-free UCQ(OBDD) detector (phase 1).
--
-- The detector is a sibling pass to the read-once rewriter: it recognises the
-- inversion-free, tuple-independent self-join class (the consistent-unification
-- self-joins the read-once path bails on), builds the SafeCert order recipe,
-- and -- in phase 1 -- only emits a diagnostic NOTICE without changing query
-- evaluation (certificate produced but unused).
--
-- The lineage must be built by the real planner (real tables + the SQL query),
-- never a hand-factored circuit: a hand-factored circuit is low-treewidth and
-- would silently skip the hard case.
--
-- Detection fires while the CREATE TEMP TABLE AS planner-rewrites its SELECT;
-- the temp table's provsql column carries non-deterministic input-token UUIDs,
-- so we never display it -- we drop provenance and assert the row count instead
-- (cardinality is preserved because the lineage is left intact).

SET provsql.verbose_level = 5;     -- acceptance NOTICE (>=1) + rejection reasons (>=5)
SET provsql.boolean_provenance = on;

-- TID base relations.  Column c2 is the "second attribute": it plays y in the
-- S(x,y),A(x,y) atoms and z in the S(x,z),B(x,z) atoms.
CREATE TABLE ifr_s(x int, c2 int);
INSERT INTO ifr_s VALUES (1,10),(1,20),(2,10);
SELECT add_provenance('ifr_s');
CREATE TABLE ifr_a(x int, c2 int);
INSERT INTO ifr_a VALUES (1,10),(2,99);
SELECT add_provenance('ifr_a');
CREATE TABLE ifr_b(x int, c2 int);
INSERT INTO ifr_b VALUES (1,20),(2,10);
SELECT add_provenance('ifr_b');

DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifr_s;
  PERFORM set_prob(provsql, 0.5) FROM ifr_a;
  PERFORM set_prob(provsql, 0.5) FROM ifr_b;
END $$;

-- (1) Witness: S(x,y),A(x,y),S(x,z),B(x,z) -- self-join on S through root x.
--     Expect a NOTICE that the certificate was attached (planner side).  The
--     certificate then round-trips to the evaluator: probability_evaluate reads
--     it back from the annotated root (a second NOTICE) and -- because the
--     annotation is transparent -- still returns the correct probability
--     (one derivation of four independent p=0.5 inputs => 0.5^4 = 0.0625).
--     Cardinality is preserved (only x=1 qualifies -> 1 row).
-- Capture the per-row provenance token (the annotated root) into an explicit
-- column p, then drop provenance tracking so the probability read below is not
-- itself rewritten (no non-deterministic provsql column on its output).
CREATE TEMP TABLE ifr_witness AS
  SELECT s1.x AS x, provenance() AS p
    FROM ifr_s s1, ifr_a a, ifr_s s2, ifr_b b
   WHERE s1.x = a.x AND s1.c2 = a.c2     -- S(x,y), A(x,y)
     AND s1.x = s2.x                     -- both S occurrences share the root x
     AND s2.x = b.x AND s2.c2 = b.c2;    -- S(x,z), B(x,z)
SELECT remove_provenance('ifr_witness');
SELECT round(probability_evaluate(p)::numeric, 6) AS witness_prob
  FROM ifr_witness ORDER BY 1;
SELECT count(*) AS witness_rows FROM ifr_witness;

-- (2) Rejection -- symmetric closure R(x,y),R(y,x): the root class sits at two
--     different column positions across the two R occurrences.  Expect NO
--     certificate; a rejection NOTICE at verbose>=5.  Lineage intact (3 rows).
CREATE TABLE ifr_r(a int, b int);
INSERT INTO ifr_r VALUES (1,2),(2,1),(3,3);
SELECT add_provenance('ifr_r');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM ifr_r; END $$;

CREATE TEMP TABLE ifr_sym AS
  SELECT r1.a AS a
    FROM ifr_r r1, ifr_r r2
   WHERE r1.a = r2.b AND r1.b = r2.a;
SELECT remove_provenance('ifr_sym');
SELECT count(*) AS sym_rows FROM ifr_sym;

-- (3) Rejection -- path R(x,y),R(y,z): the shared class y is at column 2 in one
--     occurrence and column 1 in the other (positional inconsistency).
CREATE TEMP TABLE ifr_path AS
  SELECT r1.a AS a
    FROM ifr_r r1, ifr_r r2
   WHERE r1.b = r2.a;
SELECT remove_provenance('ifr_path');
SELECT count(*) AS path_rows FROM ifr_path;

-- (4) Live path correctness on a NON-read-once witness.  Richer data gives the
--     root x=1 several (y,z) derivations; GROUP BY x OR-aggregates them into one
--     provenance token, the inversion-free block
--       (S(1,10)A(1,10) v S(1,20)A(1,20)) ^ (S(1,20)B(1,20) v S(1,30)B(1,30))
--     which shares S(1,20), so it is NOT read-once: independentEvaluation
--     rejects and the default chain takes the inversion-free structured-d-DNNF
--     rung (markers attached on the certified path, read back at eval).  Its
--     value must equal exact possible_worlds.  Distinct per-tuple probabilities
--     make the agreement a real check (hand value 0.2203).
CREATE TABLE ifr_s2(x int, c2 int);
INSERT INTO ifr_s2 VALUES (1,10),(1,20),(1,30);
SELECT add_provenance('ifr_s2');
CREATE TABLE ifr_a2(x int, c2 int);
INSERT INTO ifr_a2 VALUES (1,10),(1,20);
SELECT add_provenance('ifr_a2');
CREATE TABLE ifr_b2(x int, c2 int);
INSERT INTO ifr_b2 VALUES (1,20),(1,30);
SELECT add_provenance('ifr_b2');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifr_s2;
  PERFORM set_prob(provsql, 0.4) FROM ifr_a2 WHERE c2 = 10;
  PERFORM set_prob(provsql, 0.6) FROM ifr_a2 WHERE c2 = 20;
  PERFORM set_prob(provsql, 0.3) FROM ifr_b2 WHERE c2 = 20;
  PERFORM set_prob(provsql, 0.7) FROM ifr_b2 WHERE c2 = 30;
END $$;

CREATE TEMP TABLE ifr_block AS
  SELECT s1.x AS x, provenance() AS p
    FROM ifr_s2 s1, ifr_a2 a, ifr_s2 s2, ifr_b2 b
   WHERE s1.x = a.x AND s1.c2 = a.c2
     AND s1.x = s2.x
     AND s2.x = b.x AND s2.c2 = b.c2
   GROUP BY s1.x;
SELECT remove_provenance('ifr_block');
SELECT count(*) AS block_rows FROM ifr_block;
-- default chain (inversion-free rung) vs explicit method vs exact oracle: equal.
SELECT round(probability_evaluate(p)::numeric, 8)                   AS block_default,
       round(probability_evaluate(p, 'inversion-free')::numeric, 8) AS block_if,
       round(probability_evaluate(p, 'possible-worlds')::numeric, 8) AS block_pw
  FROM ifr_block;

-- (5) Self-join-free hierarchical query q(x) :- A(x), B(x).  With no self-join
--     the inversion-free class coincides with the read-once class, but the
--     analysis is not gated on self-joins: it still certifies the query and
--     attaches per-input order markers.  With boolean_provenance OFF (so the
--     read-once rewriter does not pre-empt it) the explicit 'inversion-free'
--     method compiles the structured d-DNNF over those markers; its value
--     matches the default chain (here independentEvaluation already suffices)
--     and exact possible worlds (0.5*0.4 = 0.2 at each x).
SET provsql.boolean_provenance = off;
CREATE TABLE ifr_sjf_a(x int);
INSERT INTO ifr_sjf_a VALUES (1),(2),(3);
SELECT add_provenance('ifr_sjf_a');
CREATE TABLE ifr_sjf_b(x int);
INSERT INTO ifr_sjf_b VALUES (1),(2),(3);
SELECT add_provenance('ifr_sjf_b');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifr_sjf_a;
  PERFORM set_prob(provsql, 0.4) FROM ifr_sjf_b;
END $$;
CREATE TEMP TABLE ifr_sjf AS
  SELECT a.x AS x, provenance() AS p
    FROM ifr_sjf_a a, ifr_sjf_b b
   WHERE a.x = b.x
   GROUP BY a.x;
SELECT remove_provenance('ifr_sjf');
SELECT count(*) AS sjf_rows FROM ifr_sjf;
SELECT x,
       round(probability_evaluate(p)::numeric, 6)                    AS sjf_default,
       round(probability_evaluate(p, 'inversion-free')::numeric, 6)  AS sjf_if,
       round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS sjf_pw
  FROM ifr_sjf ORDER BY x;

-- (6) Decline / robustness cases: the analysis must keep clear of atoms it does
--     not model, and the evaluator must treat a malformed certificate as inert.
--     In each the lineage still evaluates correctly through the normal chain.

-- (6a) Derived (view) atom: a view is an RTE_SUBQUERY, not an RTE_RELATION, so
--      the detector declines (root is not a certificate-carrying annotation) and
--      the read-once lineage is evaluated as usual.
CREATE TABLE ifr_va(x int);
INSERT INTO ifr_va VALUES (1),(2);
SELECT add_provenance('ifr_va');
CREATE TABLE ifr_vb(x int);
INSERT INTO ifr_vb VALUES (1),(2);
SELECT add_provenance('ifr_vb');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifr_va;
  PERFORM set_prob(provsql, 0.4) FROM ifr_vb;
END $$;
CREATE VIEW ifr_vbv AS SELECT x FROM ifr_vb;
CREATE TEMP TABLE ifr_vt AS
  SELECT a.x AS x, provenance() AS p
    FROM ifr_va a, ifr_vbv b WHERE a.x = b.x GROUP BY a.x;
SELECT remove_provenance('ifr_vt');
SELECT x, get_gate_type(p) AS root_type,
       round(probability_evaluate(p)::numeric, 6) AS prob
  FROM ifr_vt ORDER BY x;

-- (6b) BID atom: a repair_key relation contributes gate_mulinput leaves.  The
--      detector declines (no certificate), so the default chain evaluates the
--      block soundly (x=1: disjoint 0.3+0.4 = 0.7; x=2: 0.5) and the explicit
--      'inversion-free' method declines rather than mis-evaluating it.
CREATE TABLE ifr_bid(x int, v int, conf float);
INSERT INTO ifr_bid VALUES (1,100,0.3),(1,200,0.4),(2,300,0.5);
SELECT add_provenance('ifr_bid');
SELECT remove_provenance('ifr_bid');
SELECT repair_key('ifr_bid', 'x');
DO $$ BEGIN PERFORM set_prob(provsql, conf) FROM ifr_bid; END $$;
CREATE TEMP TABLE ifr_bt AS SELECT x, provenance() AS p FROM ifr_bid GROUP BY x;
SELECT remove_provenance('ifr_bt');
SELECT x, get_gate_type(p) AS root_type,
       round(probability_evaluate(p)::numeric, 6)                    AS prob_default,
       round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS prob_pw
  FROM ifr_bt ORDER BY x;
DO $$
DECLARE tok uuid; declined boolean := false;
BEGIN
  SELECT p INTO tok FROM ifr_bt WHERE x = 1;
  BEGIN PERFORM probability_evaluate(tok, 'inversion-free');
  EXCEPTION WHEN OTHERS THEN
    declined := (SQLERRM ~ 'requires an inversion-free certificate');
  END;
  IF NOT declined THEN
    RAISE EXCEPTION 'explicit inversion-free should have declined on a BID block';
  END IF;
END $$;
SELECT 'BID: explicit inversion-free declines; default sound' AS bid_check;

-- (6c) Malformed certificate: a stray 'C'-prefixed annotation that is not a
--      valid SafeCert recipe must be read as inert (transparent passthrough),
--      so evaluation falls back to the wrapped circuit and stays correct.  Build
--      the token with the analysis off so the only certificate present is the
--      garbage one we attach by hand.
SET provsql.inversion_free = off;
CREATE TABLE ifr_ma(x int);
INSERT INTO ifr_ma VALUES (1);
SELECT add_provenance('ifr_ma');
CREATE TABLE ifr_mb(x int);
INSERT INTO ifr_mb VALUES (1);
SELECT add_provenance('ifr_mb');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifr_ma;
  PERFORM set_prob(provsql, 0.4) FROM ifr_mb;
END $$;
CREATE TEMP TABLE ifr_mt AS
  SELECT a.x AS x, provenance() AS p
    FROM ifr_ma a, ifr_mb b WHERE a.x = b.x GROUP BY a.x;
SELECT remove_provenance('ifr_mt');
SET provsql.inversion_free = on;
SELECT x, round(probability_evaluate(p)::numeric, 6) AS prob_plain,
          round(probability_evaluate(
                  annotate(p, 'Cgarbage-not-a-recipe'))::numeric, 6) AS prob_badcert
  FROM ifr_mt ORDER BY x;

-- (7) Non-integer key columns.  The per-input order markers carry the root and
--     secondary class values as length-prefixed value text, so any scalar type
--     whose output function is injective works.  These shapes are structurally
--     identical to the integer-keyed ones above; only the key column types
--     change.  The certified path must fire (acceptance NOTICE) and the
--     structured-d-DNNF value must equal exact possible_worlds -- which proves
--     the text codec round-tripped the keys (a mis-parse would mis-group and
--     change the probability).

-- (7a) text root AND text secondary, on the self-join block of case (4).  One
--      root value contains a space, exercising the byte-length prefix that keeps
--      arbitrary text unambiguous.  Probabilities mirror case (4) (0.5 for each
--      S; A: 0.4/0.6; B: 0.3/0.7), so the block value is the same 0.2203.
SET provsql.boolean_provenance = on;
CREATE TABLE ifr_ts(x text, c2 text);
INSERT INTO ifr_ts VALUES ('grp one','a'),('grp one','b'),('grp one','c');
SELECT add_provenance('ifr_ts');
CREATE TABLE ifr_ta(x text, c2 text);
INSERT INTO ifr_ta VALUES ('grp one','a'),('grp one','b');
SELECT add_provenance('ifr_ta');
CREATE TABLE ifr_tb(x text, c2 text);
INSERT INTO ifr_tb VALUES ('grp one','b'),('grp one','c');
SELECT add_provenance('ifr_tb');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifr_ts;
  PERFORM set_prob(provsql, 0.4) FROM ifr_ta WHERE c2 = 'a';
  PERFORM set_prob(provsql, 0.6) FROM ifr_ta WHERE c2 = 'b';
  PERFORM set_prob(provsql, 0.3) FROM ifr_tb WHERE c2 = 'b';
  PERFORM set_prob(provsql, 0.7) FROM ifr_tb WHERE c2 = 'c';
END $$;
CREATE TEMP TABLE ifr_tblock AS
  SELECT s1.x AS x, provenance() AS p
    FROM ifr_ts s1, ifr_ta a, ifr_ts s2, ifr_tb b
   WHERE s1.x = a.x AND s1.c2 = a.c2
     AND s1.x = s2.x
     AND s2.x = b.x AND s2.c2 = b.c2
   GROUP BY s1.x;
SELECT remove_provenance('ifr_tblock');
SELECT count(*) AS tblock_rows FROM ifr_tblock;
SELECT round(probability_evaluate(p)::numeric, 8)                   AS tblock_default,
       round(probability_evaluate(p, 'inversion-free')::numeric, 8) AS tblock_if,
       round(probability_evaluate(p, 'possible-worlds')::numeric, 8) AS tblock_pw
  FROM ifr_tblock;

-- (7b) uuid root, self-join-free hierarchical q(x) :- A(x), B(x).  The join /
--      group key is a uuid; its canonical text rendering is the order key.
SET provsql.boolean_provenance = off;
CREATE TABLE ifr_ua(x uuid);
INSERT INTO ifr_ua VALUES
  ('11111111-1111-1111-1111-111111111111'),
  ('22222222-2222-2222-2222-222222222222');
SELECT add_provenance('ifr_ua');
CREATE TABLE ifr_ub(x uuid);
INSERT INTO ifr_ub VALUES
  ('11111111-1111-1111-1111-111111111111'),
  ('22222222-2222-2222-2222-222222222222');
SELECT add_provenance('ifr_ub');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifr_ua;
  PERFORM set_prob(provsql, 0.4) FROM ifr_ub;
END $$;
CREATE TEMP TABLE ifr_ut AS
  SELECT a.x AS x, provenance() AS p
    FROM ifr_ua a, ifr_ub b WHERE a.x = b.x GROUP BY a.x;
SELECT remove_provenance('ifr_ut');
SELECT count(*) AS ut_rows FROM ifr_ut;
SELECT round(probability_evaluate(p, 'inversion-free')::numeric, 6)  AS ut_if,
       round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS ut_pw
  FROM ifr_ut;

-- (7c) date root, same self-join-free shape; key text is 'YYYY-MM-DD'.
CREATE TABLE ifr_da(x date);
INSERT INTO ifr_da VALUES ('2026-01-01'),('2026-02-02');
SELECT add_provenance('ifr_da');
CREATE TABLE ifr_db(x date);
INSERT INTO ifr_db VALUES ('2026-01-01'),('2026-02-02');
SELECT add_provenance('ifr_db');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifr_da;
  PERFORM set_prob(provsql, 0.4) FROM ifr_db;
END $$;
CREATE TEMP TABLE ifr_dt AS
  SELECT a.x AS x, provenance() AS p
    FROM ifr_da a, ifr_db b WHERE a.x = b.x GROUP BY a.x;
SELECT remove_provenance('ifr_dt');
SELECT count(*) AS dt_rows FROM ifr_dt;
SELECT round(probability_evaluate(p, 'inversion-free')::numeric, 6)  AS dt_if,
       round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS dt_pw
  FROM ifr_dt;

RESET provsql.boolean_provenance;
RESET provsql.verbose_level;
