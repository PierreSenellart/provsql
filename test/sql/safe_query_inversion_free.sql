\set ECHO none
\pset format unaligned

-- Inversion-free UCQ(OBDD) detector.
--
-- The detector is a sibling pass to the read-once rewriter: it recognises the
-- inversion-free, tuple-independent self-join class (the consistent-unification
-- self-joins the read-once path bails on), builds the SafeCert order recipe,
-- emits a diagnostic NOTICE, and attaches a transparent certificate plus
-- per-input order markers to the lineage (read back at probability evaluation;
-- the lineage itself is left intact).
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
SET provsql.provenance = 'boolean';

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
SET provsql.provenance = 'semiring';
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

-- (6a) Aggregating view: a GROUP BY / aggregate inside the view turns its
--      provenance into a semiring sum over groups, outside the monotone-DNF
--      model.  The flattener refuses it (hasAggs / groupClause), so the detector
--      still sees an RTE_SUBQUERY and declines; the lineage evaluates correctly
--      through the normal chain.  (Plain SPJ views ARE flattened and certify --
--      see section 9.)
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
CREATE VIEW ifr_vbv AS SELECT x, count(*) AS c FROM ifr_vb GROUP BY x;
CREATE TEMP TABLE ifr_vt AS
  SELECT a.x AS x, provenance() AS p
    FROM ifr_va a, ifr_vbv b WHERE a.x = b.x GROUP BY a.x;
SELECT remove_provenance('ifr_vt');
SELECT x, (get_gate_type(p) <> 'annotation') AS declined,
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
SET provsql.provenance = 'boolean';
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
SET provsql.provenance = 'semiring';
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

-- (8) Deterministic (non-tracked) relations.  A relation with no provsql column
--     and no metadata contributes only probability-1 tuples and anchors no
--     provenance variable: the detector *erases* it before the root /
--     positional / precedence / marker passes (it still filters the cross
--     product through its join equalities).  Erasing only removes precedence
--     edges, so it enlarges the certified class soundly.  boolean_provenance is
--     off here (from 7b), so the explicit 'inversion-free' method compiles the
--     structured d-DNNF and the read-once rewriter does not pre-empt the NOTICE.

-- (8a) self-join-free q(x) :- A(x), B(x) with a deterministic filter D(x).  D
--      excludes x=3 but adds no variable; the query still certifies (root x
--      touches both *tracked* atoms) and matches possible worlds (0.5*0.4).
CREATE TABLE ifd_a(x int); INSERT INTO ifd_a VALUES (1),(2),(3);
SELECT add_provenance('ifd_a');
CREATE TABLE ifd_b(x int); INSERT INTO ifd_b VALUES (1),(2),(3);
SELECT add_provenance('ifd_b');
CREATE TABLE ifd_d(x int); INSERT INTO ifd_d VALUES (1),(2);   -- deterministic
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifd_a;
  PERFORM set_prob(provsql, 0.4) FROM ifd_b;
END $$;
CREATE TEMP TABLE ifd_t AS
  SELECT a.x AS x, provenance() AS p
    FROM ifd_a a, ifd_b b, ifd_d d
   WHERE a.x = b.x AND a.x = d.x GROUP BY a.x;
SELECT remove_provenance('ifd_t');
SELECT count(*) AS ifd_rows FROM ifd_t;
SELECT x, round(probability_evaluate(p, 'inversion-free')::numeric, 6)  AS ifd_if,
          round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS ifd_pw
  FROM ifd_t ORDER BY x;

-- (8b) the deterministic atom anchors the *secondary* class: q(x) :- A(x),
--      B(x,y), D(y).  Were D tracked, neither class {x} (in A,B) nor {y} (in
--      B,D) would touch all three atoms -> no root -> reject (this is h0, the
--      canonical non-hierarchical query).  Erasing deterministic D leaves {x}
--      touching both tracked atoms, so it certifies; value matches possible
--      worlds (x=1 has two y-derivations sharing nothing: 0.5*(1-0.6^2)=0.32).
CREATE TABLE ifd2_a(x int);        INSERT INTO ifd2_a VALUES (1),(2);
SELECT add_provenance('ifd2_a');
CREATE TABLE ifd2_b(x int, y int); INSERT INTO ifd2_b VALUES (1,10),(1,20),(2,30);
SELECT add_provenance('ifd2_b');
CREATE TABLE ifd2_d(y int);        INSERT INTO ifd2_d VALUES (10),(20),(30); -- deterministic
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifd2_a;
  PERFORM set_prob(provsql, 0.4) FROM ifd2_b;
