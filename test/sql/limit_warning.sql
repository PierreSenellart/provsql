\set ECHO none
\pset format unaligned

-- LIMIT / OFFSET over provenance-tracked relations.  ORDER BY ... LIMIT k is,
-- in each possible world, the filter of a rank (see limit_rank): nothing is
-- lost, and nothing is reported.  A LIMIT that stays a truncation of the
-- actual result -- marked plain(), without ORDER BY, over an aggregation --
-- keeps rows that carry the tokens they have in the full result.  At the top
-- level of a statement that is a sound reading, but an ORDER BY ... LIMIT that
-- is not read in each world (over a set operation, an aggregation...) is
-- reported unless marked plain(); below the top level the truncated result
-- feeds further computation, and a warning is raised (once per statement).

CREATE TABLE lw_t(a int); INSERT INTO lw_t VALUES (1),(2),(3);
CREATE TABLE lw_u(a int); INSERT INTO lw_u VALUES (1),(2),(3);
CREATE TABLE lw_dst(a int);
SELECT add_provenance('lw_t'); SELECT add_provenance('lw_dst');

\echo -- no warning: top level, INSERT source, LIMIT ALL / NULL, untracked subquery
CREATE TABLE lw_1 AS SELECT a FROM lw_t ORDER BY a LIMIT plain(2);
CREATE TABLE lw_2 AS SELECT a FROM lw_t ORDER BY a OFFSET plain(1);
INSERT INTO lw_dst SELECT a FROM lw_t ORDER BY a LIMIT plain(1);
CREATE TABLE lw_3 AS SELECT a FROM (SELECT a FROM lw_t ORDER BY a LIMIT ALL) s;
CREATE TABLE lw_4 AS SELECT a FROM (SELECT a FROM lw_t ORDER BY a LIMIT NULL) s;
CREATE TABLE lw_5 AS SELECT t.a FROM lw_t t, (SELECT a FROM lw_u ORDER BY a LIMIT 1) s WHERE t.a = s.a;
CREATE TABLE lw_6 AS SELECT a FROM lw_t UNION SELECT a FROM lw_u ORDER BY a LIMIT plain(2);

\echo -- warning: FROM subquery
CREATE TABLE lw_7 AS SELECT a FROM (SELECT a FROM lw_t ORDER BY a LIMIT plain(2)) s;
\echo -- warning: OFFSET alone
CREATE TABLE lw_8 AS SELECT a FROM (SELECT a FROM lw_t ORDER BY a OFFSET plain(1)) s;
\echo -- warning: LATERAL
CREATE TABLE lw_9 AS SELECT u.a FROM lw_u u,
  LATERAL (SELECT a FROM lw_t t WHERE t.a >= u.a ORDER BY a LIMIT plain(1)) s;
\echo -- warning: CTE
CREATE TABLE lw_10 AS WITH c AS (SELECT a FROM lw_t ORDER BY a LIMIT plain(2)) SELECT a FROM c;
\echo -- warning: arm of a set operation
CREATE TABLE lw_11 AS (SELECT a FROM lw_t ORDER BY a LIMIT plain(1)) UNION ALL SELECT a FROM lw_u;
\echo -- warning: two levels down, reported once
CREATE TABLE lw_12 AS SELECT a FROM (SELECT a FROM (SELECT a FROM lw_t LIMIT 1) s1 LIMIT 1) s2;
\echo -- warning: below the source of an INSERT
INSERT INTO lw_dst SELECT a FROM (SELECT a FROM lw_t ORDER BY a LIMIT plain(1)) s;
\echo -- warning: without ORDER BY
CREATE TABLE lw_13 AS SELECT a FROM (SELECT a FROM lw_t LIMIT 2) s;
\echo -- warning: over an aggregation
CREATE TABLE lw_14 AS SELECT a FROM (SELECT a, count(*) FROM lw_t GROUP BY a ORDER BY a LIMIT 2) s;

DROP TABLE lw_1, lw_2, lw_3, lw_4, lw_5, lw_6, lw_7, lw_8, lw_9, lw_10, lw_11,
  lw_12, lw_13, lw_14;

-- The filter of a rank needs PostgreSQL 11 (window frames with EXCLUDE);
-- before, these LIMITs stay truncations, with the warning.
SELECT current_setting('server_version_num')::int >= 110000 AS pg_has_exclude
\gset
\if :pg_has_exclude

\echo -- no warning: the filter of a rank, in a FROM subquery, alone as OFFSET, in LATERAL, a CTE, an arm, below an INSERT
CREATE TABLE lw_15 AS SELECT a FROM (SELECT a FROM lw_t ORDER BY a LIMIT 2) s;
CREATE TABLE lw_16 AS SELECT a FROM (SELECT a FROM lw_t ORDER BY a OFFSET 1) s;
CREATE TABLE lw_17 AS SELECT u.a FROM lw_u u,
  LATERAL (SELECT a FROM lw_t t WHERE t.a >= u.a ORDER BY a LIMIT 1) s;
CREATE TABLE lw_18 AS WITH c AS (SELECT a FROM lw_t ORDER BY a LIMIT 2) SELECT a FROM c;
CREATE TABLE lw_19 AS (SELECT a FROM lw_t ORDER BY a LIMIT 1) UNION ALL SELECT a FROM lw_u;
INSERT INTO lw_dst SELECT a FROM (SELECT a FROM lw_t ORDER BY a LIMIT 1) s;
DROP TABLE lw_15, lw_16, lw_17, lw_18, lw_19;

\echo -- warning: top level, not read in each world (set operation, aggregation), unless plain()
CREATE TABLE lw_20 AS SELECT a FROM lw_t UNION SELECT a FROM lw_u ORDER BY a LIMIT 2;
CREATE TABLE lw_21 AS SELECT a, count(*) FROM lw_t GROUP BY a ORDER BY a LIMIT 2;
INSERT INTO lw_dst SELECT a FROM lw_t UNION SELECT a FROM lw_u ORDER BY a LIMIT 1;
CREATE TABLE lw_22 AS SELECT a, count(*) FROM lw_t GROUP BY a ORDER BY a LIMIT plain(2);
DROP TABLE lw_20, lw_21, lw_22;

\else

\echo limit_warning: the filter of a rank skipped on PostgreSQL < 11

\endif

DROP TABLE lw_t, lw_u, lw_dst;
