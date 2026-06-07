\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- PK / NOT-NULL UNIQUE induced FDs (Dalvi & Suciu 2007 §5.1).
--
-- A relation @c R with PRIMARY KEY @c K (or NOT NULL UNIQUE on @c K)
-- carries the FD @c K @c → @c A for every non-key attribute @c A.
-- Under this FD, @c A is functionally determined within @c R: the
-- safe-query detector tags @c A's union-find class as "determined in
-- @c R" and the textbook hierarchicality check accepts queries whose
-- raw atom-sets would otherwise be neither nested nor disjoint.
--
-- The canonical motivating shape is the H-query under a PK on the
-- middle atom:
--
--   q :- R(x), S(x, y), T(y),  with PRIMARY KEY (x) on S.
--
-- Without the PK-FD pass: atoms(x)={R,S}, atoms(y)={S,T} – neither
-- nested nor disjoint – non-hierarchical → bail.  With the PK-FD
-- pass: the PK on @c S.x induces @c S.x @c → @c S.y, so @c S drops
-- from atoms(y); the FD-aware atom-sets are @c {R,S} and @c {T},
-- disjoint – hierarchical via a per-atom anchor (@c R and @c S
-- anchor on class @c x, @c T on class @c y; the rewriter exposes
-- both classes as slots on @c S).

-- ---------------------------------------------------------------------
-- (1) The PK case: PRIMARY KEY on S(x).
-- ---------------------------------------------------------------------

CREATE TABLE pk_r_pk (x int);
CREATE TABLE pk_s_pk (x int PRIMARY KEY, y int);
CREATE TABLE pk_t_pk (y int);
INSERT INTO pk_r_pk VALUES (1), (2), (1);
INSERT INTO pk_s_pk VALUES (1, 5), (2, 6);
INSERT INTO pk_t_pk VALUES (5), (6), (7);

SELECT add_provenance('pk_r_pk');
SELECT add_provenance('pk_s_pk');
SELECT add_provenance('pk_t_pk');

DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM pk_r_pk;
  PERFORM set_prob(provsql, 0.5) FROM pk_s_pk;
  PERFORM set_prob(provsql, 0.5) FROM pk_t_pk;
END $$;

-- Baseline: probability via the default evaluator on the unrewritten
-- circuit.
SET provsql.provenance = 'semiring';
CREATE TEMP TABLE pk_baseline_pk AS
  SELECT q.x, probability_evaluate(provenance()) AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM pk_r_pk r, pk_s_pk s, pk_t_pk t
           WHERE r.x = s.x AND s.y = t.y) q
   GROUP BY q.x;
SELECT remove_provenance('pk_baseline_pk');
SELECT x, ROUND(p::numeric, 6) AS prob_baseline FROM pk_baseline_pk;

-- PK-FD path: 'independent' on the rewritten circuit must match.
SET provsql.provenance = 'boolean';
CREATE TEMP TABLE pk_rewritten_pk AS
  SELECT q.x, probability_evaluate(provenance(), 'independent') AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM pk_r_pk r, pk_s_pk s, pk_t_pk t
           WHERE r.x = s.x AND s.y = t.y) q
   GROUP BY q.x;
SELECT remove_provenance('pk_rewritten_pk');
SELECT x, ROUND(p::numeric, 6) AS prob_rewritten FROM pk_rewritten_pk;

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM pk_baseline_pk b JOIN pk_rewritten_pk r ON b.x = r.x;

-- ---------------------------------------------------------------------
-- (2) NOT-NULL UNIQUE is FD-equivalent to a PRIMARY KEY: replacing
--     PRIMARY KEY with UNIQUE NOT NULL on the middle atom must
--     produce the same rewritten probability.
-- ---------------------------------------------------------------------

CREATE TABLE pk_s_nnu (x int NOT NULL UNIQUE, y int);
INSERT INTO pk_s_nnu VALUES (1, 5), (2, 6);
SELECT add_provenance('pk_s_nnu');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM pk_s_nnu;
END $$;

SET provsql.provenance = 'boolean';
CREATE TEMP TABLE pk_rewritten_nnu AS
  SELECT q.x, probability_evaluate(provenance(), 'independent') AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM pk_r_pk r, pk_s_nnu s, pk_t_pk t
           WHERE r.x = s.x AND s.y = t.y) q
   GROUP BY q.x;
SELECT remove_provenance('pk_rewritten_nnu');
SELECT x, ROUND(p::numeric, 6) AS prob_nnu FROM pk_rewritten_nnu;

-- ---------------------------------------------------------------------
-- (3) Nullable UNIQUE: must NOT induce a key-FD (UNIQUE allows
--     multiple NULL rows in PostgreSQL, so @c ∅ @c → @c attr does
--     not hold without NOT NULL).  The detector falls through;
--     'independent' rejects the non-hierarchical circuit.
-- ---------------------------------------------------------------------

CREATE TABLE pk_s_nullable (x int UNIQUE, y int);  -- nullable UNIQUE
INSERT INTO pk_s_nullable VALUES (1, 5), (2, 6);
SELECT add_provenance('pk_s_nullable');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM pk_s_nullable;
END $$;

