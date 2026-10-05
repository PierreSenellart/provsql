Memory Management
=================

ProvSQL persists provenance circuits across transactions and shares them
between PostgreSQL backend processes.  This is achieved through
memory-mapped files managed by a dedicated background worker, coordinated
via PostgreSQL shared memory.


Why Not Regular Tables?
-----------------------

Earlier versions of ProvSQL stored circuits in regular PostgreSQL
tables.  This worked but had severe performance limitations: the
PL/pgSQL functions that insert gates used ``LOCK`` to serialize
access, preventing parallelism.  The current architecture uses
memory-mapped files written by a single background worker, avoiding
both lock contention and WAL overhead.  Benchmarks in the ICDE 2026
paper :cite:`sen2026provsql` show that the mmap implementation
scales linearly with dataset size, while an earlier shared-memory
variant hit limits at moderate scale factors.

The price is that the store is outside the WAL, the buffer manager and
the catalog, so it is invisible to crash recovery, to streaming
replication, to ``pg_dump`` and to ``CREATE DATABASE ... TEMPLATE`` under
the PostgreSQL 15+ default strategy.  Gates are immutable,
content-addressed and re-created idempotently, so a transaction that
rolls back leaves orphans, not inconsistencies; the pieces that close
the rest of the gap are:

* **write-once probabilities and annotations**
  (:cfile:`probability_store.c`), with an undo list that clears the
  probabilities an aborted transaction wrote; the two ``info`` fields
  are written once *each* (0 meaning "nothing recorded"), as different
  parties write them;
* **per-relation metadata in the heap** (:cfile:`table_info.c`), where
  catalog-shaped data belongs, so it follows the transaction and
  ``pg_dump`` carries it;
* **ordered writes and an atomic rehash** (:cfile:`MMappedCircuit.cpp`,
  :cfile:`MMappedUUIDHashTable.cpp`), so an interrupted write leaves an
  unreferenced record rather than a dangling index;
* **a periodic flush and an at-commit barrier**
  (``provsql.store_synchronous_commit``), which bound and then remove what a
  machine crash can lose;
* **a custom WAL resource manager** (:cfile:`provsql_rmgr.c`,
  ``provsql.store_wal_logging``, PostgreSQL 15+), which lets a standby carry
  the store;
* **a mark-and-sweep rebuild** (:cfile:`circuit_cleanup.c`), the one
  operation allowed to remove gates, and the repair tool for a store an
  interrupted write left damaged.

The user-facing contract of all this -- what a transaction does to the
circuit, what a backup carries, what a replica sees -- is in
:doc:`../user/persistence`.


Background Worker: ``provsql_mmap``
-----------------------------------

The mmap subsystem lives in :cfile:`provsql_mmap.c` and :cfile:`MMappedCircuit.cpp`.

:cfunc:`RegisterProvSQLMMapWorker` (called from :cfunc:`_PG_init`)
registers a PostgreSQL background worker with the postmaster.  When the server
starts, the postmaster forks the worker process, which calls
:cfunc:`provsql_mmap_worker`:

1. **Initialization**: :cfunc:`initialize_provsql_mmap` is a no-op --
   circuits are opened lazily on the first IPC message for each
   database (see below).

2. **Main loop**: :cfunc:`provsql_mmap_main_loop` reads messages
   from a pipe, writes gate creations to the mmap store, and sends
   replies to lookup requests.  The loop exits when reading from
   the pipe fails (end-of-file or error), e.g. at server shutdown,
   which triggers the cleanup step below.

3. **Shutdown**: :cfunc:`destroy_provsql_mmap` syncs and closes all
   open per-database circuits.


Inter-Process Communication
---------------------------

Normal backends (the processes that execute SQL queries) cannot write
directly to the mmap files -- only the background worker does.  Two
anonymous pipes provide bidirectional IPC:

- **Backend-to-worker** (``pipebmr`` / ``pipebmw``, ``bm`` for
  backend-to-mmap-worker): backends send gate-creation and lookup
  requests.
- **Worker-to-backend** (``pipembr`` / ``pipembw``): the worker sends
  replies to lookup requests (e.g., the gate type or children
  answering a query; gate creation gets no reply).

Pipe writes use buffered macros (:cfunc:`STARTWRITEM` /
:cfunc:`ADDWRITEM` / :cfunc:`SENDWRITEM`) that respect ``PIPE_BUF``
atomicity guarantees -- each message is delivered as an atomic unit
even when multiple backends write concurrently.

