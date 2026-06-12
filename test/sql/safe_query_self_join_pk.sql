\set ECHO none
\pset format unaligned

-- PK-unifiable self-joins.
--
-- When two RTEs over the same relation have all PRIMARY KEY (or
-- NOT-NULL UNIQUE) columns equated through the union-find closure
-- of the residual equijoins, the key proves they refer to the same
-- tuple.  The PK-unification pre-pass collapses the duplicate RTEs
-- into a single survivor, renumbering @c Var.varno and
-- @c RangeTblRef.rtindex through the rewriter's compacted
-- @c rtable so the candidate gate's "no two RTEs may share a relid"
-- bail no longer fires.
--
-- The motivating shape:
--
--   q :- R(x PK, y, z) r1, R r2 WHERE r1.x = r2.x AND r1.y = c1
--                                 AND r2.z = c2.
--
-- After unification, the single-atom query @c R @c WHERE @c y @c =
-- @c c1 @c AND @c z @c = @c c2 already produces a read-once
-- circuit through the standard provenance rewrite (the @c
-- gate_times dedup folds the duplicated @c provsql Var pairs to a
-- single leaf).  The unification matters when the @em unified query
-- still has @em multiple atoms whose hierarchical structure the raw
-- shape gate refused: that's the (R, R, S) shape below.

-- ---------------------------------------------------------------------
-- (1) Three-atom case where PK-unification turns a non-hierarchical
--     query into a hierarchical one: r1, r2 share PK on x and
--     unify to a single R atom; the remaining (R, S) join on (y, z)
--     is a standard hierarchical CQ with class @c {R.y, S.y} and
--     class @c {R.z, S.z} both covering both atoms.
-- ---------------------------------------------------------------------
CREATE TABLE sjp_r (x int PRIMARY KEY, y int, z int);
CREATE TABLE sjp_s (y int, z int);
INSERT INTO sjp_r VALUES (1, 5, 7), (2, 6, 8);
INSERT INTO sjp_s VALUES (5, 7), (6, 9);

SELECT add_provenance('sjp_r');
SELECT add_provenance('sjp_s');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM sjp_r;
  PERFORM set_prob(provsql, 0.5) FROM sjp_s;
END $$;

-- Baseline: probability via the default evaluator on the unrewritten
-- circuit (the non-rewritten circuit is still read-once because of
-- the @c gate_times dedup on identical @c provsql inputs, so the
-- baseline @c probability_evaluate uses dDNNF / tree decomposition).
SET provsql.provenance = 'semiring';
CREATE TEMP TABLE sjp_baseline AS
  SELECT q.x, probability_evaluate(provenance()) AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sjp_r r1, sjp_r r2, sjp_s s
           WHERE r1.x = r2.x AND r1.y = s.y AND r2.z = s.z) q
   GROUP BY q.x;
SELECT remove_provenance('sjp_baseline');
SELECT x, ROUND(p::numeric, 6) AS prob_baseline FROM sjp_baseline;

-- PK-unification path: 'independent' on the rewritten read-once
-- circuit must match.
SET provsql.provenance = 'boolean';
CREATE TEMP TABLE sjp_rewritten AS
  SELECT q.x, probability_evaluate(provenance(), 'independent') AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sjp_r r1, sjp_r r2, sjp_s s
           WHERE r1.x = r2.x AND r1.y = s.y AND r2.z = s.z) q
   GROUP BY q.x;
SELECT remove_provenance('sjp_rewritten');
SELECT x, ROUND(p::numeric, 6) AS prob_rewritten FROM sjp_rewritten;

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM sjp_baseline b JOIN sjp_rewritten r ON b.x = r.x;

-- ---------------------------------------------------------------------
-- (2) NOT-NULL UNIQUE is FD-equivalent to a PRIMARY KEY (the
--     PK / NOT-NULL UNIQUE cache feeds the same single recogniser):
--     the same query shape over a NOT-NULL-UNIQUE relation must
--     produce the same probability as the PK case.
-- ---------------------------------------------------------------------
CREATE TABLE sjp_r_nnu (x int NOT NULL UNIQUE, y int, z int);
INSERT INTO sjp_r_nnu VALUES (1, 5, 7), (2, 6, 8);
SELECT add_provenance('sjp_r_nnu');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM sjp_r_nnu; END $$;

SET provsql.provenance = 'boolean';
CREATE TEMP TABLE sjp_nnu AS
  SELECT q.x, probability_evaluate(provenance(), 'independent') AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sjp_r_nnu r1, sjp_r_nnu r2, sjp_s s
           WHERE r1.x = r2.x AND r1.y = s.y AND r2.z = s.z) q
   GROUP BY q.x;
SELECT remove_provenance('sjp_nnu');
SELECT b.x, ROUND((b.p - n.p)::numeric, 9) AS diff_pk_vs_nnu
  FROM sjp_rewritten b JOIN sjp_nnu n ON b.x = n.x;

-- ---------------------------------------------------------------------
-- (3) Pure two-RTE self-join.  After unification the query has only
--     one atom; the safe-query hierarchical detector bails on
--     @c natoms @c < @c 2 and the query falls through to the
--     standard provenance rewrite, which produces a read-once
--     circuit on its own (the @c gate_times dedup folds the
--     duplicated @c r1.provsql / @c r2.provsql leaves to a single
--     input).  The probability must still match the baseline -- this
--     case is a regression check that the unified-single-atom path
--     doesn't break the existing rewrite.
-- ---------------------------------------------------------------------
SET provsql.provenance = 'semiring';
CREATE TEMP TABLE sjp_self_baseline AS
  SELECT q.x, probability_evaluate(provenance()) AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sjp_r r1, sjp_r r2
           WHERE r1.x = r2.x AND r1.y = 5 AND r2.z = 7) q
   GROUP BY q.x;
SELECT remove_provenance('sjp_self_baseline');

SET provsql.provenance = 'boolean';
CREATE TEMP TABLE sjp_self_rewritten AS
  SELECT q.x, probability_evaluate(provenance(), 'independent') AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sjp_r r1, sjp_r r2
           WHERE r1.x = r2.x AND r1.y = 5 AND r2.z = 7) q
   GROUP BY q.x;
SELECT remove_provenance('sjp_self_rewritten');

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_pure_self_join
  FROM sjp_self_baseline b JOIN sjp_self_rewritten r ON b.x = r.x;

DROP TABLE sjp_r, sjp_s, sjp_r_nnu CASCADE;
