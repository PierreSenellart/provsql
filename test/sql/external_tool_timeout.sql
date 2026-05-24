\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Regression: a long-running / hanging external compiler must be
-- interruptible by statement_timeout (and pg_cancel_backend).  ProvSQL
-- runs external tools in their own process group and SIGKILLs that group
-- on a pending cancel, so a tool that ignores SIGINT or forks a worker
-- into another process group (as KCBox/Panini does) is still stopped;
-- before that fix such a tool ran to completion and the timeout was
-- silently ignored.
--
-- The test plants a fake "d4" that just sleeps and points
-- provsql.tool_search_path at it, so the compile reliably hangs and the
-- 2s statement_timeout always fires -- a deterministic cancel,
-- independent of machine speed and of whether a real d4 is installed.
--
-- Portability: the fake tool lives under /tmp (accessible to the backend
-- user on both Linux -- where it may be a separate `postgres` user -- and
-- macOS, where /tmp is the world-accessible /private/tmp); it is chmod
-- 755 so the backend can exec it, and uses /bin/sh + sleep, both present
-- everywhere.  tool_search_path is prepended to PATH, so the fake d4 wins
-- over any real one.

\! rm -rf /tmp/provsql_extcancel && mkdir -p /tmp/provsql_extcancel && printf '#!/bin/sh\nexec sleep 30\n' > /tmp/provsql_extcancel/d4 && chmod 755 /tmp/provsql_extcancel /tmp/provsql_extcancel/d4

CREATE TABLE extc (x int);
INSERT INTO extc VALUES (1), (2), (3), (4);
SELECT add_provenance('extc');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM extc; END $$;

SET provsql.tool_search_path = '/tmp/provsql_extcancel';
SET statement_timeout = '2s';

-- The self-join shares each extc(x) leaf across the OR over the other
-- rows, so the per-group circuit is not read-once and 'compilation'
-- genuinely invokes the (fake, hanging) d4.
DO $$
DECLARE st text;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'compilation', 'd4')
      FROM (SELECT a.x FROM extc a, extc b WHERE a.x <> b.x GROUP BY a.x) q;
  EXCEPTION
    WHEN query_canceled THEN
      RAISE NOTICE 'hanging external tool interrupted by statement_timeout';
      RETURN;
    WHEN OTHERS THEN
      GET STACKED DIAGNOSTICS st = RETURNED_SQLSTATE;
      RAISE EXCEPTION 'expected query_canceled, got SQLSTATE %', st;
  END;
  RAISE EXCEPTION 'expected the hanging external tool to be cancelled, but the query completed';
END $$;

RESET statement_timeout;
RESET provsql.tool_search_path;
SELECT remove_provenance('extc');
DROP TABLE extc;
\! rm -rf /tmp/provsql_extcancel
