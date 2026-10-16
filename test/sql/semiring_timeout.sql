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

-- A formula built by few gates, each writing out the sub-circuits it shares
-- with others, grows exponentially with the rows, between too few polls for
-- the periodic check: that of the last row of an ORDER BY ... LIMIT over 17
-- rows is about 30 MB, from about 60 gates.  The budget is checked after
-- each large value too, and the evaluation stops.
CREATE TABLE st_ol(ref text, v float8);
INSERT INTO st_ol SELECT md5(i::text), i FROM generate_series(1,17) i;
SELECT add_provenance('st_ol');
SELECT create_provenance_mapping('st_ol_map', 'st_ol', 'ref');
SET provsql.max_memory = '50MB';
DO $$
DECLARE d text; h text;
BEGIN
  PERFORM length(sr_formula(provenance(), 'st_ol_map'))
  FROM (SELECT v FROM st_ol ORDER BY v LIMIT 10) x;
  RAISE NOTICE 'sr_formula finished';
EXCEPTION WHEN program_limit_exceeded THEN
  GET STACKED DIAGNOSTICS d = PG_EXCEPTION_DETAIL, h = PG_EXCEPTION_HINT;
  RAISE NOTICE '% / % / %', SQLERRM, d, h;
END $$;
RESET provsql.max_memory;
DROP TABLE st_ol_map;
SELECT remove_provenance('st_ol');
DROP TABLE st_ol;

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

-- A circuit too large for what is left of provsql.max_memory is refused
-- while it is built, before the backend loads it, with the same kind of
-- error (GitHub #104): the sum over a cross product of 200 x 200 rows has
-- 40,000 contributions, far past 1 MB.
CREATE TABLE st_ca AS SELECT i AS id, i::float8 AS v FROM generate_series(1, 200) i;
CREATE TABLE st_cb AS SELECT i AS id FROM generate_series(1, 200) i;
SELECT add_provenance('st_ca');
SELECT add_provenance('st_cb');
CREATE TABLE st_cs AS SELECT sum(st_ca.v) AS s FROM st_ca, st_cb;
SELECT remove_provenance('st_cs');
SELECT create_provenance_mapping('st_ca_map', 'st_ca', 'id');
SET provsql.max_memory = '1MB';
DO $$
DECLARE d text; h text;
BEGIN
  PERFORM length(sr_formula(s, 'st_ca_map')) FROM st_cs;
  RAISE NOTICE 'evaluation finished';
EXCEPTION WHEN program_limit_exceeded THEN
  GET STACKED DIAGNOSTICS d = PG_EXCEPTION_DETAIL, h = PG_EXCEPTION_HINT;
  RAISE NOTICE '% / % / %', SQLERRM, d, h;
END $$;
RESET provsql.max_memory;
SELECT length(sr_formula(s, 'st_ca_map')) > 0 AS evaluated FROM st_cs;
DROP TABLE st_ca_map, st_cs;
SELECT remove_provenance('st_ca');
SELECT remove_provenance('st_cb');
DROP TABLE st_ca, st_cb;

DROP TABLE st_r_map;
SELECT remove_provenance('st_r');
DROP TABLE st_r;