END $$;
CREATE TEMP TABLE ifd2_t AS
  SELECT a.x AS x, provenance() AS p
    FROM ifd2_a a, ifd2_b b, ifd2_d d
   WHERE a.x = b.x AND b.y = d.y GROUP BY a.x;
SELECT remove_provenance('ifd2_t');
SELECT x, round(probability_evaluate(p, 'inversion-free')::numeric, 6)  AS ifd2_if,
          round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS ifd2_pw
  FROM ifd2_t ORDER BY x;

-- (8c) near-miss: the same shape but the third relation is provenance-*tracked*.
--      Erasure must not over-fire -- a tracked atom is never erased, so the
--      genuine non-hierarchy (h0) is still caught: the detector rejects and the
--      explicit 'inversion-free' method declines, while the default chain
--      evaluates the block soundly through the fallback (matches possible
--      worlds: x=1: 0.5*(1-(1-0.4*0.3)^2)=0.5*0.2256=0.1128; x=2: 0.5*0.4*0.3).
CREATE TABLE ifd3_a(x int);        INSERT INTO ifd3_a VALUES (1),(2);
SELECT add_provenance('ifd3_a');
CREATE TABLE ifd3_b(x int, y int); INSERT INTO ifd3_b VALUES (1,10),(1,20),(2,30);
SELECT add_provenance('ifd3_b');
CREATE TABLE ifd3_d(y int);        INSERT INTO ifd3_d VALUES (10),(20),(30);
SELECT add_provenance('ifd3_d');   -- tracked: creates the inversion
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifd3_a;
  PERFORM set_prob(provsql, 0.4) FROM ifd3_b;
  PERFORM set_prob(provsql, 0.3) FROM ifd3_d;
END $$;
CREATE TEMP TABLE ifd3_t AS
  SELECT a.x AS x, provenance() AS p
    FROM ifd3_a a, ifd3_b b, ifd3_d d
   WHERE a.x = b.x AND b.y = d.y GROUP BY a.x;
SELECT remove_provenance('ifd3_t');
DO $$
DECLARE tok uuid; declined boolean := false;
BEGIN
  SELECT p INTO tok FROM ifd3_t WHERE x = 1;
  BEGIN PERFORM probability_evaluate(tok, 'inversion-free');
  EXCEPTION WHEN OTHERS THEN
    declined := (SQLERRM ~ 'requires an inversion-free certificate');
  END;
  IF NOT declined THEN
    RAISE EXCEPTION 'tracked third relation (h0) should have declined inversion-free';
  END IF;
END $$;
SELECT x, round(probability_evaluate(p)::numeric, 6)                    AS ifd3_default,
          round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS ifd3_pw
  FROM ifd3_t ORDER BY x;

-- (9) Single-base SPJ views / subqueries.  Inversion-freeness is a property of
--     the *flattened* query: the detector runs on a copy where each single-base
--     SPJ subquery/view is inlined to its base relation in place, so the markers
--     map straight back and are threaded into each subquery's recursive rewrite
--     (transparent annotations on the base inputs, so query results and the
--     circuit are unchanged).  boolean_provenance is off here.

-- (9a) view + base, hierarchical self-join-free: q(x) :- Av(x), B(x) where Av is
--      the projection view SELECT x FROM A.  Certifies and matches pw (0.5*0.4).
CREATE TABLE ifv_a(x int); INSERT INTO ifv_a VALUES (1),(2),(3);
SELECT add_provenance('ifv_a');
CREATE TABLE ifv_b(x int); INSERT INTO ifv_b VALUES (1),(2),(3);
SELECT add_provenance('ifv_b');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifv_a;
  PERFORM set_prob(provsql, 0.4) FROM ifv_b;
END $$;
CREATE VIEW ifv_av AS SELECT x FROM ifv_a;
CREATE TEMP TABLE ifv_t1 AS
  SELECT a.x AS x, provenance() AS p
    FROM ifv_av a, ifv_b b WHERE a.x = b.x GROUP BY a.x;
