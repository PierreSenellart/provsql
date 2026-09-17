\set ECHO none
\pset format unaligned

SET provsql.provenance = 'where';

/* Where-provenance of a selection with OR / NOT.
 *
 * A column equality A = B lets either value have been copied from the other:
 * the where-provenance of the two columns is merged.  A disjunction is a
 * union, whose where-provenance is the union of those of its arms: a row gets
 * the equalities of the disjuncts it satisfies.  A top-level OR or NOT used to
 * be refused (even without any column equality), and one nested under an AND
 * was ignored.
 *
 *   wpo_l(l1, l2): (1,1) (2,5) (3,9)     wpo_r(r1, r2): (1,7) (5,2) (3,3)
 */
CREATE TABLE wpo_l(l1 int, l2 int);
INSERT INTO wpo_l VALUES (1,1), (2,5), (3,9);
SELECT add_provenance('wpo_l');
CREATE TABLE wpo_r(r1 int, r2 int);
INSERT INTO wpo_r VALUES (1,7), (5,2), (3,3);
SELECT add_provenance('wpo_r');

-- No column equality at all: nothing to merge, and no error any more.
CREATE TABLE wpo_1 AS SELECT l1,
    regexp_replace(where_provenance(provenance()),':[0-9a-f-]*:','::','g') AS wprov
  FROM wpo_l WHERE l1 = 1 OR l1 = 3;
SELECT remove_provenance('wpo_1'); SELECT 'constants' AS q, * FROM wpo_1 ORDER BY l1;

-- l1 = r1 OR l2 = r1, output l1, l2, r1.  The pairs selected:
--   (1,1)x(1,7): both disjuncts hold   -> l1, l2 and r1 all merged
--   (2,5)x(5,2): only l2 = r1          -> l2 and r1 merged, l1 alone
--   (3,9)x(3,3): only l1 = r1          -> l1 and r1 merged, l2 alone
CREATE TABLE wpo_2 AS SELECT l1, l2, r1,
    regexp_replace(where_provenance(provenance()),':[0-9a-f-]*:','::','g') AS wprov
  FROM wpo_l, wpo_r WHERE l1 = r1 OR l2 = r1;
SELECT remove_provenance('wpo_2'); SELECT 'OR of equalities' AS q, * FROM wpo_2 ORDER BY l1;

-- The same under an AND (used to be ignored), and written as JOIN ... ON.
CREATE TABLE wpo_3 AS SELECT l1, l2, r1,
    regexp_replace(where_provenance(provenance()),':[0-9a-f-]*:','::','g') AS wprov
  FROM wpo_l JOIN wpo_r ON r2 > 0 AND (l1 = r1 OR l2 = r1);
SELECT remove_provenance('wpo_3'); SELECT 'nested under AND' AS q, * FROM wpo_3 ORDER BY l1;

-- A conjunction inside a disjunct counts only when the whole disjunct holds:
-- (l1 = r1 AND l2 = r2) OR l2 = r1.  For (1,1)x(1,7), l1 = r1 holds but
-- l2 = r2 does not: the row is selected by l2 = r1 alone, and l1 stays alone.
CREATE TABLE wpo_4 AS SELECT l1, l2, r1,
    regexp_replace(where_provenance(provenance()),':[0-9a-f-]*:','::','g') AS wprov
  FROM wpo_l, wpo_r WHERE (l1 = r1 AND l2 = r2) OR l2 = r1;
SELECT remove_provenance('wpo_4'); SELECT 'AND inside OR' AS q, * FROM wpo_4 ORDER BY l1;

-- NOT: a negated condition copies no value.  NOT (l1 <> r1) selects the rows
-- with l1 = r1, without merging anything.
CREATE TABLE wpo_5 AS SELECT l1, r1,
    regexp_replace(where_provenance(provenance()),':[0-9a-f-]*:','::','g') AS wprov
  FROM wpo_l, wpo_r WHERE NOT (l1 <> r1);
SELECT remove_provenance('wpo_5'); SELECT 'NOT' AS q, * FROM wpo_5 ORDER BY l1;

DROP TABLE wpo_l, wpo_r, wpo_1, wpo_2, wpo_3, wpo_4, wpo_5;
