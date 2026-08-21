\set ECHO none
\pset format unaligned

-- The circuit store is outside PostgreSQL's WAL, so a committed
-- transaction's gates can still be in the kernel's page cache when the
-- machine loses power.  The worker forces them out shortly after the
-- last write; provsql.synchronous_commit turns that into a barrier the
-- writing transaction waits on before it commits.

SELECT current_setting('provsql.synchronous_commit') AS synchronous_commit_default,
       current_setting('provsql.wal_logging')        AS wal_logging_default;

SET provsql.synchronous_commit = on;

CREATE TABLE sd_t (name text);
INSERT INTO sd_t VALUES ('alice'), ('bob');
SELECT add_provenance('sd_t');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sd_t; END $$;

-- A transaction that writes to the store, commits, and is read back.
BEGIN;
CREATE TABLE sd_join AS
  SELECT provenance() AS tok FROM (SELECT DISTINCT 1 FROM sd_t) x;
COMMIT;
SELECT remove_provenance('sd_join');
SELECT round(probability_evaluate(tok)::numeric, 4) AS survived_the_barrier
  FROM sd_join;

-- Forcing the store out leaves it consistent.
SELECT dangling_indices, unreferenced, bad_wires, bad_extra FROM check_store();

RESET provsql.synchronous_commit;
DROP TABLE sd_join;
DROP TABLE sd_t;
