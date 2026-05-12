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

2. **Main loop**: :cfunc:`provsql_mmap_main_loop` reads gate-creation
   messages from a pipe, writes them to the mmap store, and sends
   acknowledgements back.  It handles ``SIGTERM`` for graceful
   shutdown.

3. **Shutdown**: :cfunc:`destroy_provsql_mmap` syncs and closes all
   open per-database circuits.


Inter-Process Communication
---------------------------

Normal backends (the processes that execute SQL queries) cannot write
directly to the mmap files -- only the background worker does.  Two
anonymous pipes provide bidirectional IPC:

- **Main-to-background** (``pipembr`` / ``pipembw``): backends send
  gate-creation requests.
- **Background-to-main** (``pipebmr`` / ``pipebmw``): the worker sends
  acknowledgements (e.g., the UUID of a newly created gate).

Pipe writes use buffered macros (:cfunc:`STARTWRITEM` /
:cfunc:`ADDWRITEM` / :cfunc:`SENDWRITEM`) that respect ``PIPE_BUF``
atomicity guarantees -- each message is delivered as an atomic unit
even when multiple backends write concurrently.

Every message begins with a one-byte opcode followed immediately by the
sender's ``MyDatabaseId`` (a 4-byte ``Oid``).  The worker reads this
OID before dispatching to the correct per-database
:cfunc:`MMappedCircuit` instance, opening a new one lazily if this is
the first message for that database.


Shared Memory: ``provsql_shmem``
--------------------------------

The pipe file descriptors and a lightweight lock live in a PostgreSQL
shared-memory segment managed by :cfile:`provsql_shmem.c`.

The :cfunc:`provsqlSharedState` structure contains:

- **lock** -- a PostgreSQL ``LWLock`` that serializes pipe writes from
  multiple backends.  Backends acquire it in exclusive mode before
  writing to the main-to-background pipe, ensuring message atomicity.
- **pipebmr / pipebmw** -- file descriptors for the background-to-main
  pipe.
- **pipembr / pipembw** -- file descriptors for the main-to-background
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
first use.  Each instance holds four mmap-backed containers, stored in
``$PGDATA/base/<db_oid>/``:

- ``provsql_mapping.mmap`` -- a :cfunc:`MMappedUUIDHashTable` mapping
  UUID tokens to gate IDs, enabling O(1) lookup.
- ``provsql_gates.mmap`` -- a :cfunc:`MMappedVector` of
  :cfunc:`GateInformation` records, one per gate.
- ``provsql_wires.mmap`` -- a :cfunc:`MMappedVector` of
  ``pg_uuid_t``, the flattened child-UUID lists.
- ``provsql_extra.mmap`` -- a :cfunc:`MMappedVector` of ``char`` for
  variable-length per-gate string annotations.

Placing the files under ``$PGDATA/base/<db_oid>/`` gives per-database
isolation and automatic cleanup: PostgreSQL removes that directory when
the database is dropped.

Every mmap file begins with a **16-byte format header**:

.. code-block:: c

   uint64_t magic;      /* file-type identifier, e.g. 0x7365746147537650 for gates */
   uint16_t version;    /* format version, currently 1 */
   uint16_t elem_size;  /* sizeof(T) at write time */
   uint32_t _reserved;  /* padding, must be 0 */

The constructor validates all three fields on open and throws if they
do not match, catching type mismatches and incompatible recompilations
early.

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
the same rule: ``PLUS = 0``, ``TIMES = 1``, ``MINUS = 2``,
``DIV = 3``, ``NEG = 4`` are persisted on disk and must not be
reordered.

The float8 mode of ``gate_value`` introduced for the
continuous-distribution surface does not require a format
version bump: the ``extra`` blob is text, the integer-mode and
float8-mode parsers (``extract_constant_C`` and
``extract_constant_double``) both consume that same text
representation, and the choice of parser is made by the consumer
at evaluation time based on the gate's surrounding context.

:cfunc:`MMappedVector` (:cfile:`MMappedVector.h` /
:cfile:`MMappedVector.hpp`) provides a ``std::vector``-like interface
over an mmap region, supporting ``push_back``, random access, and
iteration.

:cfunc:`MMappedUUIDHashTable` (:cfile:`MMappedUUIDHashTable.h`) is an
open-addressing hash table keyed by 16-byte UUIDs, stored in an mmap
region.

These data structures grow by extending the underlying file and
remapping.  Because only the background worker writes, there are no
concurrency issues within the mmap files themselves.


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
  cache.  Returns ``true`` if the gate was *already* cached, in
  which case the caller can skip the IPC write.  This is the
  fast path for ``create_gate``: if a query allocates the same
  gate twice in the same backend (easy to trigger with shared
  sub-circuits), the second call is a hash-table lookup, not a
  pipe write.

- :cfunc:`circuit_cache_get_type` -- look up a gate's type.
  Returns ``gate_invalid`` on a miss; the SQL wrapper then
  falls back to an IPC read and, on success, re-enters the
  gate into the cache so that subsequent lookups hit.

- :cfunc:`circuit_cache_get_children` -- same pattern for the
  children list, used by :sqlfunc:`get_children`.

The cache is write-through, not write-back: every gate-creating
call still reaches the mmap worker in the end (either directly,
for a cache miss, or because an earlier call in the same
backend wrote it through), so the persistent store always
reflects the complete circuit even though each individual
lookup may resolve locally.


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