Every message begins with a one-byte opcode followed by a header of two
4-byte ``Oid``\ s: the sender's ``MyDatabaseId`` and its
``MyDatabaseTableSpace``.  The worker dispatches on the first to the
correct per-database :cfunc:`MMappedCircuit` instance, opened lazily;
the second lets it resolve the directory of a database outside the
default tablespace, since it runs outside any transaction and cannot
read ``pg_database``.

Writes and reads follow one rule: a write is not answered, a read is.
Creating a gate (``C``) sends and returns.  A gate that records infos
or a text is created together with them by a single ``G`` message
(:cfunc:`provsql_internal_create_gate_with`, :sqlfunc:`create_gate`
with six arguments); nothing sets them afterwards.  This is sound
because what a gate records follows from its address: a content-addressed
gate hashes it, and a gate with a fresh token has no other writer, so
the write-once rule has nothing to refuse.

The worker reads the pipe one ``PIPE_BUF``-sized buffer at a time
(:cfunc:`provsql_worker_read`) and parses messages from it, so that it
keeps up with the backends that feed it.


Shared Memory: ``provsql_shmem``
--------------------------------

The pipe file descriptors and a lightweight lock live in a PostgreSQL
shared-memory segment managed by :cfile:`provsql_shmem.c`.

The :cfunc:`provsqlSharedState` structure contains:

- **lock** -- a PostgreSQL ``LWLock`` coordinating pipe access from
  multiple backends.  Messages that fit in a single ``PIPE_BUF``
  write are sent under a *shared* lock, relying on the kernel's
  ``PIPE_BUF`` atomicity guarantee; the exclusive mode is reserved
  for oversized multi-part writes and for request-reply round trips.
- **pipebmr / pipebmw** -- file descriptors for the backend-to-worker
  pipe.
- **pipembr / pipembw** -- file descriptors for the worker-to-backend
  pipe.

Lifecycle:

- :cfunc:`provsql_shmem_request` (called from ``shmem_request_hook`` on
  PostgreSQL >= 15) reserves the required shared-memory size.
- :cfunc:`provsql_shmem_startup` (called from ``shmem_startup_hook``)
  allocates the segment, creates the pipes, and initializes the lock.

Locking helpers (:cfunc:`provsql_shmem_lock_exclusive`,
:cfunc:`provsql_shmem_lock_shared`, :cfunc:`provsql_shmem_unlock`) wrap
the ``LWLock`` API.


Mmap-Backed Data Structures
----------------------------

:cfunc:`MMappedCircuit` (in :cfile:`MMappedCircuit.cpp`) is the
persistent circuit store.  The worker maintains one instance per
database in a ``std::map<Oid, MMappedCircuit*>``, created lazily on
first use.  Each instance holds four mmap-backed containers, in the
database's own directory (``base/<db_oid>`` under the data directory, or
the corresponding path under ``pg_tblspc`` when the database lives in
another tablespace):

- ``provsql_mapping.mmap`` -- a :cfunc:`MMappedUUIDHashTable` mapping
  UUID tokens to gate IDs, enabling O(1) lookup.
- ``provsql_gates.mmap`` -- a :cfunc:`MMappedVector` of
  :cfunc:`GateInformation` records, one per gate.
- ``provsql_wires.mmap`` -- a :cfunc:`MMappedVector` of
  ``pg_uuid_t``, the flattened child-UUID lists.
- ``provsql_extra.mmap`` -- a :cfunc:`MMappedVector` of ``char`` for
  variable-length per-gate string annotations.

Per-relation metadata is not among them: it is the
``provsql.table_info`` heap table (see :ref:`per-table-metadata`).

Placing the files in the database's own directory gives per-database
isolation and automatic cleanup: PostgreSQL removes that directory when
the database is dropped.

Every mmap file begins with a **16-byte format header**:

.. code-block:: c

   uint64_t magic;      /* file-type identifier, e.g. 0x7365746147537650 for gates */
   uint16_t version;    /* format version: 2 for gates, 1 for the rest */
   uint16_t elem_size;  /* sizeof(T) at write time */
   uint32_t flags;      /* bit 0: open for writing and not closed since */

The constructor throws if the magic or the element size does not
match, catching type mismatches and incompatible recompilations early.
It accepts any version up to the one this build writes, and the reader
interprets an older file by its version (version 1 of the ``gates``
file cannot tell an unwritten probability from a certain one, until a
:sqlfunc:`circuit_cleanup` rewrites it).  A ``flags`` bit still set on
open marks a file left by a process that died mid-write;
:sqlfunc:`check_store` reports it.

