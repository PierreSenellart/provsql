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

1. **Initialization**: :cfunc:`initialize_provsql_mmap` opens (or
   creates) four memory-mapped files that back the circuit data
   structures.  A singleton :cfunc:`MMappedCircuit` instance is created
   over these files.

2. **Main loop**: :cfunc:`provsql_mmap_main_loop` reads gate-creation
   messages from a pipe, writes them to the mmap store, and sends
   acknowledgements back.  It handles ``SIGTERM`` for graceful
   shutdown.

3. **Shutdown**: :cfunc:`destroy_provsql_mmap` syncs and closes the
   memory-mapped files.


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
persistent circuit store.  It holds four mmap-backed containers:

- A **gate-type vector** mapping gate IDs to their :cfunc:`gate_type`.
- A **wire list** storing the children of each gate.
- A **UUID hash table** (:cfunc:`MMappedUUIDHashTable`) mapping UUID
  tokens to gate IDs, enabling O(1) lookup.
- An **annotation store** for per-gate metadata (probabilities,
  aggregate info, extra labels).

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
