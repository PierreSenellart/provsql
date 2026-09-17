\set ECHO none
\pset format unaligned

-- FETCH FIRST ... WITH TIES (PostgreSQL 13+) on a non-ALL set operation: like
-- ORDER BY / LIMIT, it applies to the deduplicated result, so it moves onto
-- the wrapper that deduplicates.  (a, b) rows, ordered by a only, so that
-- rows sharing the first a tie.
--   r = {(1,1), (1,1), (1,2), (2,1)}   s = {(1,2), (1,3), (3,1)}
--   r UNION s = {(1,1), (1,2), (1,3), (2,1), (3,1)}
SELECT current_setting('server_version_num')::int >= 130000 AS pg_has_with_ties
\gset
\if :pg_has_with_ties

CREATE TABLE swt_r(a int, b int); INSERT INTO swt_r VALUES (1,1),(1,1),(1,2),(2,1);
CREATE TABLE swt_s(a int, b int); INSERT INTO swt_s VALUES (1,2),(1,3),(3,1);
SELECT add_provenance('swt_r'); SELECT add_provenance('swt_s');

-- First row WITH TIES: the three distinct rows with a = 1.  Cutting before the
-- deduplication would tie on the five a = 1 rows of the UNION ALL instead, and
-- LIMIT 1 without ties would keep one row.
CREATE TABLE swt_1 AS SELECT a, b FROM swt_r UNION SELECT a, b FROM swt_s
  ORDER BY a FETCH FIRST 1 ROW WITH TIES;
SELECT remove_provenance('swt_1');
SELECT 'first 1 with ties' AS q, a, b FROM swt_1 ORDER BY a, b;

-- OFFSET 3 then WITH TIES: (2,1) alone.
CREATE TABLE swt_2 AS SELECT a, b FROM swt_r UNION SELECT a, b FROM swt_s
  ORDER BY a OFFSET 3 FETCH FIRST 1 ROW WITH TIES;
SELECT remove_provenance('swt_2');
SELECT 'offset 3 with ties' AS q, a, b FROM swt_2 ORDER BY a, b;

-- EXCEPT: r EXCEPT s keeps (1,1), (2,1) and the zero-able (1,2); ties on a = 1.
CREATE TABLE swt_3 AS SELECT a, b FROM swt_r EXCEPT SELECT a, b FROM swt_s
  ORDER BY a FETCH FIRST 1 ROW WITH TIES;
SELECT remove_provenance('swt_3');
SELECT 'except with ties' AS q, a, b FROM swt_3 ORDER BY a, b;

DROP TABLE swt_r, swt_s, swt_1, swt_2, swt_3;

\else

\echo set_operation_with_ties: skipped on PostgreSQL < 13 (no WITH TIES)

\endif
