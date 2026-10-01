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

DROP TABLE st_r_map;
SELECT remove_provenance('st_r');
DROP TABLE st_r;
