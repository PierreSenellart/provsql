\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- ----------------------------------------------------------------------
-- Read-only skeleton-safety detector (safe_query_skeleton_is_hierarchical,
-- exposed here via the C symbol skeleton_is_safe).  For an aggregate /
-- HAVING query, reports whether the conjunctive skeleton sk(Q) feeding
-- the aggregate is a self-join-free hierarchical CQ (Dalvi-Suciu safe) --
-- the skeleton-safety axis of the Ré-Suciu HAVING trichotomy.
--
-- Verdicts are conservative: true only when a hierarchical structure is
-- positively certified.  A single base relation is trivially safe;
-- disconnected multi-component skeletons and shapes the detector cannot
-- inspect are reported unsafe (never a false "safe").
-- ----------------------------------------------------------------------

-- Test-local binding to the exported C entry point (not part of the
-- shipped extension API).
CREATE FUNCTION skel(text) RETURNS boolean
  AS 'provsql','skeleton_is_safe' LANGUAGE C STABLE;

CREATE TABLE sk_r(x int);
CREATE TABLE sk_s(x int, y int);
CREATE TABLE sk_t(y int);
CREATE TABLE sk_u(x int, z int);
CREATE TABLE sk_det(x int);          -- no provenance: deterministic atoms
INSERT INTO sk_r  VALUES (1),(2),(3);
INSERT INTO sk_s  VALUES (1,1),(2,2),(3,3);
INSERT INTO sk_t  VALUES (1),(2),(3);
INSERT INTO sk_u  VALUES (1,1),(2,2),(3,3);
INSERT INTO sk_det VALUES (1),(2),(3);
SELECT add_provenance('sk_r');
SELECT add_provenance('sk_s');
SELECT add_provenance('sk_t');
SELECT add_provenance('sk_u');

-- (1) Single base relation: trivially hierarchical.
SELECT 'single-table' AS shape,
       skel('SELECT count(*) FROM sk_r GROUP BY x HAVING count(*) > 1') AS safe;

-- (2) Star join on the root variable x (atoms(x) = {R,S}, atoms(y) = {S}
-- nested): hierarchical.
SELECT 'star-join (root x)' AS shape,
       skel('SELECT count(*) FROM sk_r r, sk_s s WHERE r.x = s.x '
            'GROUP BY r.x HAVING count(*) > 1') AS safe;

-- (3) Three-way star on x (atoms(x) = {R,S,U}, atoms(y),atoms(z) nested):
-- hierarchical.
SELECT 'three-way star (root x)' AS shape,
       skel('SELECT count(*) FROM sk_r r, sk_s s, sk_u u '
            'WHERE r.x = s.x AND r.x = u.x GROUP BY r.x HAVING count(*) > 1') AS safe;

-- (4) Aggregated variable as a head Var: HAVING SUM(s.y) over the star
-- join still has root x; the skeleton stays hierarchical.
SELECT 'agg-var head (sum)' AS shape,
       skel('SELECT r.x FROM sk_r r, sk_s s WHERE r.x = s.x '
            'GROUP BY r.x HAVING sum(s.y) > 5') AS safe;

-- (5) Deterministic (no-provenance) atom joined on the root: accepted as
-- a probability-1 atom; still hierarchical.
SELECT 'deterministic join (root x)' AS shape,
       skel('SELECT count(*) FROM sk_r r, sk_det d WHERE r.x = d.x '
            'GROUP BY r.x HAVING count(*) > 1') AS safe;

-- (6) Canonical non-hierarchical H-query R(x),S(x,y),T(y): atoms(x)={R,S}
-- and atoms(y)={S,T} overlap without nesting -- no root variable, #P-hard.
SELECT 'non-hierarchical R-S-T' AS shape,
       skel('SELECT count(*) FROM sk_r r, sk_s s, sk_t t '
            'WHERE r.x = s.x AND s.y = t.y GROUP BY r.x HAVING count(*) > 1') AS safe;

-- (7) Self-join with no PK / disjoint-constant rescue: not self-join-free.
SELECT 'unrescued self-join' AS shape,
       skel('SELECT count(*) FROM sk_s a, sk_s b WHERE a.y = b.x '
            'GROUP BY a.x HAVING count(*) > 1') AS safe;

-- (8) Disconnected components (Cartesian product feeding the aggregate):
-- conservatively reported unsafe (not certified component-wise).
SELECT 'disconnected components' AS shape,
       skel('SELECT count(*) FROM sk_r r, sk_t t GROUP BY r.x HAVING count(*) > 1') AS safe;

SELECT remove_provenance('sk_r');
SELECT remove_provenance('sk_s');
SELECT remove_provenance('sk_t');
SELECT remove_provenance('sk_u');
DROP TABLE sk_r, sk_s, sk_t, sk_u, sk_det CASCADE;
DROP FUNCTION skel(text);