Gate-Type ABI
^^^^^^^^^^^^^

The :cfunc:`gate_type` enum in :cfile:`provsql_utils.h` is
persisted: each :cfunc:`GateInformation` record's ``type`` field
stores the numeric enum value, not the name. The enum is
therefore **append-only**: new gate types must be added before
the ``gate_invalid`` sentinel without renumbering existing
values, and pre-existing files must remain readable by a
recompiled extension.

The companion ``provsql_arith_op`` enum (used in ``info1`` of
every ``gate_arith`` gate to identify the operator) follows
the same rule: ``PLUS = 0`` through ``PERCENTILE = 10`` are
persisted on disk and must not be reordered.

The float8 mode of ``gate_value`` introduced for the
continuous-distribution surface does not require a format
version bump: the ``extra`` blob is text, both kinds of consumer
parse that same text representation (the ``HAVING``-side
``extract_constant_string`` in :cfile:`having_semantics.cpp`, and
the ``std::stod``-based float8 readers such as
``extract_finite_double`` in :cfile:`RangeCheck.cpp`), and the
choice of parser is made by the consumer at evaluation time based
on the gate's surrounding context.

Beyond ``gate_value`` literals, the ``extra`` blob carries the
``gate_assumed`` assumption label, ``gate_annotation`` certificates,
``gate_rv`` distribution parameters, and the ``gate_mobius``
per-child coefficient map (which :cfunc:`MMappedCircuit`'s extra-copy
path preserves verbatim, the same mechanism as ``gate_arith``).  The
``info1`` field doubles as the persisted **d-DNNF certificate** on
``gate_plus`` / ``gate_times`` gates (deterministic / decomposable
bits, given at creation by the reachability and joint-width compilers
and loaded back by
:cfunc:`createGenericCircuit`).

:cfunc:`MMappedVector` (:cfile:`MMappedVector.h` /
:cfile:`MMappedVector.hpp`) provides a ``std::vector``-like interface
over an mmap region: appending with ``add``, random access (and
in-place update) with ``operator[]``, and size queries with
``nbElements``.

:cfunc:`MMappedUUIDHashTable` (:cfile:`MMappedUUIDHashTable.h`) is an
open-addressing hash table keyed by 16-byte UUIDs, stored in an mmap
region.

These data structures grow by extending the underlying file and
remapping.  Because only the background worker writes, there are no
concurrency issues within the mmap files themselves.

.. _per-table-metadata:

Per-Table Provenance Metadata
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The safe-query rewriter needs to know, for each base relation,
whether it contributes independent (TID), block-correlated (BID),
or correlated-in-ways-we-cannot-rule-out (OPAQUE) tuples, and what
its base-relation ancestry is.  That is metadata *about relations*,
so it lives where catalog-shaped data belongs: the
``provsql.table_info`` heap table (:cfile:`table_info.c`).

.. code-block:: postgresql

    CREATE TABLE provsql.table_info(
      relid     regclass PRIMARY KEY,
      kind      text     NOT NULL,           -- 'tid' / 'bid' / 'opaque'
      block_key int2[]   NOT NULL DEFAULT ARRAY[]::int2[],
      ancestors oid[]    NOT NULL DEFAULT ARRAY[]::oid[]
    );

``relid`` is a ``regclass`` so that a dump carries the relation's
*name*, which ``pg_restore`` resolves to the OID it has in the target;
the table is registered with ``pg_extension_config_dump``.  As a heap
table, it follows transactions, the WAL, replication and ``pg_dump``.

The kind half is written by ``add_provenance`` (TID), ``repair_key``
(BID) and ``set_table_info`` (also reached by the ``provenance_guard``
trigger flipping a relation to OPAQUE when the user supplies their own
``provsql`` UUID); the ancestor half by :sqlfunc:`set_ancestors`, read
by :sqlfunc:`get_ancestors`, cleared by :sqlfunc:`remove_ancestors`.
Base tables auto-seed ``{self}``; CTAS-derived tables inherit the
transitive union of their sources' ancestor sets via the lineage hook
(:ref:`tid-bid-propagation`).

The planner hook reads the table, without SPI, through the backend-local
caches :cfunc:`provsql_lookup_table_info` and
:cfunc:`provsql_lookup_ancestry` (:cfile:`provsql_utils.c`), which a
row trigger on the table invalidates through the relcache; the
``cleanup_table_info`` event trigger on ``sql_drop`` deletes the row of
a tracked relation dropped outside of ``remove_provenance``.
:sqlfunc:`migrate_table_info` (:cfile:`TableInfoMigrate.cpp`) imports
the mmap file that held this metadata in earlier versions; the upgrade
script calls it.


