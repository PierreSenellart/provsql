\set ECHO none
\pset format unaligned

-- PostgreSQL 15+ lets an extension register a resource manager of its
-- own and write WAL records the startup process replays, which is how a
-- physical standby can carry the circuit.  Registration is
-- unconditional -- what makes a WAL stream that already holds these
-- records replayable is having the resource manager present -- while
-- emitting them is opt-in.

SELECT count(*) = 1 AS resource_manager_registered
  FROM pg_get_wal_resource_managers() WHERE rm_name = 'provsql';
SELECT rm_builtin AS is_builtin
  FROM pg_get_wal_resource_managers() WHERE rm_name = 'provsql';

-- Logging requires the at-commit barrier: what keeps replay complete is
-- that the store on disk is never behind the WAL.
SET provsql.wal_logging = on;
SET provsql.synchronous_commit = off;
DO $$
DECLARE t uuid := public.uuid_generate_v4();
BEGIN
  PERFORM create_gate(t, 'input');
  RAISE NOTICE 'logging without the barrier was accepted';
EXCEPTION WHEN others THEN
  RAISE NOTICE 'refused: %', SQLERRM;
END $$;

-- With both, the write goes through and the record is emitted.
SET provsql.synchronous_commit = on;
SELECT pg_current_wal_insert_lsn() AS before \gset
DO $$
DECLARE t uuid := public.uuid_generate_v4();
BEGIN
  PERFORM create_gate(t, 'input');
  PERFORM set_prob(t, 0.5);
  RAISE NOTICE 'logged write accepted, probability %', get_prob(t);
END $$;
SELECT pg_current_wal_insert_lsn() > :'before'::pg_lsn AS wal_grew;

RESET provsql.wal_logging;
RESET provsql.synchronous_commit;
