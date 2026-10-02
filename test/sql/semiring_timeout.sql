\set ECHO none
\pset format unaligned

-- Regression: the semiring evaluation of a circuit must be interruptible by
-- statement_timeout (and pg_cancel_backend).  The condition on a sum over a
-- group of 20 rows is evaluated by enumerating the 2^20 worlds of the group,
-- for seconds and a formula of about 100 MB; the loops poll the cancel and
-- stop, where they used to run to the end.
CREATE TABLE st_r AS
  SELECT i AS id, 1 AS grp, (i % 7 + 1) / 1000.0::double precision AS v
  FROM generate_series(1, 20) AS i;
SELECT add_provenance('st_r');
SELECT create_provenance_mapping('st_r_map', 'st_r', 'id');

SET statement_timeout = '200ms';
DO $$
BEGIN
  PERFORM length(sr_formula(provenance(), 'st_r_map'))
  FROM (WITH c AS (SELECT grp, SUM(v) AS total FROM st_r GROUP BY grp)
        SELECT grp FROM c WHERE total > 0.001) x;
  RAISE NOTICE 'sr_formula finished';
EXCEPTION WHEN query_canceled THEN
  RAISE NOTICE 'sr_formula cancelled';
END $$;
RESET statement_timeout;

-- The same evaluation under a memory budget: its formula (about 100 MB)
-- exceeds it, and the evaluation stops with an error.
SET provsql.max_memory = '50MB';
DO $$
DECLARE d text; h text;
BEGIN
  PERFORM length(sr_formula(provenance(), 'st_r_map'))
  FROM (WITH c AS (SELECT grp, SUM(v) AS total FROM st_r GROUP BY grp)
        SELECT grp FROM c WHERE total > 0.001) x;
  RAISE NOTICE 'sr_formula finished';
EXCEPTION WHEN program_limit_exceeded THEN
  GET STACKED DIAGNOSTICS d = PG_EXCEPTION_DETAIL, h = PG_EXCEPTION_HINT;
  RAISE NOTICE '% / % / %', SQLERRM, d, h;
END $$;
RESET provsql.max_memory;

-- A condition on an aggregate whose possible worlds have to be enumerated
-- stops past provsql.max_worlds of them, with the same kind of error.
CREATE TABLE st_mw(g int, v int);
INSERT INTO st_mw SELECT 1, i FROM generate_series(1,20) i;
SELECT add_provenance('st_mw');
SELECT set_prob(provenance(), 0.5) FROM st_mw \g /dev/null
SET provsql.max_worlds = 100;
DO $$
DECLARE d text; h text;
BEGIN
  PERFORM probability_evaluate(provenance())
  FROM (SELECT g FROM st_mw GROUP BY g HAVING SUM(v) > 100 AND COUNT(*) > 3) x;
  RAISE NOTICE 'evaluation finished';
EXCEPTION WHEN program_limit_exceeded THEN
  GET STACKED DIAGNOSTICS d = PG_EXCEPTION_DETAIL, h = PG_EXCEPTION_HINT;
  RAISE NOTICE '% / % / %', SQLERRM, d, h;
END $$;
RESET provsql.max_worlds;
DROP TABLE st_mw;

DROP TABLE st_r_map;
SELECT remove_provenance('st_r');
DROP TABLE st_r;
