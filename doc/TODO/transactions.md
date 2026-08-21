# ProvSQL and transactions: audit and what is achievable

Audit date: 2026-08-21, against master at 9bf5a8c7 (1.13.0-dev), PostgreSQL
18.4. Every "observed" item below was reproduced on a scratch database on
this host; items marked "by code reading" were not run.

## Summary

- Everything ProvSQL does *through SQL* is already transactional, because
  PostgreSQL's DDL is: `add_provenance`'s `ALTER TABLE` / `CREATE INDEX` /
  `CREATE TRIGGER`, the token rewrites that the data-modification triggers
  perform on user tables, the `update_provenance` log table, the
  `provenance_mapping_registry`, the tool registry. A rolled-back
  `add_provenance` leaves no column, a rolled-back `DELETE` under
  `provsql.update_provenance` leaves the rows' tokens untouched.
- The *only* non-transactional state is the circuit store
  (`$PGDATA/base/<oid>/provsql_*.mmap`, written by the background worker).
  It is outside PostgreSQL's heap, WAL, buffer manager, and catalog, so it
  has none of the four ACID properties with respect to the transaction that
  writes to it: no atomicity (a rolled-back `set_prob` stays), no isolation
  (other sessions see uncommitted probabilities and table metadata), no
  durability guarantee (it is flushed to disk only when the worker exits),
  and it is invisible to every PostgreSQL mechanism that relies on WAL or
  the catalog: crash recovery, streaming replication, PITR, `pg_dump`, and
  (on PostgreSQL 15+) the default `CREATE DATABASE ... TEMPLATE` strategy.
