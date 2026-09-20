\set ECHO none
\pset format unaligned

-- A subquery condition that is not a conjunct of the WHERE clause, and EXISTS
-- read as a value in the select list.  The semijoin drops the rows without a
-- match, which the other disjunct of a combination may license, so the
-- condition is read instead as the atom "count of the body >= 1" over the
-- outer join grouped by the outer rows, combined with the other conditions by
-- the rules of HAVING: a row keeps its own annotation where the regular
-- disjunct holds it, and the semijoin's where only the subquery does.  In the
-- select list the same count comparison is a value, and the explosion of its
-- truth gives the two rows the semantics asks for, annotated with the
-- semijoin's and the antijoin's provenance.

CREATE TABLE sb_div(id int, name text);
CREATE TABLE sb_syn(name text, syn text);
INSERT INTO sb_div VALUES (1, 'NY'), (2, 'CA');
INSERT INTO sb_syn VALUES ('CA', 'NY'), ('CA', 'NYC');
SELECT add_provenance('sb_div');
SELECT add_provenance('sb_syn');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sb_syn; END $$;

-- The rows of sb_div are certain, the two synonyms of CA are present with
-- probability one half each.  NY is licensed by its own name in every world;
-- CA only by the subquery, so it is there in the worlds holding a synonym,
-- 0.75, and with the negated condition in the one world holding none, 0.25.
CREATE TABLE sb_r AS
  SELECT a.id FROM sb_div a
  WHERE a.name = 'NY' OR EXISTS (SELECT 1 FROM sb_syn s WHERE s.name = a.name);
SET provsql.active = off;
SELECT id, round(probability(provsql)::numeric, 6) AS p FROM sb_r ORDER BY id;
SET provsql.active = on;
DROP TABLE sb_r;
CREATE TABLE sb_r AS
  SELECT a.id FROM sb_div a
  WHERE a.name = 'NY'
     OR NOT EXISTS (SELECT 1 FROM sb_syn s WHERE s.name = a.name);
SET provsql.active = off;
SELECT id, round(probability(provsql)::numeric, 6) AS p FROM sb_r ORDER BY id;
SET provsql.active = on;
DROP TABLE sb_r;

-- EXISTS as a value: two rows per outer row, one per truth.  The parent of two
-- children has them at 0.75 (a child present) and 0.25 (none); the childless
-- parent has only its false row, its true one holding in no world.  NOT EXISTS
-- is the same reading with the truths exchanged, and a CASE over the value
-- picks its branch per exploded row.
CREATE TABLE sb_p(pid int);
CREATE TABLE sb_c(pid int, name text);
INSERT INTO sb_p VALUES (1), (2);
INSERT INTO sb_c VALUES (1, 'a'), (1, 'b');
SELECT add_provenance('sb_p');
SELECT add_provenance('sb_c');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sb_c; END $$;
CREATE TABLE sb_r AS
  SELECT p.pid, EXISTS (SELECT FROM sb_c c WHERE c.pid = p.pid) AS has_child
  FROM sb_p p;
SET provsql.active = off;
SELECT pid, has_child, round(probability(provsql)::numeric, 6) AS p
FROM sb_r ORDER BY pid, has_child;
SET provsql.active = on;
DROP TABLE sb_r;
CREATE TABLE sb_r AS
  SELECT p.pid, NOT EXISTS (SELECT FROM sb_c c WHERE c.pid = p.pid) AS childless
  FROM sb_p p;
SET provsql.active = off;
SELECT pid, childless, round(probability(provsql)::numeric, 6) AS p
FROM sb_r ORDER BY pid, childless;
SET provsql.active = on;
DROP TABLE sb_r;
CREATE TABLE sb_r AS
  SELECT p.pid,
         CASE WHEN EXISTS (SELECT FROM sb_c c WHERE c.pid = p.pid)
              THEN 'some' ELSE 'none' END AS lbl
  FROM sb_p p;
SET provsql.active = off;
SELECT pid, lbl, round(probability(provsql)::numeric, 6) AS p
FROM sb_r ORDER BY pid, lbl;
SET provsql.active = on;
DROP TABLE sb_r;

-- Refused, as the semantics says: two subquery conditions in one combination
-- would need the counts of both bodies on one tuple, which the grouping of one
-- of them does not carry into the other; and IN as a value is not the EXISTS
-- line, SQL giving it unknown where no row matches and a comparison is unknown,
-- which a count does not tell from false.
SELECT a.id FROM sb_div a
WHERE EXISTS (SELECT 1 FROM sb_syn s WHERE s.name = a.name)
   OR EXISTS (SELECT 1 FROM sb_syn s WHERE s.syn = a.name);
SELECT a.id, a.name IN (SELECT s.syn FROM sb_syn s) AS is_syn FROM sb_div a;
-- And two EXISTS values in one select list: the decorrelation groups the outer
-- rows once, for one body, so the second is read as a plain value with the
-- warning that says so.
CREATE TABLE sb_r AS
  SELECT p.pid,
         EXISTS (SELECT FROM sb_c c WHERE c.pid = p.pid) AS has_child,
         NOT EXISTS (SELECT FROM sb_c c WHERE c.pid = p.pid) AS childless
  FROM sb_p p;
SELECT remove_provenance('sb_r');
SELECT pid, has_child, childless FROM sb_r ORDER BY pid;
DROP TABLE sb_r;

DROP TABLE sb_div, sb_syn, sb_p, sb_c;
