\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SET provsql.provenance = 'where';

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
 *   1. l3       – wpj_left, varattno 3
 *   2. r2       – wpj_right, varattno 2
 *   3. l2       – wpj_left, varattno 2
 *   4. wprov    – function expression (no source)
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

/* Where-provenance over a relation with a NULL-valued data column.
 *
 * The bug this guards against: identify_token tested "result IS NOT NULL"
 * on the whole matched record, but "RECORD IS NOT NULL" is true only when
 * *every* field is non-null. A provenance-tracked table with any NULL data
 * column (here wpn.c, always NULL) therefore never matched, so its input
 * gates were left with table_name = NULL / nb_columns = -1, and
 * where_provenance crashed with an opaque "construction from null" error.
 * After the fix the row is identified via its (always non-null) provsql
 * column and the locator points at the right table:column.
 */
CREATE TABLE wpn (a int, b int, c int);
INSERT INTO wpn VALUES (1,10,NULL),(2,20,NULL),(3,30,NULL);
SELECT add_provenance('wpn');

CREATE TABLE wpn_other (k int, v text);
INSERT INTO wpn_other VALUES (1,'one'),(2,'two'),(3,'three');
SELECT add_provenance('wpn_other');

CREATE TABLE wpn_result AS
  SELECT a, v,
    regexp_replace(where_provenance(provenance()),':[0-9a-f-]*:','::','g') AS wprov
  FROM wpn JOIN wpn_other ON a = k
  ORDER BY a;

SELECT remove_provenance('wpn_result');
SELECT * FROM wpn_result;
DROP TABLE wpn_result;

DROP TABLE wpn;
DROP TABLE wpn_other;

/* An input gate that belongs to no provenance-tracked relation (e.g. a
 * table that was dropped, or one outside the search_path) must yield a
 * clear error rather than the opaque "construction from null". */
SELECT create_gate(public.uuid_generate_v5(uuid_ns_provsql(), 'wpj-orphan'::text), 'input');
SELECT where_provenance(public.uuid_generate_v5(uuid_ns_provsql(), 'wpj-orphan'::text));