SET provsql.provenance = 'boolean';
DO $$
DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'independent')
      FROM (SELECT DISTINCT 1 AS x
              FROM pk_r_pk r, pk_s_nullable s, pk_t_pk t
             WHERE r.x = s.x AND s.y = t.y) q
     GROUP BY q.x;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected ''independent'' to reject the H-query '
                    'under a nullable UNIQUE (no PK FD applies)';
  END IF;
END $$;

-- ---------------------------------------------------------------------
-- (4) Composite PK partial coverage: PRIMARY KEY @c (a, b) on the
--     middle atom requires the query to equate @em both @c a and
--     @c b to the outer atoms before the FD applies.  Equating only
--     @c a leaves the H-shape non-hierarchical; the PK-FD pass must
--     NOT fire.
-- ---------------------------------------------------------------------

CREATE TABLE pk_r_comp (a int);
CREATE TABLE pk_s_comp (a int, b int, y int, PRIMARY KEY (a, b));
CREATE TABLE pk_t_comp (y int);
-- Duplicate R rows on a=1 make the circuit non-read-once without
-- the PK-FD pass: r1 and r3 both join the same (s, t) pair, so the
-- (s, t) tokens appear in multiple rows of the cross product.
INSERT INTO pk_r_comp VALUES (1), (2), (1);
INSERT INTO pk_s_comp VALUES (1, 1, 5), (2, 2, 6);
INSERT INTO pk_t_comp VALUES (5), (6), (7);

SELECT add_provenance('pk_r_comp');
SELECT add_provenance('pk_s_comp');
SELECT add_provenance('pk_t_comp');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM pk_r_comp;
  PERFORM set_prob(provsql, 0.5) FROM pk_s_comp;
  PERFORM set_prob(provsql, 0.5) FROM pk_t_comp;
END $$;

-- Equate only @c a: PK has 2 columns, only @c a anchored.  Must bail.
SET provsql.provenance = 'boolean';
DO $$
DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'independent')
      FROM (SELECT DISTINCT 1 AS x
              FROM pk_r_comp r, pk_s_comp s, pk_t_comp t
             WHERE r.a = s.a AND s.y = t.y) q
     GROUP BY q.x;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected ''independent'' to reject when only @c a '
                    'of a composite PRIMARY KEY (a, b) is equated';
  END IF;
END $$;

-- ---------------------------------------------------------------------
-- (5) Collision on the FD-determined value: PRIMARY KEY (x) on S still
--     holds (x -> y is a function) but y is NOT injective -- several x
--     map to the same y.  The query is genuinely read-once via the
--     y-grouped factorisation
--       OR_y  T(y) AND (OR_{x: S(x,y)} R(x) AND S(x,y)),
--     so 'independent' on the rewritten circuit must match the exact
--     baseline.  The earlier cases all used injective y, where the flat
--     per-atom wrap happens to be read-once; this case exercises the
--     FD bridging-group rewrite that GROUPs the determining side {R,S}
--     on the determined value y.  Regression for the gap where the flat
--     wrap shared the T(y) leaf across colliding keys.
-- ---------------------------------------------------------------------

CREATE TABLE pk_r_coll (x int);
CREATE TABLE pk_s_coll (x int PRIMARY KEY, y int);
CREATE TABLE pk_t_coll (y int);
-- x=1 and x=2 both map to y=5 (collision); x=3 maps to y=6.  Duplicate
-- R rows on x=1 too, so neither side is read-once without the rewrite.
INSERT INTO pk_r_coll VALUES (1), (2), (3), (1);
INSERT INTO pk_s_coll VALUES (1, 5), (2, 5), (3, 6);
INSERT INTO pk_t_coll VALUES (5), (6), (7);

SELECT add_provenance('pk_r_coll');
SELECT add_provenance('pk_s_coll');
SELECT add_provenance('pk_t_coll');

DO $$ BEGIN
  PERFORM set_prob(provsql, 0.4) FROM pk_r_coll;
  PERFORM set_prob(provsql, 0.6) FROM pk_s_coll;
  PERFORM set_prob(provsql, 0.5) FROM pk_t_coll;
END $$;

SET provsql.provenance = 'semiring';
CREATE TEMP TABLE pk_baseline_coll AS
  SELECT q.x, probability_evaluate(provenance()) AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM pk_r_coll r, pk_s_coll s, pk_t_coll t
           WHERE r.x = s.x AND s.y = t.y) q
   GROUP BY q.x;
SELECT remove_provenance('pk_baseline_coll');
SELECT x, ROUND(p::numeric, 6) AS prob_baseline_coll FROM pk_baseline_coll;

SET provsql.provenance = 'boolean';
CREATE TEMP TABLE pk_rewritten_coll AS
  SELECT q.x, probability_evaluate(provenance(), 'independent') AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM pk_r_coll r, pk_s_coll s, pk_t_coll t
           WHERE r.x = s.x AND s.y = t.y) q
   GROUP BY q.x;
SELECT remove_provenance('pk_rewritten_coll');
SELECT x, ROUND(p::numeric, 6) AS prob_rewritten_coll FROM pk_rewritten_coll;

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten_coll
  FROM pk_baseline_coll b JOIN pk_rewritten_coll r ON b.x = r.x;

DROP TABLE pk_r_pk, pk_s_pk, pk_t_pk, pk_s_nnu, pk_s_nullable,
           pk_r_comp, pk_s_comp, pk_t_comp,
           pk_r_coll, pk_s_coll, pk_t_coll CASCADE;