SELECT remove_provenance('ifv_t1');
SELECT x, round(probability_evaluate(p, 'inversion-free')::numeric, 6)  AS t1_if,
          round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS t1_pw
  FROM ifv_t1 ORDER BY x;

-- (9b) the view referenced *twice* -> a structured self-join through the view,
--      which inlines to a self-join on the base A.  The read-once path rejects
--      self-joins; inversion-free certifies it.  A(x) AND A(x) collapses
--      idempotently, so the probability is P(A(x)) = 0.5.
CREATE TEMP TABLE ifv_t2 AS
  SELECT v1.x AS x, provenance() AS p
    FROM ifv_av v1, ifv_av v2 WHERE v1.x = v2.x GROUP BY v1.x;
SELECT remove_provenance('ifv_t2');
SELECT x, round(probability_evaluate(p, 'inversion-free')::numeric, 6)  AS t2_if,
          round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS t2_pw
  FROM ifv_t2 ORDER BY x;

-- (9c) selection view (a WHERE inside the view): SELECT x FROM B WHERE x <= 2.
--      The view's selection is pulled up into the flattened conjunction, so x=3
--      is filtered; the certified shape matches pw on the surviving groups.
CREATE VIEW ifv_bv AS SELECT x FROM ifv_b WHERE x <= 2;
CREATE TEMP TABLE ifv_t3 AS
  SELECT a.x AS x, provenance() AS p
    FROM ifv_a a, ifv_bv b WHERE a.x = b.x GROUP BY a.x;
SELECT remove_provenance('ifv_t3');
SELECT x, round(probability_evaluate(p, 'inversion-free')::numeric, 6)  AS t3_if,
          round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS t3_pw
  FROM ifv_t3 ORDER BY x;

-- (9d) same structured self-join through the view, but with boolean_provenance
--      ON: the parent is not read-once (self-join), so it still reaches the
--      inversion-free analysis; each inlined subquery is rewritten with the
--      parent-supplied markers (the read-once rewrite is skipped for them so the
--      transparent markers are not bypassed).  Default chain matches pw.
SET provsql.provenance = 'boolean';
CREATE TEMP TABLE ifv_t4 AS
  SELECT v1.x AS x, provenance() AS p
    FROM ifv_av v1, ifv_av v2 WHERE v1.x = v2.x GROUP BY v1.x;
SELECT remove_provenance('ifv_t4');
SELECT x, round(probability_evaluate(p)::numeric, 6)                    AS t4_default,
          round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS t4_pw
  FROM ifv_t4 ORDER BY x;
SET provsql.provenance = 'semiring';

-- (10) Multi-relation SPJ views (a join *inside* the view).  The flattener
--      expands one subquery slot into all its base atoms, pulling the view's
--      join condition up into the flat conjunction; markers are threaded to each
--      base input.  boolean_provenance is off.

-- (10a) view joins A(x,k), B(k,z) on the internal key k and projects x: the
--       flattened conjunction A(x,k), B(k,z) has root k (touches both atoms).
--       Certifies and matches pw (0.5 * 0.4).
CREATE TABLE ifm_a(x int, k int); INSERT INTO ifm_a VALUES (1,10),(2,20);
SELECT add_provenance('ifm_a');
CREATE TABLE ifm_b(k int, z int); INSERT INTO ifm_b VALUES (10,100),(20,200);
SELECT add_provenance('ifm_b');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifm_a;
  PERFORM set_prob(provsql, 0.4) FROM ifm_b;
END $$;
CREATE VIEW ifm_v AS SELECT a.x AS x, b.z AS z FROM ifm_a a, ifm_b b WHERE a.k = b.k;
CREATE TEMP TABLE ifm_t1 AS
  SELECT v.x AS x, provenance() AS p FROM ifm_v v GROUP BY v.x;
SELECT remove_provenance('ifm_t1');
SELECT x, round(probability_evaluate(p, 'inversion-free')::numeric, 6)  AS t1_if,
          round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS t1_pw
  FROM ifm_t1 ORDER BY x;

