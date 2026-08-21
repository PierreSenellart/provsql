# ProvSQL and transactions: what is left

The audit this file used to hold has been acted on. What follows is what
the work left behind: the one place where the implementation departed
from the plan and why, and the properties that are still not there.

The user-facing contract is `doc/source/user/persistence.rst`; the
internals are in `doc/source/dev/memory.rst`.

## What shipped

- Per-relation metadata moved from a fifth mmap file to the
  `provsql.table_info` heap table (`src/table_info.c`), keyed by
  `regclass` so a dump carries the relation's name, and registered with
  `pg_extension_config_dump`. `provsql.migrate_table_info()` imports the
  legacy file.
- Probabilities are written once (`src/probability_store.c`): written on
  a gate that has none, idempotent on the same value, refused otherwise,
  and cleared when the writing (sub)transaction aborts. `set_infos` and
  `set_extra` are written once too. `info1` and `info2` are written once
  *each*, with 0 meaning "nothing recorded", because the two are written
  by different parties: `CertifiedDDMaterialize` marks a gate certified
  as it builds it and tags it with the route that made it a query's root
  afterwards, once the whole d-D exists. The unset marker
  is `NaN` in the record, behind a `gates` file format version that makes
  a version-1 file's ambiguous `1.0` count as unset until a clean-up
  normalises it. `provsql.replace_input` / `replace_block` /
  `replace_update` are how a probability changes; `provenance_guard`
  recognises a replacement leaf and keeps the relation's kind, and every
  mapping built by `create_provenance_mapping` – snapshot ones included –
  follows the token to its replacement.
- `repair_key` records the block size in `info2` instead of writing the
  uniform `1/size` as a probability, so the documented "repair_key then
  `set_prob(provenance(), p)`" is still a first write.
- Store writes are ordered so that an interrupted one leaves an
  unreferenced record rather than a dangling index, and the UUID table's
  rehash goes through a complete new file renamed into place.
  `provsql.check_store()` reports what does not add up.
- The worker forces the store out shortly after the last write;
  `provsql.synchronous_commit` turns that into an at-commit barrier.
- `provsql.circuit_cleanup()` rebuilds the store from what the tokens in
  the database reach, under exclusive access, and doubles as the repair
  tool.
- `provsql.wal_logging` (PostgreSQL 15+) writes each store mutation to
  the WAL through a custom resource manager, so a standby's startup
  process can apply it.
- The store follows the database's tablespace (`GetDatabasePath` rather
  than a hardcoded `base/<oid>`).
- Data modification records the transaction as well as the statement: one
  `update` gate per transaction, `update_provenance.xid` / `.tx_token`,
  `undo()` at either granularity, and commit-time validity through a
  deferred trigger.

## Where the implementation departed from the plan

**No LSN-tracking checkpoint-gap detector.** The plan proposed keeping
the LSN of the last flushed record in the file header and failing to
start when the store is behind the checkpoint redo pointer. Requiring
`provsql.synchronous_commit` for `provsql.wal_logging` removes the gap
instead of detecting it: a transaction cannot commit until its store
writes are on disk, so the store on disk is never behind the WAL, and
crash recovery does not need the records at all. What they are for is
replication and PITR.

## Still not there

- **Reads write.** Every provenance query persists gates, so a `READ
  ONLY` transaction writes to the store, the store grows with ad hoc
  exploration, and a hot standby cannot serve provenance queries (under
  `provsql.wal_logging` its backends refuse the write; without it they
  write to a store that diverges from the primary's). The alternative is
  a different model – a query's gates kept in a session-local overlay and
  persisted only when a token is actually stored in a table – which is a
  redesign of the rewriter and the evaluators, not an increment.
- **No MVCC for the store.** Write-once removes rewrites, but a snapshot
  still sees the future: a `REPEATABLE READ` transaction that started
  before a concurrent `set_prob` on an unset gate reads the new
  probability, and a concurrent session sees an uncommitted probability
  until it is cleared on rollback. Closing this means deferring the write
  to `XACT_EVENT_PRE_COMMIT` behind a backend-local overlay that every
  reader consults, or heap storage for probabilities.
- **No online clean-up.** `circuit_cleanup` needs the database to itself
  because gates are re-created idempotently and a backend's own cache
  answers "it exists" without asking the store. A concurrent collector
  needs per-gate epochs and session pinning: a new on-disk field and a
  cache-invalidation protocol.
- **Two-phase commit excluded.** A transaction that has written a
  probability refuses `PREPARE TRANSACTION`; the undo list lives in the
  backend and a prepared transaction outlives it.
- **`CREATE DATABASE ... TEMPLATE` under `WAL_LOG`** still drops the
  store, and PostgreSQL offers no hook. Only `STRATEGY = FILE_COPY` and
  documentation help.
- **`pg_dump`, logical replication and `pg_upgrade`** do not carry the
  circuit. Closing this needs a logical export / import of the reachable
  circuit – `provsql.dump_circuit()` into a dumpable table and its
  inverse – which the clean-up's mark phase half builds.
- **The at-commit barrier's cost is unmeasured.** One flush per
  store-writing transaction through a single worker would need group
  commit (coalescing the sync requests queued while one flush runs) to
  hold up under concurrency.
- **WAL replay is untested at run time.** Emission and decoding are
  exercised; `rm_redo` is only reachable from crash recovery or a
  standby, neither of which the regression suite can set up.
- **The resource-manager id (151) is claimed in the code but not yet on
  the PostgreSQL wiki.** It was free when
  https://wiki.postgresql.org/wiki/CustomWALResourceManagers was last
  read (128-150 and 241 were taken); the row has to be added there before
  1.13.0 ships, or another extension can take it and a cluster loading
  both will hand one's records to the other during replay.
- **Transaction-level provenance stays eager and single-database.** No
  provenance across databases or foreign data wrappers, no reenactment of
  a transaction that ran before tracking was enabled, and `query` still
  comes from `pg_stat_activity` (the outer call inside a function or a
  prepared statement).