Per-Backend Circuit Cache
-------------------------

Every access to the persistent circuit -- creating a gate,
reading a gate type, fetching the children of a gate -- goes
through the anonymous pipe to the mmap worker.  That pipe trip
is cheap but not free, and for a query that touches thousands
of gates the round-trips dominate the wall-clock cost of the
SQL functions that wrap them.  :cfunc:`CircuitCache` (in
:cfile:`CircuitCache.cpp`, with a C-linkage shim in
:cfile:`circuit_cache.h`) is a small in-process cache whose
sole purpose is to avoid those round-trips for gates that the
same backend has seen recently.

A cache entry stores a gate's UUID, its :cfunc:`gate_type`, and
the list of its children.  The cache is backed by a Boost
``multi_index_container`` with two indices: a sequenced index
(used as FIFO eviction order) and a hashed-unique index on the
UUID (for O(1) lookup).  It is bounded by a fixed byte budget;
when inserting a new entry would exceed the budget,
:cfunc:`CircuitCache::insert` drops the oldest one.  The cache
is single-threaded: it lives as a file-scope singleton in
:cfile:`CircuitCache.cpp`, so each PostgreSQL backend process
has its own instance and there is no sharing between backends.

The C functions in :cfile:`circuit_cache.h` that
:cfile:`provsql_mmap.c` actually calls are:

- :cfunc:`circuit_cache_create_gate` -- insert a gate into the
  cache.  Returns ``true`` if the gate was newly inserted,
  ``false`` if it was already present.  The return value does
  *not* gate the IPC write: :cfunc:`provsql_internal_create_gate`
  always falls through to the worker, because a cache hit only
  proves the token was seen by this backend, not that the worker
  has the gate (skipping the write on a hit silently dropped gates
  under concurrent backends).  :cfunc:`MMappedCircuit::createGate`
  is idempotent on already-mapped tokens, so the unconditional
  write is safe.

- :cfunc:`circuit_cache_get_type` -- look up a gate's type.
  Returns ``gate_invalid`` on a miss; the SQL wrapper then
  falls back to an IPC read and, on success, re-enters the
  gate into the cache so that subsequent lookups hit.

- :cfunc:`circuit_cache_get_children` -- same pattern for the
  children list, used by :sqlfunc:`get_children`.

The cache thus only accelerates *reads*
(:sqlfunc:`get_gate_type` / :sqlfunc:`get_children`): every
gate-creating call reaches the mmap worker, so the persistent
store always reflects the complete circuit even though
individual lookups may resolve locally.

The data-decomposition compilers add a second, coarser per-backend
cache on top: a set of already-materialised root tokens, so
re-materialising an unchanged certified circuit (a warm rerun of the
same reachability or joint-width query) skips the per-gate IPC
entirely.  It is sound for the same reason the gate cache is: the
store is append-only and the worker pipe ordered, so a token seen
created by this backend can never designate anything else.  The
compilers also materialise at **content-addressed** UUIDs (v5 hashes
of the construction recipe).  Gates planted in place of a sum or a
product of working-table tokens sit in the *plus-canonical* and
*times-canonical* namespaces; their addresses are remembered by the
backend that planted them (``gate_builders.c``), for as long as the
working table exists, and that is where :sqlfunc:`provenance_plus` /
:sqlfunc:`provenance_times` look for them.  The store is not probed:
the sum or product is computed above a scan of a temporary table,
hence by the planting backend, and sessions that planted nothing pay
nothing.


Reading Circuits Back
---------------------

When a semiring evaluation function is called (e.g.,
:sqlfunc:`sr_boolean`), it needs to build an in-memory circuit from
the persistent mmap store.  The function :cfunc:`getGenericCircuit`
performs a breadth-first traversal starting from the root gate's
UUID, reading gates and wires from the mmap store and constructing
a :cfunc:`GenericCircuit` in process-local memory.  This is the
primitive used by all circuit readers.

Probability evaluation (:sqlfunc:`probability_evaluate`), Shapley
and Banzhaf value computation, and the :cfunc:`BoolExpr` semiring
need a pure Boolean circuit (AND / OR / NOT) suitable for knowledge
compilation and model counting.  They call
:cfunc:`getBooleanCircuit`, which first builds a
:cfunc:`GenericCircuit` via :cfunc:`getGenericCircuit` and then
evaluates it under the :cfunc:`BoolExpr` semiring to produce a
:cfunc:`BooleanCircuit`.