-- (10b) a join-on-x view V(x) := A(x), B(x) referenced *twice* -> a structured
--       self-join through a multi-relation view, flattening to A,B,A,B all
--       sharing the root x.  Certifies; A(x)&B(x) repeated collapses
--       idempotently, so the value is P(A(x) & B(x)) = 0.5*0.4 = 0.2.
CREATE TABLE ifm2_a(x int); INSERT INTO ifm2_a VALUES (1),(2),(3);
SELECT add_provenance('ifm2_a');
CREATE TABLE ifm2_b(x int); INSERT INTO ifm2_b VALUES (1),(2),(3);
SELECT add_provenance('ifm2_b');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifm2_a;
  PERFORM set_prob(provsql, 0.4) FROM ifm2_b;
END $$;
CREATE VIEW ifm2_v AS SELECT a.x AS x FROM ifm2_a a, ifm2_b b WHERE a.x = b.x;
CREATE TEMP TABLE ifm2_t AS
  SELECT v1.x AS x, provenance() AS p
    FROM ifm2_v v1, ifm2_v v2 WHERE v1.x = v2.x GROUP BY v1.x;
SELECT remove_provenance('ifm2_t');
SELECT x, round(probability_evaluate(p, 'inversion-free')::numeric, 6)  AS t2_if,
          round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS t2_pw
  FROM ifm2_t ORDER BY x;

-- (11) Nested subqueries / views (a subquery whose FROM contains another).  The
--      flattener recurses, collapsing views-over-views to base atoms before
--      inlining, and composes the slot path so markers reach the base input
--      through the nested rewrite.  boolean_provenance is off.
CREATE TABLE ifn_a(x int); INSERT INTO ifn_a VALUES (1),(2),(3);
SELECT add_provenance('ifn_a');
CREATE TABLE ifn_b(x int); INSERT INTO ifn_b VALUES (1),(2),(3);
SELECT add_provenance('ifn_b');
CREATE TABLE ifn_c(x int); INSERT INTO ifn_c VALUES (1),(2),(3);
SELECT add_provenance('ifn_c');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifn_a;
  PERFORM set_prob(provsql, 0.4) FROM ifn_b;
  PERFORM set_prob(provsql, 0.3) FROM ifn_c;
END $$;

-- (11a) view over a view (two levels) joined with a base: V2 := SELECT x FROM
--       V1, V1 := SELECT x FROM A.  Flattens through both levels to A; joined
--       with B on x.  Matches pw (0.5 * 0.4).
CREATE VIEW ifn_v1 AS SELECT x FROM ifn_a;
CREATE VIEW ifn_v2 AS SELECT x FROM ifn_v1;
CREATE TEMP TABLE ifn_t1 AS
  SELECT v.x AS x, provenance() AS p
    FROM ifn_v2 v, ifn_b b WHERE v.x = b.x GROUP BY v.x;
SELECT remove_provenance('ifn_t1');
SELECT x, round(probability_evaluate(p, 'inversion-free')::numeric, 6)  AS t1_if,
          round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS t1_pw
  FROM ifn_t1 ORDER BY x;

-- (11b) a *multi-relation* inner view nested inside an outer view: the path
--       composes through a multi-base level.  Inner V := A(x) JOIN B(x) on x,
--       outer wraps it, joined with C on x -> flat A(x),B(x),C(x) sharing root
--       x.  Matches pw (0.5 * 0.4 * 0.3).
CREATE VIEW ifn_in AS SELECT a.x AS x FROM ifn_a a, ifn_b b WHERE a.x = b.x;
CREATE VIEW ifn_out AS SELECT x FROM ifn_in;
CREATE TEMP TABLE ifn_t2 AS
  SELECT o.x AS x, provenance() AS p
    FROM ifn_out o, ifn_c c WHERE o.x = c.x GROUP BY o.x;
SELECT remove_provenance('ifn_t2');
SELECT x, round(probability_evaluate(p, 'inversion-free')::numeric, 6)  AS t2_if,
          round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS t2_pw
  FROM ifn_t2 ORDER BY x;

-- (11c) inline nested derived tables (no views): FROM (SELECT FROM (SELECT FROM A)).
CREATE TEMP TABLE ifn_t3 AS
  SELECT s2.x AS x, provenance() AS p
    FROM (SELECT x FROM (SELECT x FROM ifn_a) s1) s2, ifn_b b
   WHERE s2.x = b.x GROUP BY s2.x;
