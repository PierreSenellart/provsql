\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Disjoint-constant self-joins.
--
-- Two (or more) RTEs over the same relation, each carrying a
-- @c Var @c = @c Const conjunct on the @em same column with
-- provably distinct literal values, range over disjoint tuple-sets
-- of the underlying relation: a single base row can satisfy at most
-- one of the constant predicates, so their @c provsql tokens never
-- overlap.  The disjoint-constant pre-pass certifies such groups;
-- the candidate gate's shared-relid bail then skips them, and the
-- standard per-atom DISTINCT wrap (with the constant predicate
-- pushed in) produces a read-once circuit -- the disjoint partition
-- guarantees each token appears in at most one wrap.
--
-- The canonical motivating shape:
--
--   q :- R(x, kind) r1, R r2  WHERE r1.kind = 'A' AND r2.kind = 'B'
--                               AND r1.x = r2.x.

-- ---------------------------------------------------------------------
-- (1) The motivating example.  Two RTEs of @c R distinguished by
--     mutually exclusive @c kind values; their tuple-sets are
--     disjoint, the rewrite emits each as its own DISTINCT wrap.
-- ---------------------------------------------------------------------
CREATE TABLE sjd_r (x int, kind char(1));
INSERT INTO sjd_r VALUES (1, 'A'), (2, 'A'), (1, 'B'), (3, 'B');

SELECT add_provenance('sjd_r');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM sjd_r; END $$;

-- Baseline: probability via the default evaluator on the unrewritten
-- circuit.  @c r1 and @c r2 reference disjoint partitions of @c R,
-- so the standard rewrite already produces a read-once circuit (no
-- shared inputs across @c gate_times); the default evaluator
-- returns the exact value.
SET provsql.provenance = 'semiring';
CREATE TEMP TABLE sjd_baseline AS
  SELECT q.x, probability_evaluate(provenance()) AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sjd_r r1, sjd_r r2
           WHERE r1.kind = 'A' AND r2.kind = 'B'
             AND r1.x = r2.x) q
   GROUP BY q.x;
SELECT remove_provenance('sjd_baseline');
SELECT x, ROUND(p::numeric, 6) AS prob_baseline FROM sjd_baseline;

-- Disjoint-constant path: 'independent' on the rewritten circuit
-- must match.
SET provsql.provenance = 'boolean';
CREATE TEMP TABLE sjd_rewritten AS
  SELECT q.x, probability_evaluate(provenance(), 'independent') AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sjd_r r1, sjd_r r2
           WHERE r1.kind = 'A' AND r2.kind = 'B'
             AND r1.x = r2.x) q
   GROUP BY q.x;
SELECT remove_provenance('sjd_rewritten');
SELECT x, ROUND(p::numeric, 6) AS prob_rewritten FROM sjd_rewritten;

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM sjd_baseline b JOIN sjd_rewritten r ON b.x = r.x;

-- ---------------------------------------------------------------------
-- (2) Three-RTE group with pairwise-distinct literals on @c kind:
--     every pair (@c r1, @c r2), (@c r1, @c r3), (@c r2, @c r3)
--     must satisfy the disjointness check.  The rewrite emits each
--     RTE as an independent wrap.
-- ---------------------------------------------------------------------
SET provsql.provenance = 'boolean';
CREATE TEMP TABLE sjd_three AS
  SELECT q.x, probability_evaluate(provenance(), 'independent') AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sjd_r r1, sjd_r r2, sjd_r r3
           WHERE r1.kind = 'A' AND r2.kind = 'B' AND r3.kind = 'C'
             AND r1.x = r2.x AND r2.x = r3.x) q
   GROUP BY q.x;
SELECT remove_provenance('sjd_three');
-- (no rows in sjd_r have kind = 'C', so probability = 0; just verify
-- the rewrite doesn't bail under a 3-RTE pairwise-disjoint group.)
SELECT x, ROUND(p::numeric, 6) AS prob_three FROM sjd_three;

-- ---------------------------------------------------------------------
-- (3) Same constant on both sides: not disjoint, the
--     disjoint-constant pass must not certify, the rewrite must
--     fall back to the generic provenance path.  The probability
--     under @c boolean_provenance = on must still match the default
--     evaluator (i.e. nothing else broke).
--
-- ---------------------------------------------------------------------
SET provsql.provenance = 'semiring';
CREATE TEMP TABLE sjd_same_baseline AS
  SELECT q.x, probability_evaluate(provenance()) AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sjd_r r1, sjd_r r2
           WHERE r1.kind = 'A' AND r2.kind = 'A'
             AND r1.x = r2.x) q
   GROUP BY q.x;
SELECT remove_provenance('sjd_same_baseline');
SET provsql.provenance = 'boolean';
CREATE TEMP TABLE sjd_same_rewritten AS
  SELECT q.x, probability_evaluate(provenance()) AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sjd_r r1, sjd_r r2
           WHERE r1.kind = 'A' AND r2.kind = 'A'
             AND r1.x = r2.x) q
   GROUP BY q.x;
SELECT remove_provenance('sjd_same_rewritten');
SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_same_baseline_vs_rewritten
  FROM sjd_same_baseline b JOIN sjd_same_rewritten r ON b.x = r.x;

-- ---------------------------------------------------------------------
-- (4) Constants on different columns: an R-tuple can satisfy both
--     predicates simultaneously, so the disjoint-constant pass must
--     not certify.  Probability under both GUC modes must match the
--     default evaluator.
-- ---------------------------------------------------------------------
CREATE TABLE sjd_r2 (x int, kind char(1), y int);
INSERT INTO sjd_r2 VALUES (1, 'A', 10), (2, 'A', 20), (1, 'B', 30);
SELECT add_provenance('sjd_r2');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM sjd_r2; END $$;

SET provsql.provenance = 'semiring';
CREATE TEMP TABLE sjd_diffcol_baseline AS
  SELECT q.x, probability_evaluate(provenance()) AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sjd_r2 r1, sjd_r2 r2
           WHERE r1.kind = 'A' AND r2.y = 10
             AND r1.x = r2.x) q
   GROUP BY q.x;
SELECT remove_provenance('sjd_diffcol_baseline');

SET provsql.provenance = 'boolean';
CREATE TEMP TABLE sjd_diffcol_rewritten AS
  SELECT q.x, probability_evaluate(provenance()) AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sjd_r2 r1, sjd_r2 r2
           WHERE r1.kind = 'A' AND r2.y = 10
             AND r1.x = r2.x) q
   GROUP BY q.x;
SELECT remove_provenance('sjd_diffcol_rewritten');

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_diffcol_baseline_vs_rewritten
  FROM sjd_diffcol_baseline b JOIN sjd_diffcol_rewritten r ON b.x = r.x;

DROP TABLE sjd_r, sjd_r2 CASCADE;
