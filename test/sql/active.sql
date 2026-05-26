\set ECHO none
\pset format unaligned
SET search_path TO provsql_test;

SET provsql.active=0;

SELECT DISTINCT p1.city
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
ORDER BY city;

  SELECT city FROM personnel
EXCEPT
  SELECT p1.city
  FROM personnel p1, personnel p2
  WHERE p1.city = p2.city AND p1.id < p2.id
  GROUP BY p1.city
ORDER BY city;

-- Regression (crash): INSERT ... SELECT into a provenance-tracked table while
-- provsql.active = off must not crash the backend.  The planner hook formerly
-- ran process_insert_select unconditionally; with provenance off the rewritten
-- source carries no provsql column, so the synthesized provsql target entry
-- was left with a NULL expr that the planner then dereferenced (SIGSEGV).
SET search_path TO provsql_test, provsql;
CREATE TABLE active_ins_src(x int);
INSERT INTO active_ins_src VALUES (1),(2),(3);
SELECT add_provenance('active_ins_src');
CREATE TABLE active_ins_dst(x int);
SELECT add_provenance('active_ins_dst');
INSERT INTO active_ins_dst(x) SELECT x FROM active_ins_src WHERE x > 1;
SELECT count(*) FROM active_ins_dst;
DROP TABLE active_ins_src, active_ins_dst;
