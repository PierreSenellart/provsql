\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Exercises the HAVING-trichotomy classifier surfaced through the
-- provsql.classify_having GUC.  When on, every top-level SELECT with a
-- HAVING aggregate comparison alpha(y) theta k emits a NOTICE labelling
-- the (alpha, theta) pair under the Ré-Suciu trichotomy, combining the
-- static (alpha, theta) overlay with the skeleton-safety axis.  Read-only.
-- See the "HAVING Query Complexity" section of
-- doc/source/dev/probability-evaluation.rst for the verdict tables.
--
-- Each classifier query uses the top-level CREATE TEMP TABLE AS /
-- remove_provenance idiom (as in classify_query.sql): the CTAS source
-- SELECT still goes through the planner hook at executor depth 0 (so the
-- NOTICE fires), but its non-deterministic provenance-token rows sink
-- into the temp table instead of the captured output.  (The query must
-- stay top-level: the classifier is gated on executor_depth == 0, so a
-- PL/pgSQL wrapper would suppress the NOTICE.)

SET client_min_messages = notice;
SET provsql.classify_having = on;

-- Safe skeleton: a single flat base relation (trivially hierarchical).
CREATE TABLE clh_flat(id int, v int);
INSERT INTO clh_flat VALUES (1,10),(1,20),(2,30);
SELECT add_provenance('clh_flat');

-- Unsafe skeleton: the canonical non-hierarchical H-query R(x),S(x,y),T(y).
CREATE TABLE clh_r(x int);
CREATE TABLE clh_s(x int, y int);
CREATE TABLE clh_t(y int);
INSERT INTO clh_r VALUES (1);
INSERT INTO clh_s VALUES (1,1);
INSERT INTO clh_t VALUES (1);
SELECT add_provenance('clh_r');
SELECT add_provenance('clh_s');
SELECT add_provenance('clh_t');

-- ---- Safe skeleton: MIN/MAX/COUNT are P for every operator (incl. <>);
--      SUM/AVG/COUNT(DISTINCT) need the stricter alpha-safety. ----
CREATE TEMP TABLE k AS SELECT 1 FROM clh_flat GROUP BY id HAVING max(v) >= 15;
SELECT remove_provenance('k'); DROP TABLE k;
CREATE TEMP TABLE k AS SELECT 1 FROM clh_flat GROUP BY id HAVING max(v) <> 15;
SELECT remove_provenance('k'); DROP TABLE k;
CREATE TEMP TABLE k AS SELECT 1 FROM clh_flat GROUP BY id HAVING count(*) = 2;
SELECT remove_provenance('k'); DROP TABLE k;
CREATE TEMP TABLE k AS SELECT 1 FROM clh_flat GROUP BY id HAVING sum(v) >= 15;
SELECT remove_provenance('k'); DROP TABLE k;
CREATE TEMP TABLE k AS SELECT 1 FROM clh_flat GROUP BY id HAVING avg(v) > 15;
SELECT remove_provenance('k'); DROP TABLE k;
CREATE TEMP TABLE k AS SELECT 1 FROM clh_flat GROUP BY id HAVING count(DISTINCT v) >= 2;
SELECT remove_provenance('k'); DROP TABLE k;
-- operand order flipped: 15 <= max(v) is (MAX, >=)
CREATE TEMP TABLE k AS SELECT 1 FROM clh_flat GROUP BY id HAVING 15 <= max(v);
SELECT remove_provenance('k'); DROP TABLE k;

-- ---- Unsafe skeleton: the Layer-2 approximation overlay (direction-
--      asymmetric); <> and COUNT/SUM with >,>= are open. ----
CREATE TEMP TABLE k AS SELECT 1 FROM clh_r r, clh_s s, clh_t t WHERE r.x=s.x AND s.y=t.y GROUP BY r.x HAVING max(s.y) >= 5;
SELECT remove_provenance('k'); DROP TABLE k;
CREATE TEMP TABLE k AS SELECT 1 FROM clh_r r, clh_s s, clh_t t WHERE r.x=s.x AND s.y=t.y GROUP BY r.x HAVING max(s.y) <= 5;
SELECT remove_provenance('k'); DROP TABLE k;
CREATE TEMP TABLE k AS SELECT 1 FROM clh_r r, clh_s s, clh_t t WHERE r.x=s.x AND s.y=t.y GROUP BY r.x HAVING min(s.y) < 5;
SELECT remove_provenance('k'); DROP TABLE k;
CREATE TEMP TABLE k AS SELECT 1 FROM clh_r r, clh_s s, clh_t t WHERE r.x=s.x AND s.y=t.y GROUP BY r.x HAVING min(s.y) >= 5;
SELECT remove_provenance('k'); DROP TABLE k;
CREATE TEMP TABLE k AS SELECT 1 FROM clh_r r, clh_s s, clh_t t WHERE r.x=s.x AND s.y=t.y GROUP BY r.x HAVING count(*) <= 2;
SELECT remove_provenance('k'); DROP TABLE k;
CREATE TEMP TABLE k AS SELECT 1 FROM clh_r r, clh_s s, clh_t t WHERE r.x=s.x AND s.y=t.y GROUP BY r.x HAVING count(*) > 2;
SELECT remove_provenance('k'); DROP TABLE k;
CREATE TEMP TABLE k AS SELECT 1 FROM clh_r r, clh_s s, clh_t t WHERE r.x=s.x AND s.y=t.y GROUP BY r.x HAVING sum(s.y) < 5;
SELECT remove_provenance('k'); DROP TABLE k;
CREATE TEMP TABLE k AS SELECT 1 FROM clh_r r, clh_s s, clh_t t WHERE r.x=s.x AND s.y=t.y GROUP BY r.x HAVING sum(s.y) >= 5;
SELECT remove_provenance('k'); DROP TABLE k;
CREATE TEMP TABLE k AS SELECT 1 FROM clh_r r, clh_s s, clh_t t WHERE r.x=s.x AND s.y=t.y GROUP BY r.x HAVING avg(s.y) = 5;
SELECT remove_provenance('k'); DROP TABLE k;
CREATE TEMP TABLE k AS SELECT 1 FROM clh_r r, clh_s s, clh_t t WHERE r.x=s.x AND s.y=t.y GROUP BY r.x HAVING max(s.y) <> 5;
SELECT remove_provenance('k'); DROP TABLE k;

-- Multiple aggregate comparisons in one HAVING: one NOTICE per comparison.
CREATE TEMP TABLE k AS SELECT 1 FROM clh_flat GROUP BY id HAVING count(*) >= 2 AND max(v) <= 30;
SELECT remove_provenance('k'); DROP TABLE k;

SET provsql.classify_having = off;
DROP TABLE clh_flat, clh_r, clh_s, clh_t CASCADE;