- The structure of the store makes most of this tractable. Gates are
  append-only, content-addressed (`uuid_generate_v5` over the operation and
  the children) or freshly minted (`uuid_generate_v4`), immutable once
  created, and `createGate` is idempotent. A gate created by a transaction
  that later rolls back is therefore an orphan, not an inconsistency: no
  committed row can reference it, and if the same expression is computed
  again the same UUID lands on the same gate. **Gate creation does not need
  undo.** `set_infos` and `set_extra` already follow the same discipline in
  practice: every caller writes them immediately after `create_gate` on a
  gate it owns. The one genuine post-hoc mutation in the circuit is
  `set_prob`, whose documented use (`set_prob(provenance(), p) FROM t`,
  Studio's click-to-edit) rewrites probabilities at will. The per-relation
  TID/BID/OPAQUE metadata and ancestry are mutable too, but they are
  catalog data about relations, not part of the circuit.
- The direction taken here: **make the circuit append-only for real**
  (probabilities written once, idempotent on the same value, cleared on
  rollback like an orphan; a probability *change* becomes a fresh input
  gate that replaces the old token in the rows that carry it, the way the
  data-modification triggers already rewrite tokens), and move the
  relation metadata into a heap table where catalog data belongs. Concretely, in increasing order of
  effort: (A) document the current contract and the template-copy trap;
  (B) write-once `set_prob` + clear-on-abort, and a heap table for the
  relation metadata; (C) make the store durable and crash-atomic (periodic
  or at-commit `fdatasync`, fix two torn-state windows); (D) on PostgreSQL
  15+, WAL-log store mutations through a custom resource manager, which
  extends crash recovery and streaming replication to the store. With the
  store append-only, (D) is conceptually clean: every WAL record is
  "append this fact", replay is trivially idempotent.
- An append-only store needs a way to reclaim space eventually. A
  **circuit clean-up** that removes every gate not reachable from a token
  stored in the database is feasible, but, because gates are
  content-addressed and re-created idempotently, it cannot run
  concurrently with store writers: it is a `VACUUM FULL`-style operation
  taking exclusive access to the database, which also makes it the
  natural rebuild tool for a torn store. See the dedicated section.
- Data-modification provenance (`provsql.update_provenance`) is entirely
  heap-side and already behaves correctly under `ROLLBACK`; what it lacks
  is the transaction as a unit (one update token per transaction,
  commit-time validity, transaction-granularity `undo`). That is a modest,
  purely SQL-side addition; see the dedicated section.

## How the store is written (the facts the audit rests on)

- Backends never touch the mmap files. Every mutation is a message on an
  anonymous pipe to the single worker (`src/provsql_mmap.c`,
  `src/MMappedCircuit.cpp`): `C` create gate, `P` set prob, `I` set infos,
  `E` set extra, `T`/`D`/`A`/`R` table info and ancestry. Reads (`t`, `c`,
  `p`, `i`, `e`, `s`, `a`, `g`, `j`, `n`) are request/reply under the
  exclusive LWLock.
- `C`, `I`, `E`, `T`, `D`, `A`, `R` get **no acknowledgement**; `P` does
  (it reports whether the gate accepted a probability). The pipe is FIFO
  and the worker single-threaded, so a backend's own later read always
  sees its earlier write, and a message sent before a commit is applied
  before any read issued after another session sees the commit. Causality
  is fine; durability is not (next point).
- `MMappedCircuit::sync()` (`msync(MS_SYNC)` on the five files) is called
  from the destructor only, i.e., from `destroy_provsql_mmap` at worker
  exit. Between restarts the data lives in the kernel page cache as dirty
  `MAP_SHARED` pages. A PostgreSQL crash or `pg_ctl stop -m immediate` does
  not lose them (the page cache survives the processes); an OS crash or
  power loss loses whatever the kernel had not written back (dirty pages
  are typically flushed within ~30 s, configurable).
- Gates are created at **execution** time, by reads as much as by writes:
  a plain `SELECT a, provenance() FROM big` over five rows added 11 gates,
  a join 21, a rolled-back `GROUP BY` one. `EXPLAIN` creates none. A
  `READ ONLY` transaction, a parallel worker, or (by code reading) a hot
  standby backend all write to the store, because the SQL functions are
  `IMMUTABLE` / `PARALLEL SAFE` and the write bypasses PostgreSQL.
- Unknown tokens are not errors: `getGateType` returns `input` for a UUID
  that is not in the mapping, `get_children` returns `{}`, `get_prob`
  returns NULL. This lazy default is what lets a freshly `add_provenance`d
  table work without materialising a gate per row. It is also what turns
  every "gate lost" scenario below into **silent** degradation rather than
  an error: a lost `times` gate reads back as an independent input with
  probability 1.

## Findings

Severity: **high** = silently wrong results or data loss; **medium** =
visible anomaly, conservative or recoverable; **low** = cosmetic or
space.

### F1. Gates created by a rolled-back transaction persist (low)

Observed: `get_nb_gates()` 2 → 10 inside `BEGIN; CREATE TABLE; add_provenance;
SELECT ... provenance() ...; ROLLBACK` and still 10 afterwards, with the
table gone. Also every failed statement (error after `provenance_plus`
ran), every `EXCEPT`/`ROLLBACK TO SAVEPOINT`, every Studio notebook cell
that errors. Harmless for correctness (see Summary); a space leak with no
garbage collector. Note that there is no `DELETE` message in the protocol
at all: the store only grows, and not only through rollbacks: every
dropped or truncated tracked table, every `remove_provenance`, every
exploratory query over a big table, every `set_extra` (which appends new
bytes to `provsql_extra.mmap` and abandons the old ones) leaves gates or
bytes nothing references any more. See the circuit clean-up section for
the remedy.

### F2. `set_prob` in a rolled-back transaction is not undone (high)

Observed: `BEGIN; SELECT set_prob(tok, 0.25); ROLLBACK;` →
`get_prob(tok)` = 0.25. `set_infos` and `set_extra` go through the same
non-transactional path, but every caller in the tree writes them right
after `create_gate` on a gate it has just created (the SQL constructors,
`repair_key`, and the compilers in `CertifiedDDMaterialize.cpp`,
`reachability_evaluate.cpp`, `mobius_evaluate.cpp`, all guarded by
content-addressed UUIDs or a per-backend `created` set), so a rolled-back
`set_infos` / `set_extra` only ever annotates an orphan. `set_prob` is
the exception: it is called on pre-existing input gates, possibly more
than once with different values. `repair_key` rolled back: its `ALTER TABLE` is undone
but the key-group input gates and the `mulinput` gates with their
`1/group_size` probabilities stay (orphans, F1-class). A user who loads
probabilities in a transaction and aborts it on a validation error keeps
the half-loaded probabilities; a retry with different values overwrites
them (last writer wins), so the end state is usually right, which is why
this has not been reported.

### F3. Per-relation metadata does not follow the transaction (medium)

All three observed:

- `BEGIN; INSERT INTO t(a, provsql) VALUES (9, uuid_generate_v4()); ROLLBACK;`
  → `get_table_info('t')` is `opaque` although the offending row never
  existed. The safe-query / read-once rewriter is disabled for `t` from
  then on. Conservative direction (plans get slower, never wrong), and
  nothing user-facing resets it except `set_table_info(..., 'tid')`.
- `BEGIN; DROP TABLE u; ROLLBACK;` → table back, `get_table_info('u')` is
  NULL: the `sql_drop` event trigger's `remove_table_info` is not rolled
  back. Conservative again (no record = refuse path), but a BID table loses
  its block-key declaration, so `repair_key` semantics are silently
  forgotten by the rewriter until someone re-declares them.
- `add_provenance` / `repair_key` rolled back → a `tid` / `bid` record for
  a relation that is not tracked (or, after the table is really dropped, for
  a recyclable OID). Harmless today because the planner keys tracking on the
  presence of the `provsql` column, not on the record.

The `provenance_mapping_registry` half of the same cleanup trigger is a
heap table and *does* roll back correctly, which is the model to follow.

### F4. No isolation (medium)

`set_prob`, `set_table_info` and friends are visible to every session the
moment the worker applies them, before the issuing transaction commits.
A concurrent `probability_evaluate` reads a mix of old and new
probabilities if another session is mid-way through a `set_prob` loop.
Two sessions setting the same probability: last message wins, no conflict
detection. This matters little for the typical "load once, query many"
usage, but it is what makes the undo-log design in option C2 below only a
partial answer.

### F5. Durability and crash atomicity of the store (high, narrow windows)

- **No flush before commit.** A transaction commits (WAL fsynced) while
  its gates sit as dirty page-cache pages. After an OS crash, committed
  rows can reference gates that never reached disk; they read back as
  inputs (lazy default), so a join result's `times` becomes an independent
  variable with probability 1. Silent.
- **No acknowledgement for `C`.** A backend commits before the worker has
  necessarily *applied* the message. If the worker dies between the two
  (a `std::runtime_error` from `MappedRegion` on ENOSPC turns into a
  worker FATAL; `bgw_restart_time = 1`), the message is lost. Queued
  messages in the kernel pipe buffer survive a worker-only restart, since
  the postmaster still holds both pipe ends; they do **not** survive a
  postmaster crash-restart cycle: `provsql_shmem_startup` runs again with
  `found == false` and creates fresh pipes.
- **Torn states inside one message.** The worker is killed with SIGQUIT /
  `_exit` on any backend crash (PostgreSQL's crash-restart) and on an
  immediate shutdown, at an arbitrary instruction. Two windows are
  dangerous (by code reading, `MMappedCircuit::createGate`,
  `MMappedUUIDHashTable::add` / `grow`):
  1. `mapping.add(token)` has assigned index `next_value++` but
     `gates.add(...)` has not run. The mapping now has one more entry than
     the gates vector. Every later gate is stored at `gates[nb_elements]`
     while the mapping says `next_value`: an off-by-one for the rest of
     the store's life, so *every subsequently created gate* resolves to
     the wrong record. The window is a few instructions, but it is
     permanent corruption.
  2. `MMappedUUIDHashTable::grow()` rehashes **in place**: it bumps
     `log_size`, overwrites every slot with `NOTHING`, then re-inserts.
     A kill during the re-insert loop loses the mapping for every token
     not yet re-inserted; they all read back as fresh inputs. This window
     is O(n) in the number of gates and occurs once per doubling.
  `MMappedVector::add` itself is safe (element written, then the count
  bumped), and `MappedRegion::remap` + `ftruncate` is safe (growing a file
  zero-fills).
- The comment in `provsql_mmap.c` ("the mmap store is crash-safe, so
  exiting mid-read is fine") is right about *reads*; it is not a statement
  about writes and should not be read as one.

### F6. `CREATE DATABASE ... TEMPLATE` silently drops the store on PG 15+ (high)

Observed on PG 18: `CREATE DATABASE b TEMPLATE a` (default
`STRATEGY = WAL_LOG`) → in `b`, `get_nb_gates()` = 0, a `times` token
stored in a table reads as `input`, probabilities are gone. With
`STRATEGY = FILE_COPY`, `b` has all 18 gates, the `times` gate and the
probability. `WAL_LOG` copies relation forks through shared buffers and
ignores foreign files in `base/<oid>/`; `FILE_COPY` copies the directory.
Nothing in the docs mentions this; anyone cloning a tutorial/case-study
database (the Docker image and Studio's database creation both use
`createdb`, without `-T`, so they are not affected today) gets a database
whose tokens all look like fresh inputs. The same applies to
`ALTER DATABASE ... SET TABLESPACE` (and, by code reading, a database
created in a non-default tablespace has no `base/<oid>/` directory at all,
so `makePath` points nowhere and the worker's `open(O_CREAT)` fails).

### F7. `pg_dump` / restore and logical replication silently degrade (high)

Observed: `pg_dump -t d -t t a | psql d2` (with `provsql` installed in
`d2`): the restored derived token reads as `input` (was `times`),
probabilities are NULL, `get_table_info` is NULL, while the
`provenance_guard` and statement triggers are restored with the table (so
the table is half-tracked: it mints tokens for new rows, but the
rewriter has no TID record). Logical replication has the same shape: rows
and UUIDs replicate, gates do not. Base-table-only workloads survive this
by accident of the lazy default; anything derived (CTAS, `INSERT ...
SELECT`, data-modification provenance, `repair_key`) is lost without a
message.

### F8. Physical replication, hot standby, PITR (high, by code reading)

The store is not WAL-logged, so a streaming standby holds whatever the
base backup copied (a non-atomic snapshot of five files taken at some
point during the backup) and never receives later gates. The worker starts
on the standby (`BgWorkerStart_PostmasterStart`) and hot-standby backends
will happily *create* gates there (reads create gates, see above), so the
standby's store diverges from the primary's and, after promotion, the two
can never be reconciled. PITR restores the files as copied and replays
WAL for the heap only; the probability that the copied files are
internally consistent with each other depends on how much was written
during the copy (see F5's torn-state analysis for what an inconsistency
does). None of this is documented; the user manual has no
backup/replication chapter.

### F9. Savepoints, subtransactions, two-phase commit (low today)

Nothing specific: `ROLLBACK TO SAVEPOINT` behaves like F1–F3 at finer
grain; `PREPARE TRANSACTION` has no ProvSQL state to persist because
nothing is deferred. This changes as soon as any design below keeps
per-transaction state in the backend (C2, D): an undo log or a pending
sync must then be written out at `XACT_EVENT_PRE_PREPARE` or the extension
must refuse `PREPARE TRANSACTION` in transactions that touched it (the
precedent is PostgreSQL refusing it after temporary-table access).

### F10. Documentation claims (low)

- `doc/source/user/studio.rst` ("a failed cell rolls back cleanly"): the
  SQL effects do; gates, probabilities and table metadata set by the cell
  stay. True enough for users, but worth a footnote once the contract is
  written down (option A).
- `doc/source/dev/memory.rst` says the store avoids "WAL overhead" without
  spelling out what is given up; `getting-provsql.rst` says the store "is
  preserved across the upgrade" and nothing about backups.

## What PostgreSQL offers, and why the "DDL is transactional" observation helps

The reason to be hopeful is not that DDL can be rolled back (ProvSQL's DDL
already is), but that everything ProvSQL keeps *in* PostgreSQL is already
correct, and the part it keeps *outside* has a structure that makes
transactional treatment cheap:

- **Immutable, idempotent, content-addressed gates** need no undo and no
  isolation: the only question is durability, and the only cost of
  rollback is space.
- **The only post-hoc mutation of the circuit is `set_prob`**, and
  nothing forces it to be one: a probability written once is just another
  appended fact. The per-relation metadata is the other mutable piece, and
  it is catalog-shaped data about relations; the heap is its natural home,
  and `pg_extension_config_dump` (already used for the tool registry)
  makes `pg_dump` carry it.
- `RegisterXactCallback` / `RegisterSubXactCallback` (`access/xact.h`,
  all supported versions) give per-transaction hooks:
  `XACT_EVENT_PRE_COMMIT` runs inside the transaction (SPI usable, can
  still error out and abort the commit), `XACT_EVENT_ABORT` /
  `SUBXACT_EVENT_ABORT_SUB` run on rollback (no SPI, but a pipe write is
  fine), `XACT_EVENT_PREPARE` / `PRE_PREPARE` cover two-phase commit, and
  the `PARALLEL_*` variants distinguish parallel workers.
- `SET LOCAL` GUCs and `set_config(..., true)` are transaction-scoped and
  roll back with the transaction; a cheap place to hold a per-transaction
  token (see last section).
- `RegisterCustomRmgr` (`access/xlog_internal.h`, **PostgreSQL 15+**,
  requires `shared_preload_libraries`, which ProvSQL already requires)
  lets an extension write its own WAL records (`XLogBeginInsert` /
  `XLogRegisterData` / `XLogInsert`) and supply `rm_redo`, which the
  startup process calls during crash recovery, on a streaming standby,
  and during PITR. This is the mechanism that would bring the store under
  WAL without putting it in shared buffers. Custom rmgr IDs (128–255) are
  claimed on the PostgreSQL wiki's registry page.
- What PostgreSQL does **not** offer an extension: a checkpoint hook (the
  sync-request queue handlers are a fixed enum), and any way to make a
  non-relation file part of `CREATE DATABASE ... WAL_LOG`, base backups'
  consistency, or `pg_dump`. Those have to be worked around (see D and
  the notes under it) or avoided by moving data into relations.

## Options, by increasing effort

### A. Document the contract and the traps

A "Persistence, backups and replication" page in the user manual:
the store is outside WAL; use `STRATEGY = FILE_COPY` for template copies;
`pg_dump` does not carry circuits, only tokens (and what that means);
physical replicas do not carry the store; stop the server cleanly before
file-level backups; transactions do not roll back `set_prob` or table
metadata. Plus a NOTICE-level hint somewhere cheap, e.g., `get_nb_gates()
= 0` while tracked tables exist is a reliable sign of F6/F7. Also fix the
"crash-safe" comment and the `getProb` docstring ("1.0 if not found";
it returns NULL).

### B. Make the circuit append-only; relation metadata to the heap (the real fix)

**B1. Per-relation metadata** (`provsql_table_info.mmap` → a table
`provsql.table_info(relid oid primary key, kind, block_key int2[],
ancestors oid[])`, registered with `pg_extension_config_dump`). The
planner-side caches (`provsql_lookup_table_info`, `provsql_lookup_ancestry`)
stay as they are, only the miss path changes from a pipe round-trip to an
index lookup (`SPI` or, better, a direct `systable_beginscan` on the
table's primary key to stay out of SPI in the planner hook). The
`sql_drop` trigger's `remove_table_info` becomes a `DELETE`; the guard
trigger's OPAQUE flip becomes an `UPDATE`; all of F3 disappears, F7 gets
the metadata back, logical replication carries it, and the worker loses
seven opcodes and one file. The upgrade script seeds the table from the
existing mmap file (one `s`/`a` query per tracked relation, the same way
1.5.0 → 1.6.0 seeded the file from the catalog). There is no foreign
key onto `pg_class`, so the `sql_drop` event trigger stays to delete the
row when a tracked table is dropped; its effect simply becomes
transactional.

**B2. Write-once probabilities (keep the store append-only).**
Rule: `set_prob(token, p)` succeeds if the gate has no probability yet,
is a no-op if it already holds exactly `p` (so setup scripts and notebook
cells stay re-runnable, the same idempotence `create_gate` and
`add_provenance` offer), and raises otherwise ("probability of <token>
already set to <q>; probabilities are written once"). A probability then
has the same status as a gate: a fact appended to the circuit. The
consequences:

- *Unset marker.* Today `GateInformation::prob` defaults to `1.0` and a
  lazily materialised input (any query over the table creates it) is
  indistinguishable from one explicitly set to 1. The tutorial runs
  queries before `set_prob`, so "gate exists with prob 1.0" cannot mean
  "set". Use `NaN` as the unset marker in the record: lazy creation and
  `createGate`'s child placeholders write `NaN`; `getProb`,
  `createGenericCircuit` (the `result.setProb` path) and the `p` reply
  map `NaN` to the evaluation default 1.0. Existing files hold `1.0` for
  both cases; on open, a file whose header version is 1 is treated
  leniently (`1.0` accepted as unset, i.e., today's behaviour for those
  gates), new writes use `NaN`, and the header version is bumped to 2 so
  the lenient mode applies exactly once. The ambiguity for gates written
  under version 1 persists until the circuit clean-up rewrites the
  file: its sweep normalises `1.0` on input-type gates to `NaN`, after
  which the store is unambiguous and the lenient mode no longer applies.
  No layout change, so the ABI warning in `MMappedCircuit.h` is
  respected.
- *Clear on rollback.* A rolled-back `set_prob` must not pin the
  probability forever (that would make F2 worse: the retry would be
  refused). Register an xact callback: `set_prob` records
  `(token, current subtransaction id)` in a backend-local list when it
  actually wrote; `XACT_EVENT_ABORT` and `SUBXACT_EVENT_ABORT_SUB` send
  `P` with `NaN` for the tokens of the aborted (sub)transaction; commit
  and `SUBXACT_EVENT_COMMIT_SUB` drop or re-parent the entries. No old
  value is needed, because there never was one: this is an undo log of
  tokens, not of values. `XACT_EVENT_PRE_PREPARE` refuses
  `PREPARE TRANSACTION` when the list is non-empty. `set_prob` becomes
  `PARALLEL RESTRICTED` so the list lives in the leader (a parallel worker
  ends its own transaction before the leader decides).
- *Residual isolation anomaly.* Another session can read the probability
  between the write and the rollback. Acceptable for the "load once,
  query many" usage; if it ever matters, the write can be deferred to
  `XACT_EVENT_PRE_COMMIT` with a backend-local overlay consulted by
  `get_prob` and patched onto the in-memory circuit after
  `createGenericCircuit` (one place: `getGenericCircuit` /
  `getBooleanCircuit` in `CircuitFromMMap.cpp`). Not proposed now.
- *Who writes probabilities today.* `repair_key` (once, at creation),
  the joint-width compiler's synthetic stick-breaking coins
  (`CertifiedDDMaterialize.cpp:178`, once, content-addressed: a
  recompilation rewrites the same value, hence the idempotent case must
  compare values, not just existence), the documented
  `set_prob(provenance(), p) FROM t`, and Studio's inspector
  (`POST /api/set_prob`, `editProbability` in `circuit.js`). Studio's
  click-to-edit must become "set if unset": show the probability
  read-only once it is set, and let the SQL error through otherwise
  (the endpoint already forwards `sqlstate` and the message). Changing
  a probability is done by **replacing the input** (B2', next), and the
  error message of a refused `set_prob` should say so. An in-place
  override, if one is ever wanted, should be a separate, documented
  non-transactional superuser call rather than the default path.
- *`set_infos` / `set_extra` get the same check.* They are write-once by
  convention today (every caller writes them right after `create_gate`);
  making the worker refuse a second write with a different value (and
  accept an identical one) costs a comparison and turns the convention
  into a guarantee. `I` and `E` gain a one-byte reply like `P` so the
  backend can raise.
- *Validation.* Implement the check as a `WARNING` first and run
  `make installcheck`: 25+ regression files call `set_prob`, and any test
  that rewrites a probability shows up as a warning in the diffs. The
  `extension_upgrade` canary and the version-1 lenient mode need one test
  each.

**B2'. Changing a probability: replace the input instead of mutating it.**
Under write-once, the question "how does a user change a probability"
gets the answer the rest of ProvSQL already gives for changing a tuple:
the token column of the base table is the place of truth, and the
circuit keeps history. `provsql.replace_input(old uuid, p float8)
RETURNS uuid` mints a fresh `v4` input gate, sets its probability (a
first write, so B2 applies: the gate and its probability roll back as
an orphan), and returns it; the caller stores it:

```sql
UPDATE s SET provsql = provsql.replace_input(provsql, 0.3) WHERE id = 42;
```

What this gives and costs:

- *Fully transactional.* The heap update has MVCC isolation, WAL,
  replication and `pg_dump`; the new gate is append-only and its
  probability is cleared on rollback. No state in the store is ever
  rewritten.
- *History is kept, derivations are not rewritten.* The old input gate,
  its probability, and every derived gate built over it stay as they
  were. Re-running a query over the base table produces new
  content-addressed derived gates over the new token automatically
  (their UUIDs hash the children), so anything recomputed sees the new
  probability; a *materialised* derived table (`CREATE TABLE ... AS
  SELECT provenance()`) keeps the old tokens and the old probability.
  This is the same behaviour a `DELETE` under `provsql.update_provenance`
  has today (it rewrites the base table's tokens, and copies made earlier
  are untouched), so it is a rule users already live with, to be stated
  once in the documentation: "a derived table reflects the base tables
  as they were when it was built".
- *The guard trigger must cooperate.* `provenance_guard` flips a table to
  OPAQUE when `provsql` changes on `UPDATE`, because it cannot tell a
  user-supplied arbitrary UUID from an independent fresh input. A
  replacement *is* an independent fresh input, so TID-ness is preserved
  by construction. `replace_input` records the minted token in a
  backend-local set (a C helper `provsql.is_fresh_input(uuid)` consumes
  it); the guard's `UPDATE` branch then keeps the table's kind and, for
  the maintained mappings registered on the table, copies the old
  token's `(value, provenance)` rows to the new token (the same job its
  `INSERT` branch does for a new row). The targetlist is evaluated before
  `BEFORE ROW` triggers fire, so the token is in the set when the guard
  sees the row. A procedural form, `replace_input(tbl regclass, old uuid,
  p float8)` doing the `UPDATE` itself under a guard-bypass GUC like the
  DML triggers do, is an alternative with the same semantics.
- *Optional logging.* Under `provsql.update_provenance`, the replacement
  can be recorded in `update_provenance` as a modification of its own
  (`query_type = 'REPLACE'`, the old token's validity ended and the new
  one's begun), which gives the temporal semiring the right reading and
  makes `undo` applicable. Not required for the mechanism.
- *Studio.* The inspector already resolves an input gate to its tracked
  row(s) (`resolve_input`), so click-to-edit on a gate whose probability
  is set becomes "replace": issue the `UPDATE` on the resolved row, and
  refuse (with the reason) when the token resolves to several rows or
  tables, or to none (anonymous Bernoullis minted by hand or by
  `provsql.mixture`). On an unset gate it stays a plain `set_prob`.
- *The other leaf kinds follow the same pattern.* A `mulinput` block
  (`repair_key`) encodes `1/group_size` per row under a shared key
  token; a change is a change of the whole block, so `replace_block(tbl,
  key values...)` re-mints the key token and one `mulinput` gate per row
  with the new probabilities and rewrites the block's rows in one
  `UPDATE` (the second pass of `repair_key`, restricted to one group).
  An `rv` leaf is replaced by constructing a new distribution, which is
  already how `random_variable` columns are updated today; the guard
  cooperation above must accept those fresh `rv` tokens as well. A
  probability on an `update` gate ("how likely is it that this
  modification happened") is replaced by minting a new `update` gate
  with the new probability, inserting its `update_provenance` row, and
  rewriting the rows whose tokens carry the old one, which is the walk
  `undo` already performs over every tracked table; expose it as
  `replace_update(old uuid, p float8)`.

**B3. Alternative: probabilities in a heap table** (`provsql.probability
(token uuid primary key, prob float8)`, evaluators fetching leaf
probabilities in one query per evaluation). Gives MVCC isolation, WAL,
replication and `pg_dump` for probabilities, and allows rewriting. Rejected
in favour of B2: it reintroduces per-row heap writes on the load path,
splits the circuit across two stores, and keeps probabilities mutable,
which is the property B2 removes on purpose. Worth revisiting only if the
residual isolation anomaly of B2 turns out to matter.

### C. Make the store durable and crash-atomic

**C1. Bounded-loss durability.** Replace the worker's blocking `read()`
with `poll()` + timeout and `fdatasync` the five files when the pipe has
been idle for a short while or every N ms under load (`fdatasync` on the
descriptor, not `msync` on the mapping: it flushes the file's dirty
page-cache pages whoever dirtied them and does not walk the mapping). This
is `synchronous_commit = off` semantics for the store: an OS crash loses at
most the last interval. Cheap, no protocol change.

**C2. At-commit durability and the acknowledgement barrier.** A new
request/reply message `S` ("sync"): the backend sends it from
`XACT_EVENT_PRE_COMMIT` when it has written to the store in this
transaction (a backend-local flag set by `provsql_internal_create_gate`
and friends), the worker processes it after every earlier message (FIFO),
`fdatasync`s, replies. Because the reply comes after the earlier messages
are *applied*, this also closes the "committed before applied" hole of
F5, at the price of one fsync per store-writing transaction, which is
what the heap pays for its WAL anyway. Gate it behind a GUC
(`provsql.synchronous_commit`, default off = C1 behaviour, or default on
once measured). Must also run at `XACT_EVENT_PRE_PREPARE`. Note that
read-only queries that create gates would then fsync at their commit; if
that is too costly the flag can be set only by the explicitly mutating
functions (`set_prob` once B2 is not done, `create_gate`, the DML
triggers) and not by `provenance_plus` / `provenance_times`, accepting F5's
first bullet for read-created gates, which are reproducible from the
query.

**C3. Torn-state windows** (F5, third bullet):

- `createGate`: append the `GateInformation` (and its wires) *before*
  publishing the mapping entry, so a kill between the two leaves an
  unreferenced record rather than a dangling index. `MMappedUUIDHashTable::add`
  then needs a two-step API (reserve index, publish), or `createGate`
  can compute the index as `gates.nbElements()` and assert it equals
  `next_value`. The slot's `value` store is an aligned 8-byte write, so
  publication is atomic on the supported platforms.
- `grow()`: build the rehashed table in a **new file**, `fdatasync`, then
  `rename(2)` over the old one (atomic on POSIX) and remap; or keep the
  old table intact and write the new one at the end of the same file,
  switching a header pointer last. Either way a kill mid-rehash leaves a
  complete old or a complete new table.
- A `dirty` flag in the header (set on open for write, cleared on clean
  close) plus a `provsql.check_store()` function that verifies
  `mapping.next_value == gates.nbElements()` and that every mapped index
  is in range, so a torn state is at least reported instead of silently
  shifting every later gate.
- Crash-restart of the postmaster: either keep the pipes across restarts
  (stash the descriptors in the postmaster, not only in shared memory, so
  `provsql_shmem_startup` can reuse them) or accept that C2's barrier is
  what makes the loss of queued messages harmless (nothing in them was
  committed).

**C4. Store location and non-default tablespaces.**
`MMappedCircuit::makePath` hardcodes `$PGDATA/base/<oid>/`. A database
created `TABLESPACE ts` lives under `pg_tblspc/<tsoid>/PG_<ver>_<cat>/<oid>/`
and has no `base/<oid>/` directory, so the worker's `open(O_CREAT)` fails
with `ENOENT`, the worker exits, and every ProvSQL call in that database
errors until it restarts and fails again. Resolve the directory through
`GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace)` (`common/relpath.h`),
passing the tablespace OID along with the database OID in the IPC header
(or resolving it in the worker from `pg_database`, which it cannot read
without a transaction; the header is simpler). The same path then follows
`ALTER DATABASE ... SET TABLESPACE`, which copies the directory. Small,
independent of everything else.

### D. WAL-log the store with a custom resource manager (PG 15+)

The IPC messages are already a serialised, self-describing record format
(opcode, database OID, payload). The design is to write each
store-mutating message into PostgreSQL's WAL *before* writing it to the
pipe, and to replay it from WAL:

- Backend: `XLogBeginInsert(); XLogRegisterData(msg, len); XLogInsert(RM_PROVSQL_ID, opcode)`
  then the pipe write as today. Records are not tied to the commit: like
  an nbtree page split they are applied whether or not the transaction
  commits, which is exactly the semantics gate creation already has.
- `rm_redo` (startup process): forward the payload to the worker through
  the pipe, keeping the worker the single writer (the worker starts at
  `PostmasterStart`, so it is up during recovery), or apply directly if
  the worker is not available. Replay is idempotent
  (`createGate` on an existing token is a no-op; `setProb` overwrites),
  so replaying from an LSN older than the store's state is harmless.
- Durability then no longer depends on the page cache: a committed
  transaction's records are in the fsynced WAL. Streaming replication
  ships them and the standby's worker applies them. `pg_rewind`
  works at the WAL level. Hot-standby backends must refuse to *write*
  (`RecoveryInProgress()` check before every pipe write), which means
  provenance-computing queries on a standby fail unless every gate they
  need already exists; supporting them would need a session-local overlay
  store (future work, orthogonal).
- **The checkpoint gap.** Crash recovery replays from the last completed
  checkpoint's redo pointer R and PostgreSQL recycles WAL before R. For
  replay to be complete the files on disk must contain every record with
  LSN < R. PostgreSQL gives extensions no checkpoint hook, so the worker
  has to get there on its own: keep the LSN of the last fsynced record in
  the file header (C1 gives the periodic fsync; the backend can pass the
  record's LSN in the message), and at `rm_startup` compare the header LSN
  with R: if the header is behind, stop with a clear error ("ProvSQL store
  behind WAL; restore from backup or rebuild") instead of replaying an
  incomplete history. With C1's interval in the hundreds of milliseconds
  the gap is only reachable if the worker stalls across a full
  checkpoint, which is rare but not impossible; fail-stop makes it safe.
  (The alternative that removes the gap entirely is to put the store in
  shared buffers, i.e., in relation files: that is the "regular tables"
  design `memory.rst` moved away from, or a custom access method / generic
  WAL pages, which is a rewrite.)
- Base backups and PITR: `pg_basebackup` copies the five files at
  different instants; replay from the backup's start checkpoint is
  idempotent only if the copied files are *mutually* consistent (see F5's
  torn-state analysis). C3's ordering makes "gates ahead of mapping" the
  only possible skew, which replay repairs (the mapping entry is re-added
  and points to a new, duplicate record; the old one is unreferenced).
  With C3 done, PITR becomes supportable; without it, document it as
  unsupported.
- Versions: PG 10–14 keep today's behaviour (the records simply are not
  emitted); the code is `#if PG_VERSION_NUM >= 150000`.
- Cost: WAL volume is one record per gate (tens of bytes + 16 bytes per
  child), comparable to the heap row that references it; the
  `XLogInsert` call is the same cost PostgreSQL pays for any index insert.
  The ICDE benchmark's argument against tables was lock contention in
  the PL/pgSQL gate inserts, not WAL volume, so this should not undo the
  mmap design's gains, but it needs measuring.

### E. Back to heap storage for the whole circuit (rejected)

Full ACID for free, at the performance cost documented in `memory.rst`.
Not proposed; B captures the part of it that matters.

### Recommended order

1. **B2 + B1** (write-once probabilities with clear-on-abort; relation
   metadata in a heap table). Makes the circuit append-only, fixes every
   rollback anomaly a user can observe (F2, F3, most of F4), and gets the
   relation metadata into `pg_dump`. Independent of PostgreSQL version.
2. **A** alongside, as the release note.
3. **C1 + C3** (bounded-loss durability, torn-state fixes, `check_store`).
   Small, removes the silent-corruption windows.
4. **C2** behind a GUC, measured.
5. **Circuit clean-up** (next section) once the store is append-only;
   it doubles as the rebuild tool for C3 and is independent of D.
6. **D** when there is demand for replicas carrying provenance; it is the
   only path to that, and B + C are prerequisites for it anyway.

## Circuit clean-up: removing what nothing references

The store only grows (F1). Orphans from rolled-back transactions are the
smallest contributor; dropped and truncated tables, `remove_provenance`,
re-loaded datasets, exploratory joins over large tables, and the dead
bytes `set_extra` leaves behind are the bulk. A clean-up step is the
complement of "append-only": the one operation allowed to remove
things, run explicitly, the way `VACUUM FULL` is the complement of MVCC.

### What "referenced" means

A gate is live if it is reachable, through `wires`, from a **root**. The
roots are the tokens stored in the database:

- every value of a `uuid`, `agg_token` (its UUID part), or
  `random_variable` column, and arrays of these, in every table and
  materialised view of the database. Not only columns named `provsql`:
  `CREATE TABLE ... AS SELECT provenance()` stores tokens under whatever
  name the user picks, mapping tables created by
  `create_provenance_mapping` use `provenance`, `update_provenance` uses
  `provsql`, and users store `get_children` results, conditioning
  events, and hand-minted tokens wherever they like. Scanning by *type*
  rather than by name is the only conservative choice; a clean-up that
  misses a root silently turns a derived gate back into an input
  (the lazy default, F7), which is the failure mode to avoid at all
  costs;
- the semiring constants `gate_zero` and `gate_one`. The planner emits
  their UUIDs as literals (`uuid_generate_v5(ns, 'one')`); if their gates
  disappear they read back as *inputs*, not as constants, so they are
  kept unconditionally. The same holds for any future gate whose UUID is
  a fixed literal in the SQL or C side;
- nothing else. Tokens that live only outside the database (a UUID copied
  into a notebook cell or a Studio deep link, a `tseytin_cnf_mapping`
  output kept in a file, a `jsonb` or `text` column holding UUIDs as
  strings) are **not** roots. Content-addressed gates come back by
  re-running the query that produced them; `v4` gates (an `rv` leaf, an
  `update` gate of a dropped `update_provenance` row, the input gate of
  a row deleted from an untracked copy) do not. This must be stated in
  the documentation in exactly those words.

Everything reachable from a root is kept with its `prob`, `info1` /
`info2`, and `extra`; the per-relation metadata is untouched (it is keyed
by relation, not by gate, and B1 moves it to the heap anyway).

### Why it cannot run concurrently

Two properties of the store rule out a background or concurrent
collector:

1. **Idempotent re-creation.** A query running concurrently computes
   `provenance_plus(a, b)`, whose UUID is a content-addressed hash. If
   that gate exists as an orphan, `createGate` finds the token in the
   mapping and returns: the query's row then references a gate the
   collector, which saw no reference, removes a moment later. Marking
   the gate as "recently touched" does not help either, because the
   backend's per-session caches (`CircuitCache`, the compilers'
   `created` sets) answer "exists" without contacting the worker at all.
2. **Invisible references.** A transaction in progress has created gates
   and inserted rows the collector's snapshot cannot see. The rule "keep
   every gate appended after the snapshot" covers gates it *created*, not
   orphans it *adopted* through point 1, and it also requires a gate
   index horizon that the store does not record per transaction.

So the clean-up takes **exclusive access to the database**, enforced the
way `DROP DATABASE` enforces it (`dropdb()` in `dbcommands.c`):

1. `LockSharedObject(DatabaseRelationId, MyDatabaseId, 0, AccessExclusiveLock)`,
   held until the end of the call. Every new connection takes this lock
   in `RowExclusiveLock` mode during its startup transaction
   (`postinit.c`), so new sessions block until the clean-up finishes;
   the same lock also excludes a concurrent `CREATE DATABASE ... TEMPLATE`
   or `DROP DATABASE`. (The startup lock is released once the connection
   is up, so it does not detect *existing* sessions; that is the next
   step.)
2. `CountOtherDBBackends(MyDatabaseId, ...)` (`storage/procarray.h`):
   if any other backend is connected to this database, raise the same
   "database is being accessed by other users" error `DROP DATABASE`
   raises, listing the count. The function already terminates autovacuum
   workers and waits for them. The operator disconnects the others
   (or `pg_terminate_backend`s them) and retries; no waiting inside the
   call, so there is nothing to time out.

Within that window there is no writer, no reader, and no stale cache
except the calling backend's own, which it clears. This is the same
operational contract as `CREATE DATABASE ... TEMPLATE` or `VACUUM FULL`
on a busy table, and it is appropriate for a step run after a bulk
reload or a clean-up of experiments, not on a schedule. The
single-process build (Playground) satisfies it trivially.

### Mechanics

`provsql.circuit_cleanup(dry_run boolean DEFAULT false)` returning
`(gates_before, gates_after, wires_before, wires_after, extra_bytes_before,
extra_bytes_after)`:

1. **Lock** as above, then flush and clear this backend's caches.
2. **Collect roots**: from `pg_attribute`, every column of the root
   types in every relation of kind `r`, `m`, `p` (partitioned parents
   through their children), excluding `provsql`'s own catalogue tables
   that hold no tokens; one `SELECT DISTINCT` per table through SPI,
   `unnest` for arrays, the UUID field for `agg_token`. Roots go into a
   backend-local hash set; at one UUID per row this is 16 bytes per
   tracked row, fine for tens of millions, and a spill to a temporary
   file is possible if ever needed.
3. **Mark**: open the five files read-only in the backend
   (`MMappedCircuit(oid, read_only = true)` exists for this purpose) and
   BFS from the roots over `wires`, producing the live gate index set.
   With exclusive access the worker is idle, so a read-only mapping in
   the backend is coherent; alternatively ship the root set to the
   worker and let it mark, which is the same code. The mark phase alone,
   without the lock, is also a useful read-only estimate
   (`dry_run = true` reports the counts and skips steps 4–6; without the
   lock the numbers are approximate, which is fine for an estimate).
4. **Sweep by rewriting**: build new `mapping`, `gates`, `wires`,
   `extra` files side by side (`provsql_*.mmap.new`), copying live gates
   in index order with remapped child indices, live wires only, and only
   the `extra` bytes the live gates point to (the dead bytes from
   overwritten `set_extra` disappear here too). The new mapping is built
   fresh at the right size, so the hash table is also rehashed and
   compacted. `fsync` each new file.
5. **Swap**: ask the worker (new opcode, request/reply) to close its
   `MMappedCircuit` for this database; write a marker file
   `provsql_cleanup.commit`; `rename` the five `.new` files over the old
   ones; remove the marker. On open, the worker finishes an interrupted
   swap if the marker exists (rename whatever `.new` files remain) and
   discards stray `.new` files if it does not, so a crash at any point
   leaves either the old or the new store. The worker reopens lazily on
   the next message.
6. **Report** and release the lock.

Complexity is linear in the store size plus the root scan, memory linear
in the live set. A store with a high dead fraction gets smaller files
and a denser hash table, so lookups speed up as a side effect.

### What it also buys

- **Rebuild after a torn state (F5).** Steps 3–5 read only what is
  reachable from the mapping and rewrite it consistently; with a
  consistency check added to the mark phase (every mapped index in range,
  `next_value == gates.nbElements()`, every wire index in range), the
  same function is the repair tool for a store damaged by a crash
  mid-write, and `dry_run = true` is the `check_store()` of C3.
- **Format migrations.** A rewrite that already remaps every record is
  the place to change an on-disk layout when one is ever needed, and it
  is where the version-1 probability ambiguity of B2 is normalised away.
- **Template copies and backups.** A compacted store is what one wants
  to copy with `STRATEGY = FILE_COPY` (F6) or before a file-level backup.

### Interaction with the other options

- **B2 (write-once probabilities)** is unaffected: a probability is an
  attribute of a live gate and travels with it.
- **C2's sync barrier** is unnecessary during the clean-up (exclusive
  access) and the new files are fsynced before the swap.
- **D (WAL-logged store)**: a compaction cannot be described as an
  append, so it needs its own treatment. Two workable designs: (i) log
  a `reset` record followed by one `create` record per live gate (with
  its `prob` / `infos` / `extra`), so a standby replays the rebuild from
  empty; WAL volume equals the live store, which is the same order as
  shipping the files, and no new replay logic is needed beyond `reset`;
  or (ii) declare that a clean-up invalidates physical replicas' stores,
  to be re-synced by a fresh base backup, as `VACUUM FULL` of an unlogged
  table effectively does. (i) is cleaner and not much more work once D
  exists; without D, nothing is needed.
- **Logical replication / `pg_dump`** (F7) are unaffected, since they do
  not carry the store in the first place.

### Scheduling and safety rails

- Refuse to run inside a transaction block that has already written to
  the store in this session (the backend-local flag of C2), and refuse
  on a standby.
- `dry_run` first, then the real run; log the counts at `NOTICE`.
- A GUC-free design: no automatic trigger. If an automatic policy is ever
  wanted, the right hook is a size threshold reported by
  `get_nb_gates()` versus the dry-run live count, surfaced in Studio's
  schema panel, and the run itself left to the operator because of the
  exclusive-access requirement.

## Data-modification provenance (`provsql.update_provenance`) and transactions

Everything this feature does is heap-side: the statement-level triggers
(`insert_statement_trigger`, `delete_statement_trigger`,
`update_statement_trigger`, PG 14+, `sql/provsql.14.sql`) mint an
`update` gate, insert a row into `update_provenance`, re-insert the
deleted rows, and rewrite each affected row's `provsql` to
`provenance_monus(old, tok)` / `provenance_times(old, tok)`. Observed on
PG 18 with `update_provenance = on`:

- `BEGIN; DELETE FROM e WHERE id = 3; ROLLBACK;` → inside the
  transaction row 3 reads `monus`, the log has one `DELETE` row; after
  the rollback row 3 is back to its `input` token and the log is empty.
  Only the gates remain (2 → 11: the update gate, the monus, `one`, and
  the inputs materialised along the way), which is F1. The GUC toggling
  the triggers do with `set_config(..., false)` is transactional too, so
  an error inside a trigger cannot leave `provsql.update_provenance` off.
- `BEGIN; DELETE ... id = 1; UPDATE ... id = 2; COMMIT;` → two unrelated
  `update` tokens in the log, one per statement; rows 1 and 2 read
  `x ⊖ 𝟙` under `sr_formula` (update gates evaluate to 1 there), row 20
  is `times(old, update_token)`. Nothing ties the two to the same
  transaction: `undo` can reverse either statement but not "the
  transaction", and a reader of `update_provenance` cannot even tell they
  were one.
- `ts` and `valid_time` come from `CURRENT_TIMESTAMP`, i.e., transaction
  *start*, so two overlapping transactions can commit in the opposite
  order of their recorded validity; `query` comes from
  `pg_stat_activity`, which is the top-level statement text (fine in
  psql, the outer call inside a function or prepared statement).
- Two notions of undo coexist and are consistent: PostgreSQL's
  `ROLLBACK` removes the modification *and its record* (the transaction
  never happened), ProvSQL's `undo(tok)` appends a compensating `update`
  gate and keeps the history (the modification happened and was
  reversed). This is the right split; a transaction token (below) gives
  `undo` the same granularity `ROLLBACK` has.
- Concurrency, by code reading: the "deleted rows stay in the table"
  model is implemented by a physical delete followed by a re-insert with
  the monus token, inside the statement trigger. A concurrent
  `READ COMMITTED` transaction blocked on the same row sees, after the
  first commits, the original row version gone and the re-inserted copy
  invisible to its snapshot, so its own `DELETE` affects zero rows but
  still fires the statement trigger, logging an `update` gate that
  touches nothing. Under `REPEATABLE READ` it gets a serialization
  failure. Ordinary PostgreSQL behaviour, but worth a sentence in the
  data-modification chapter, whose "Limitations" section currently says
  only "experimental".
- The store-side items above apply unchanged: the `update` gates and
  `monus` / `times` gates are append-only and content-addressed or
  `v4`, the rewritten tokens live in the heap, so B/C/D cover this
  feature without anything specific. `set_prob` on `update` gates (the
  "probability that the update happened" use) falls under the write-once
  rule of B2. The token-rewrite pattern of these triggers is also what
  B2' generalises to probability changes: the base table's token column
  is rewritten, the circuit keeps the old gates.

## Transaction-level provenance (the other reading of "transaction support")

As shown in the previous section, each DML statement mints its own
`update` gate and a rolled-back transaction already leaves no trace
except orphan gates. What is missing is the transaction as a unit:

- **One update gate per transaction.** Mint `tx_token` on the first
  tracked DML of the transaction and keep it in a transaction-scoped
  place: `set_config('provsql.transaction_token', ..., true)` (`SET
  LOCAL` semantics, vanishes at commit or rollback, no C needed), or a
  backend-local variable cleared by an xact callback. Each statement's
  gate becomes `times(tx_token, stmt_token)` (or the statement gate gets
  `tx_token` as an extra child); `update_provenance` gains `xid`
  (`pg_current_xact_id()`) and `tx_token` columns. `undo(tx_token)` then
  undoes the whole transaction through the existing machinery, and
  `undo(stmt_token)` keeps working. Temporal evaluation (`sr_temporal`)
  sees the transaction's validity through the shared factor.
- **Commit-time validity.** `valid_time` currently starts at transaction
  start, so two overlapping transactions can commit in the opposite order
  of their timestamps. A `DEFERRABLE INITIALLY DEFERRED` constraint
  trigger on `update_provenance` fires at commit and can stamp
  `clock_timestamp()`; or, with `track_commit_timestamp = on`, readers can
  use `pg_xact_commit_timestamp(xid)` after the fact. Either is a few
  lines.
- **GProM-style reenactment** (replaying a past transaction under
  snapshot isolation through MV-semirings) is not needed in ProvSQL's
  eager model; the per-transaction gate *is* the transaction's provenance.
  The feature-gap analysis (item 11) can be closed by the above rather
  than by importing reenactment.

All of this is PL/pgSQL plus one or two C helpers, PostgreSQL 14+ like the
rest of data-modification tracking, and independent of options A–D.

## Out of scope after the plan

What the plan above does *not* deliver, even once A, B, C, the clean-up,
D and the transaction token are all in place. Listed so nobody mistakes
the plan for full transactional integration.

- **Reads write.** Every `SELECT` with provenance still persists gates.
  Three consequences stay: a physical replica cannot serve provenance
  queries (under D its backends refuse store writes, and almost any query
  needs a gate that does not exist yet); `READ ONLY` transactions write
  to the store; and the store grows with ad hoc exploration, so clean-up
  remains a recurring chore. The alternative is a different model, with
  a query's gates kept ephemeral (in-memory, per session) and persisted
  only when a token is actually stored in a table. That is a redesign of
  the rewriter and the evaluators, not an increment.
- **No MVCC for the store.** Write-once removes rewrites, but a snapshot
  still sees the future: a `REPEATABLE READ` transaction that started
  before a concurrent `set_prob` on an unset gate reads the new
  probability, and a concurrent session sees an uncommitted probability
  until it is cleared on rollback. Only the deferred-write overlay
  (mentioned under B2, not proposed) or heap storage (B3, rejected)
  closes this.
- **No online clean-up.** Exclusive access is required because of
  idempotent re-creation. A concurrent collector needs per-gate epochs
  and session pinning, i.e., a new on-disk field and a cache-invalidation
  protocol. Until then, 24/7 deployments can only grow between
  maintenance windows.
- **Durability is a near-guarantee with fail-stop, not a guarantee.** D's
  checkpoint gap is detected, not eliminated; PITR and base backups work
  only with C3's write ordering and the caveats listed there. A hard
  guarantee needs the store in shared buffers.
- **Two-phase commit excluded.** A transaction that wrote a probability
  (B2) or awaits a sync (C2) refuses `PREPARE TRANSACTION`.
- **Template copies and tablespace moves.** `CREATE DATABASE ... TEMPLATE`
  under `WAL_LOG` still drops the store; PostgreSQL offers no hook. Only
  `STRATEGY = FILE_COPY` and documentation help. (C4 makes `ALTER
  DATABASE ... SET TABLESPACE` work, since that one copies the
  directory.)
- **`pg_dump`, logical replication, `pg_upgrade`.** D covers physical
  replication only. `pg_upgrade` transfers relation files by `pg_class`
  entry and ignores foreign files in `base/<oid>/` (and preserves
  database OIDs only from PG 15), so a major-version upgrade loses every
  circuit unless the files are copied by hand; `getting-provsql.rst`
  says the store "carries over unchanged", which holds for in-place
  `ALTER EXTENSION` and not for `pg_upgrade` (by knowledge of
  `pg_upgrade`, not tested here). Closing this needs a logical export /
  import of the reachable circuit (`provsql.dump_circuit()` into a
  dumpable table and its inverse), which the clean-up's mark phase half
  builds; it is not in the plan.
- **C2's cost is unmeasured.** One `fdatasync` per store-writing
  transaction through a single worker needs group commit (coalesce the
  `S` requests queued while one sync runs) to hold up under concurrency.
- **Transaction-level provenance stays eager and single-database.** No
  provenance across databases or foreign data wrappers; no reenactment
  of a transaction that ran before tracking was enabled (GProM's use
  case); `query` still comes from `pg_stat_activity` (the outer call
  inside a function or prepared statement); commit-time validity depends
  on `track_commit_timestamp` or a deferred trigger rather than being
  intrinsic.