SELECT remove_provenance('ifn_t3');
SELECT x, round(probability_evaluate(p, 'inversion-free')::numeric, 6)  AS t3_if,
          round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS t3_pw
  FROM ifn_t3 ORDER BY x;

-- (12) Top-level UNION ALL of inversion-free branches.  UNION ALL is bag
--      semantics (Jha & Suciu's UCQ(OBDD) result is set semantics): each output
--      row's provenance is a *single* branch's conjunction, no cross-branch OR.
--      So each arm certifies its own inversion-free root and the union carries
--      that annotated token verbatim.  Branch 1 is the 4-atom self-join witness
--      (only x=1 qualifies, 0.5^4); branch 2 a 2-atom read-once join A(x),B(x)
--      (x=1 and x=2, 0.5^2 each).  Expect one acceptance NOTICE per arm, and
--      per-row 'inversion-free' == 'possible-worlds'.
CREATE TEMP TABLE ifr_ua AS
  SELECT s1.x AS x, provenance() AS p
    FROM ifr_s s1, ifr_a a, ifr_s s2, ifr_b b
   WHERE s1.x = a.x AND s1.c2 = a.c2 AND s1.x = s2.x
     AND s2.x = b.x AND s2.c2 = b.c2
  UNION ALL
  SELECT a.x AS x, provenance() AS p
    FROM ifr_a a, ifr_b b
   WHERE a.x = b.x;
SELECT remove_provenance('ifr_ua');
SELECT x, round(probability_evaluate(p, 'inversion-free')::numeric, 6)  AS ua_if,
          round(probability_evaluate(p, 'possible-worlds')::numeric, 6) AS ua_pw
  FROM ifr_ua ORDER BY x, ua_if;

-- (13) UNION (set semantics) of two BRANCH-DISJOINT inversion-free witnesses.
--      Jha & Suciu's UCQ(OBDD) tractability is a *set*-semantics result: the
--      per-tuple lineage is the OR of the branch lineages.  A deduplicating
--      UNION lowers to a GROUP BY over UNION ALL whose per-group root is the
--      provenance_plus OR; here that lowered shape is exercised directly
--      (GROUP BY x over an inner UNION ALL).  With disjoint branch relations the
--      OR decomposes (orDecompose), the certificate lands on the plus root, and
--      the structured d-DNNF stays polynomial.  x=1 comes from both branches
--      (each 0.5^4 = 0.0625) -> 1-(1-0.0625)^2 = 0.12109375; x=2 from the second
--      branch only -> 0.0625.  Expect one acceptance NOTICE per arm;
--      inversion-free == possible-worlds.
CREATE TABLE ifu_s(x int, c2 int);
INSERT INTO ifu_s VALUES (1,10),(1,20),(2,30),(2,40);
SELECT add_provenance('ifu_s');
CREATE TABLE ifu_a(x int, c2 int);
INSERT INTO ifu_a VALUES (1,10),(2,30);
SELECT add_provenance('ifu_a');
CREATE TABLE ifu_b(x int, c2 int);
INSERT INTO ifu_b VALUES (1,20),(2,40);
SELECT add_provenance('ifu_b');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ifu_s;
  PERFORM set_prob(provsql, 0.5) FROM ifu_a;
  PERFORM set_prob(provsql, 0.5) FROM ifu_b;
END $$;
CREATE TEMP TABLE ifr_union AS
  SELECT x, provenance() AS p FROM (
    SELECT s1.x AS x FROM ifr_s s1, ifr_a a, ifr_s s2, ifr_b b
     WHERE s1.x = a.x AND s1.c2 = a.c2 AND s1.x = s2.x
       AND s2.x = b.x AND s2.c2 = b.c2
    UNION ALL
    SELECT s1.x AS x FROM ifu_s s1, ifu_a a, ifu_s s2, ifu_b b
     WHERE s1.x = a.x AND s1.c2 = a.c2 AND s1.x = s2.x
       AND s2.x = b.x AND s2.c2 = b.c2
  ) q GROUP BY x;
SELECT remove_provenance('ifr_union');
SELECT x, round(probability_evaluate(p, 'inversion-free')::numeric, 8)  AS u_if,
          round(probability_evaluate(p, 'possible-worlds')::numeric, 8) AS u_pw
  FROM ifr_union ORDER BY x;

RESET provsql.provenance;
RESET provsql.verbose_level;
