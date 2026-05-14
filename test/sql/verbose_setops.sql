\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Regression for a backend segfault when provsql.verbose_level >= 20
-- triggered pg_get_querydef() on a rewritten EXCEPT / non-ALL UNION
-- tree. ProvSQL's transform_except_into_join built the RTE_JOIN with
-- NULL eref / joinaliasvars / joinleftcols / joinrightcols (the
-- planner/executor never read those because outer Vars reference the
-- input subqueries directly, but the ruleutils deparser walks rtable
-- and dereferenced the NULL).
--
-- The test runs each rewrite path under verbose_level=20 inside a
-- DO block (which discards any result rows). The NOTICE-level
-- deparser output is suppressed via client_min_messages=WARNING so
-- the expected output stays stable across PostgreSQL versions. A
-- crash in pg_get_querydef would kill the backend; surviving the
-- statement is itself the assertion.

SET provsql.verbose_level = 20;
SET client_min_messages = WARNING;

-- EXCEPT (non-ALL): transform_except_into_join builds the RTE_JOIN
DO $$ BEGIN
  PERFORM provenance() FROM (
    (SELECT DISTINCT city FROM personnel)
  EXCEPT
    (SELECT city FROM personnel WHERE id = 99)
  ) t;
END $$;
SELECT 'except_ok' AS result;

-- UNION (non-ALL): goes through rewrite_non_all_into_external_group_by
DO $$ BEGIN
  PERFORM provenance() FROM (
    (SELECT city FROM personnel WHERE id = 1)
  UNION
    (SELECT city FROM personnel WHERE id = 2)
  ) t;
END $$;
SELECT 'union_ok' AS result;

RESET provsql.verbose_level;
RESET client_min_messages;
