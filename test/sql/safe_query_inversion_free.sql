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
-- would silently skip the hard case (see doc/TODO/inversion-free.md).
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
--     Expect a NOTICE that the inversion-free certificate was produced, and
--     the row count to be unchanged (only x=1 qualifies -> 1 row).
CREATE TEMP TABLE ifr_witness AS
  SELECT s1.x AS x
    FROM ifr_s s1, ifr_a a, ifr_s s2, ifr_b b
   WHERE s1.x = a.x AND s1.c2 = a.c2     -- S(x,y), A(x,y)
     AND s1.x = s2.x                     -- both S occurrences share the root x
     AND s2.x = b.x AND s2.c2 = b.c2;    -- S(x,z), B(x,z)
SELECT remove_provenance('ifr_witness');
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

-- TODO (phase-1 follow-up, after the basics verify): BID-atom and derived/view
-- atom rejections (both are metadata-gate refusals -> no certificate, no
-- acceptance NOTICE), and a correctness cross-check of the witness probability
-- against monte_carlo / possible_worlds once the certificate is consumed
-- (phase 4).

RESET provsql.boolean_provenance;
RESET provsql.verbose_level;
