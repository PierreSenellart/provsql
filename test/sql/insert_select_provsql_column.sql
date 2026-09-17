\set ECHO none
\pset format unaligned

-- INSERT ... SELECT into a table without provenance tracking.
--  * The provsql column of a tracked relation cannot be selected as a value:
--    ProvSQL strips it from the source, and the INSERT used to fail inside the
--    planner on the dangling column reference.  It is refused with a message
--    pointing to provenance(), and not read as provenance(): the token of an
--    input relation is the provenance of the output row only in the simplest
--    queries.
--  * provenance() stores the provenance of each row; the statement then needs
--    no warning about provenance being lost.

CREATE TABLE isp(a int, b text);
INSERT INTO isp VALUES (1,'x'), (2,'y');
SELECT add_provenance('isp');
CREATE TABLE isp_m(value int, token uuid);
CREATE TABLE isp_m3(token uuid, value int, label text);
CREATE TABLE isp_plain(a int);
CREATE TABLE isp_tr(a int, b text);
SELECT add_provenance('isp_tr');

\echo -- refused: the provsql column as a value, in any position, alone or in a join
INSERT INTO isp_m SELECT a, provsql FROM isp;
INSERT INTO isp_m3 SELECT provsql, a, b FROM isp;
INSERT INTO isp_m SELECT s1.a, s1.provsql FROM isp s1, isp s2 WHERE s1.a < s2.a;
SELECT 'nothing inserted' AS q, (SELECT count(*) FROM isp_m) + (SELECT count(*) FROM isp_m3) AS n;

\echo -- provenance(): stored, without a warning
INSERT INTO isp_m SELECT a, provenance() FROM isp;
INSERT INTO isp_m3 SELECT provenance(), a, b FROM isp;
-- a join: the token stored is that of the joined row
INSERT INTO isp_m SELECT 10 * s1.a + s2.a, provenance() FROM isp s1, isp s2 WHERE s1.a < s2.a;

SET provsql.active = off;
SELECT 'single table' AS q, m.value, m.token = s.provsql AS is_row_token
  FROM isp_m m JOIN isp s ON s.a = m.value ORDER BY m.value;
SELECT 'three columns' AS q, m.value, m.label, m.token = s.provsql AS is_row_token
  FROM isp_m3 m JOIN isp s ON s.a = m.value ORDER BY m.value;
SELECT 'join' AS q, m.value, provsql.get_gate_type(m.token) AS gate,
       (SELECT array_agg(c ORDER BY c) FROM unnest(provsql.get_children(m.token)) c)
       = (SELECT array_agg(provsql ORDER BY provsql) FROM isp) AS children_are_the_rows
  FROM isp_m m WHERE m.value = 12;
RESET provsql.active;

\echo -- no provenance stored: the warning stays
INSERT INTO isp_plain SELECT a FROM isp;

\echo -- tracked target: provenance propagates, with or without the column named
INSERT INTO isp_tr SELECT a, b FROM isp;
INSERT INTO isp_tr SELECT a, b, provsql FROM isp WHERE a = 1;
SET provsql.active = off;
SELECT 'tracked target' AS q, t.a, count(*) AS copies, bool_and(t.provsql = s.provsql) AS same_token
  FROM isp_tr t JOIN isp s ON s.a = t.a GROUP BY t.a ORDER BY t.a;
RESET provsql.active;

DROP TABLE isp, isp_m, isp_m3, isp_plain, isp_tr;
