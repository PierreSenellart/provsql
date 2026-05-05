\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SET provsql.where_provenance = on;

/* Where-provenance under a multi-table join.
 *
 * The bug this test guards against: when the planner builds the project
 * gate over a TIMES (join) of distinct base tables, each output column's
 * position must be the column's offset in the *concatenated* locator
 * vector produced by WhereCircuit::evaluate(TIMES), not the column's
 * varattno within its own table.  Otherwise a column from the second
 * (or later) join input lands inside the first input's range and the
 * locator reports the wrong table:column.
 */
CREATE TABLE wpj_left  (l1 int, l2 text, l3 int);
INSERT INTO wpj_left  VALUES (1,'a',10),(2,'b',20),(3,'c',30);
SELECT add_provenance('wpj_left');

CREATE TABLE wpj_right (r1 int, r2 text);
INSERT INTO wpj_right VALUES (1,'one'),(2,'two'),(3,'three');
SELECT add_provenance('wpj_right');

/* Output columns, in order:
 *   1. l3       — wpj_left, varattno 3
 *   2. r2       — wpj_right, varattno 2
 *   3. l2       — wpj_left, varattno 2
 *   4. wprov    — function expression (no source)
 *
 * The where-provenance for column 2 (r2) must report wpj_right::2,
 * not wpj_left::2 (which is what the buggy varattno-without-offset
 * code would report).
 */
CREATE TABLE wpj_result AS
  SELECT l3, r2, l2,
    regexp_replace(where_provenance(provenance()),':[0-9a-f-]*:','::','g') AS wprov
  FROM wpj_left JOIN wpj_right ON l1 = r1
  ORDER BY l3;

SELECT remove_provenance('wpj_result');
SELECT * FROM wpj_result;
DROP TABLE wpj_result;

DROP TABLE wpj_left;
DROP TABLE wpj_right;
