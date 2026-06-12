\set ECHO none
\pset format unaligned

-- General FD closure (detector-only; no rewriter change).
--
-- The textbook framework (Dalvi & Suciu 2007 §5.1) envisions a
-- fixpoint over the @c DETERMINED matrix: collect every FD in
-- Γ_p(q) (constant-selection FDs, PK FDs, deterministic-relation
-- FDs) and iteratively apply each FD until no new
-- FD-determined-in-RTE tags are set.  The hierarchicality check
-- then runs on the FD-reduced atom-sets.
--
-- In the current rule set (constant-pinning + schema PK/UNIQUE +
-- deterministic transparency), every FD application is
-- @em independent: the PK-FD pass already fires every FD in a
-- single walk over the RTE list, and constant-selection and
-- deterministic transparency act through orthogonal mechanisms
-- (residual-cleanup multi-component for constant pinning,
-- @c DETERMINED-all-classes for deterministic atoms).  No
-- iteration unlocks a new tag.  The fixpoint reduces to a no-op on
-- the canonical motivating case -- the triangle CQ with two PKs on
-- different relations.  An FD-induced nested rewrite
-- (function/free split) is required to compose constant-pinning
-- with a PK on the same relation; that is deferred.
--
-- This file regression-tests the canonical triangle example to
-- confirm the cumulative passes deliver the closure guarantee:
-- "accept any query whose FD-reduced atom sets are hierarchical
-- and whose existing single-level wrap is read-once".

-- ---------------------------------------------------------------------
-- (1) Triangle CQ with PKs on R(a) and T(c) (Dalvi & Suciu 2007
--     §5.1).  Raw atom-sets are pairwise neither nested nor
--     disjoint -- non-hierarchical without the FDs.  Applying both
--     PKs gives @c atoms(a) @c = @c {R}, @c atoms(b) @c = @c {S},
--     @c atoms(c) @c = @c {S, T}: pairwise nested-or-disjoint
--     (a disjoint from b and c; b nested in c).
-- ---------------------------------------------------------------------
CREATE TABLE fdc_r (a int PRIMARY KEY, b int);
CREATE TABLE fdc_s (b int, c int);
CREATE TABLE fdc_t (c int PRIMARY KEY, a int);

INSERT INTO fdc_r VALUES (1, 10);
INSERT INTO fdc_s VALUES (10, 100);
INSERT INTO fdc_t VALUES (100, 1);

SELECT add_provenance('fdc_r');
SELECT add_provenance('fdc_s');
SELECT add_provenance('fdc_t');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM fdc_r;
  PERFORM set_prob(provsql, 0.5) FROM fdc_s;
  PERFORM set_prob(provsql, 0.5) FROM fdc_t;
END $$;

SET provsql.provenance = 'semiring';
CREATE TEMP TABLE fdc_baseline AS
  SELECT q.x, probability_evaluate(provenance()) AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM fdc_r r, fdc_s s, fdc_t t
           WHERE r.b = s.b AND s.c = t.c AND t.a = r.a) q
   GROUP BY q.x;
SELECT remove_provenance('fdc_baseline');
SELECT x, ROUND(p::numeric, 6) AS prob_baseline FROM fdc_baseline;

SET provsql.provenance = 'boolean';
CREATE TEMP TABLE fdc_rewritten AS
  SELECT q.x, probability_evaluate(provenance(), 'independent') AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM fdc_r r, fdc_s s, fdc_t t
           WHERE r.b = s.b AND s.c = t.c AND t.a = r.a) q
   GROUP BY q.x;
SELECT remove_provenance('fdc_rewritten');
SELECT x, ROUND(p::numeric, 6) AS prob_rewritten FROM fdc_rewritten;

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM fdc_baseline b JOIN fdc_rewritten r ON b.x = r.x;

-- ---------------------------------------------------------------------
-- (2) PK-FD + deterministic-transparency composition (star schema
--     with PKs on every dimension).  Transparency drops the
--     dimension atoms from atom-set membership; the PK on each
--     dimension is then irrelevant (the dimension's FDs would have
--     been redundant once it became transparent), but the rewrite
--     still uses the PK FD on the fact table if one is present.
--     Cross-check that this composition works and that no FD
--     interaction regression occurs.
-- ---------------------------------------------------------------------
CREATE TABLE fdc_fact (pid int, cid int);
CREATE TABLE fdc_dim_p (pid int PRIMARY KEY, cat text);
CREATE TABLE fdc_dim_c (cid int PRIMARY KEY, region text);
INSERT INTO fdc_fact VALUES (1, 10), (1, 10), (2, 11);
INSERT INTO fdc_dim_p VALUES (1, 'A'), (2, 'B');
INSERT INTO fdc_dim_c VALUES (10, 'EU'), (11, 'US');
SELECT add_provenance('fdc_fact');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM fdc_fact; END $$;

SET provsql.provenance = 'semiring';
CREATE TEMP TABLE fdc_star_baseline AS
  SELECT q.x, probability_evaluate(provenance()) AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM fdc_fact f, fdc_dim_p p, fdc_dim_c c
           WHERE f.pid = p.pid AND f.cid = c.cid
             AND p.cat = 'A' AND c.region = 'EU') q
   GROUP BY q.x;
SELECT remove_provenance('fdc_star_baseline');

SET provsql.provenance = 'boolean';
CREATE TEMP TABLE fdc_star_rewritten AS
  SELECT q.x, probability_evaluate(provenance(), 'independent') AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM fdc_fact f, fdc_dim_p p, fdc_dim_c c
           WHERE f.pid = p.pid AND f.cid = c.cid
             AND p.cat = 'A' AND c.region = 'EU') q
   GROUP BY q.x;
SELECT remove_provenance('fdc_star_rewritten');

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_star_baseline_vs_rewritten
  FROM fdc_star_baseline b JOIN fdc_star_rewritten r ON b.x = r.x;

DROP TABLE fdc_r, fdc_s, fdc_t, fdc_fact, fdc_dim_p, fdc_dim_c CASCADE;
