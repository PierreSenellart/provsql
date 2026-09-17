\set ECHO none
\pset format unaligned

-- LIMIT / OFFSET over provenance-tracked relations.  The rows kept carry the
-- tokens they have without the cut.  At the top level of a statement that is
-- a sound reading, and nothing is reported; below it the cut feeds further
-- computation, and a warning is raised (once per statement).

CREATE TABLE lw_t(a int); INSERT INTO lw_t VALUES (1),(2),(3);
CREATE TABLE lw_u(a int); INSERT INTO lw_u VALUES (1),(2),(3);
CREATE TABLE lw_dst(a int);
SELECT add_provenance('lw_t'); SELECT add_provenance('lw_dst');

\echo -- no warning: top level, INSERT source, LIMIT ALL / NULL, untracked subquery
CREATE TABLE lw_1 AS SELECT a FROM lw_t ORDER BY a LIMIT 2;
CREATE TABLE lw_2 AS SELECT a FROM lw_t ORDER BY a OFFSET 1;
INSERT INTO lw_dst SELECT a FROM lw_t ORDER BY a LIMIT 1;
CREATE TABLE lw_3 AS SELECT a FROM (SELECT a FROM lw_t ORDER BY a LIMIT ALL) s;
CREATE TABLE lw_4 AS SELECT a FROM (SELECT a FROM lw_t ORDER BY a LIMIT NULL) s;
CREATE TABLE lw_5 AS SELECT t.a FROM lw_t t, (SELECT a FROM lw_u ORDER BY a LIMIT 1) s WHERE t.a = s.a;
CREATE TABLE lw_6 AS SELECT a FROM lw_t UNION SELECT a FROM lw_u ORDER BY a LIMIT 2;

\echo -- warning: FROM subquery
CREATE TABLE lw_7 AS SELECT a FROM (SELECT a FROM lw_t ORDER BY a LIMIT 2) s;
\echo -- warning: OFFSET alone
CREATE TABLE lw_8 AS SELECT a FROM (SELECT a FROM lw_t ORDER BY a OFFSET 1) s;
\echo -- warning: LATERAL
CREATE TABLE lw_9 AS SELECT u.a FROM lw_u u,
  LATERAL (SELECT a FROM lw_t t WHERE t.a >= u.a ORDER BY a LIMIT 1) s;
\echo -- warning: CTE
CREATE TABLE lw_10 AS WITH c AS (SELECT a FROM lw_t ORDER BY a LIMIT 2) SELECT a FROM c;
\echo -- warning: arm of a set operation
CREATE TABLE lw_11 AS (SELECT a FROM lw_t ORDER BY a LIMIT 1) UNION ALL SELECT a FROM lw_u;
\echo -- warning: two levels down, reported once
CREATE TABLE lw_12 AS SELECT a FROM (SELECT a FROM (SELECT a FROM lw_t LIMIT 1) s1 LIMIT 1) s2;
\echo -- warning: below the source of an INSERT
INSERT INTO lw_dst SELECT a FROM (SELECT a FROM lw_t ORDER BY a LIMIT 1) s;

DROP TABLE lw_t, lw_u, lw_dst, lw_1, lw_2, lw_3, lw_4, lw_5, lw_6, lw_7, lw_8,
  lw_9, lw_10, lw_11, lw_12;
