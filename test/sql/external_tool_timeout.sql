\set ECHO none
\pset format unaligned

-- Regression: a long-running / hanging external compiler must be
-- interruptible by statement_timeout (and pg_cancel_backend).  ProvSQL
-- runs external tools in their own process group and SIGKILLs that group
-- on a pending cancel, so a tool that ignores SIGINT or forks a worker
-- into another process group (as KCBox/Panini does) is still stopped;
-- before that fix such a tool ran to completion and the timeout was
-- silently ignored.
--
-- We plant a fake "d4" that just sleeps and point
-- provsql.tool_search_path at it, so the compile reliably hangs and the
-- 2s statement_timeout always fires -- a deterministic cancel,
-- independent of machine speed and of whether a real d4 is installed.
--
-- Portability: the fake tool lives under /tmp and is chmod 755 so the
-- backend (possibly a separate `postgres` user) can exec it.  On a
-- runner where /tmp is mounted noexec (some WSL setups), exec fails
-- before the tool runs, so the cancellation path cannot be exercised;
-- we probe for that and skip (matching the expected_1 output).

\! rm -rf /tmp/provsql_extcancel && mkdir -p /tmp/provsql_extcancel && printf '#!/bin/sh\ncase "$1" in --provsql-probe) exit 0 ;; *) exec sleep 30 ;; esac\n' > /tmp/provsql_extcancel/d4 && chmod 755 /tmp/provsql_extcancel /tmp/provsql_extcancel/d4

-- Can the backend actually execute the fake tool here?  (Runs it as the
-- same OS user the backend uses; fails on a noexec /tmp.)
\set CANEXEC `/tmp/provsql_extcancel/d4 --provsql-probe >/dev/null 2>&1 && echo true || echo false`

\if :CANEXEC
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
\else
\echo 'SKIPPING external_tool_timeout: fake compiler not executable here (noexec /tmp?)'
\endif

\! rm -rf /tmp/provsql_extcancel
