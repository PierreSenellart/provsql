Persistence, backups and replication
====================================

Everything ProvSQL keeps *inside* PostgreSQL behaves the way the rest of
your database does: the ``provsql`` column of a tracked table, the
per-relation metadata in ``provsql.table_info``, the data-modification
log ``provsql.update_provenance``, the tool registry.  They are ordinary
tables, so they roll back with the transaction that wrote them, they are
written to the WAL, they replicate, and ``pg_dump`` carries them.

The **provenance circuit** does not.  It lives in four memory-mapped
files in the database's directory, outside PostgreSQL's WAL, buffer
manager and catalog -- a deliberate choice that is what makes gate
creation cheap enough to happen on every query (see :doc:`../dev/memory`
for the design and its measurements).  This chapter says exactly what
that costs, and what to do about each consequence.

.. _persistence-transactions:

What a transaction does to the circuit
--------------------------------------

The circuit is **append-only**, and it is that property, rather than
transactional undo, that makes rollback harmless:

* **A gate is never removed and never changes.** Its UUID is either
  content-addressed -- a hash of the operation and its children -- or
  freshly minted.  A gate created by a transaction that rolls back is an
  *orphan*: no committed row can reference it, and if the same expression
  is computed again the same UUID lands on the same gate.  So a
  rolled-back transaction leaves gates behind, but nothing inconsistent.
  Reclaiming them is what :ref:`circuit_cleanup <persistence-cleanup>` is
  for.

* **A probability is written once.** :sqlfunc:`set_prob` writes one on a
  gate that has none, accepts the identical value again (so setup scripts
  and notebook cells stay re-runnable), and refuses a different one.  A
  write made by a transaction that rolls back is cleared, so the circuit
  is left as the transaction found it -- including at savepoint
  granularity.

  The same holds for a gate's annotations, the integer pair
  ``set_infos`` records and the string ``set_extra`` attaches: each is
  written once and accepts the value it already holds.
  They are not cleared on rollback, and do not need to be -- an
  annotation is a function of the gate, so a rolled-back write leaves the
  field holding the value any later writer would have written.

  To give a tuple a *different* probability, give it a different input
  gate with :sqlfunc:`replace_input`:

  .. code-block:: postgresql

      UPDATE s SET provsql = provsql.replace_input(provsql, 0.3) WHERE id = 42;

  The update is an ordinary heap write, so MVCC, WAL, replication and
  ``pg_dump`` all apply to it, and the new gate and its probability roll
  back with it.  The old gate stays as it was, and so does everything
  derived from it: re-running a query over the base table builds new
  derived gates over the new token and sees the new probability, while a
  table materialised earlier keeps the old tokens and the old
  probability.  *A derived table reflects the base tables as they were
  when it was built* -- the same rule a ``DELETE`` under
  :doc:`data-modification tracking <data-modification>` already follows.

  The block counterpart is :sqlfunc:`replace_block`, which re-mints a
  whole :sqlfunc:`repair_key` block, and :sqlfunc:`replace_update`, which
  gives a recorded data modification a different probability.

* **Per-relation metadata follows the transaction.** A rolled-back
  :sqlfunc:`add_provenance` leaves no record; a rolled-back ``DROP TABLE``
  keeps one; a concurrent session sees a change only once it commits.

What is *not* transactional is isolation of the circuit itself: another
session reads a probability between the write and the rollback that
clears it, and a ``REPEATABLE READ`` transaction that started before a
concurrent :sqlfunc:`set_prob` still sees the new value.  For the usual
"load once, query many" shape this does not arise.

Two further limits are worth stating plainly.  Every provenance query
**writes**, reads included -- computing an answer's provenance creates
the gates that represent it -- so a ``READ ONLY`` transaction writes to
the store, and ad hoc exploration makes it grow.  And a transaction that
has written a probability refuses ``PREPARE TRANSACTION``: the list of
writes to clear on rollback lives in the backend, and a prepared
transaction outlives it.

Durability
----------

A committed transaction's rows are in the WAL, fsynced.  Its gates are in
the kernel's page cache, and reach the disk shortly afterwards.  A crash
of PostgreSQL loses nothing (the page cache outlives the processes); a
crash of the *machine* can lose whatever had not been written back, and a
gate lost that way reads back as an independent input with probability 1
-- silently, because an unknown token is a valid input gate.

The worker forces the store out a fraction of a second after the last
write, which bounds that loss the way ``synchronous_commit = off`` bounds
the heap's.  To remove it:

.. code-block:: postgresql

    SET provsql.synchronous_commit = on;

A transaction that has written to the store then forces it to stable
storage before it commits, and waits for the acknowledgement.  The cost
is one flush per store-writing transaction -- which, per the note above,
includes read-only queries.

:sqlfunc:`check_store` reports whether the files still agree with each
other:

.. code-block:: postgresql

    SELECT * FROM provsql.check_store();

Every count is 0 for a healthy store.  ``unclean_shutdown`` is true when
a file was still marked open-for-writing when it was opened, which an
immediate shutdown or a server crash leaves behind and is not by itself a
problem.  A non-zero ``dangling_indices``, ``bad_wires`` or ``bad_extra``
means the four files do not agree -- typically because they were copied
at different instants.  :ref:`circuit_cleanup <persistence-cleanup>`
rebuilds the store from what is still reachable.

Backups
-------

**``pg_dump`` does not carry the circuit.** A dump carries the tokens --
they are ``uuid`` values in ordinary columns -- and the per-relation
metadata, but not the gates behind them.  Restored elsewhere, every token
reads back as an input gate: a base table's rows survive this by accident
of that default, while anything derived (a ``CREATE TABLE AS SELECT
provenance()``, a data-modification history, a :sqlfunc:`repair_key`
block) is lost.  The same holds for logical replication.

**File-level backups must include the store.** The four files are
``provsql_gates.mmap``, ``provsql_wires.mmap``, ``provsql_mapping.mmap``
and ``provsql_extra.mmap`` in each database's directory under the data
directory.  Copy them with the server stopped, or with the rest of the
data directory in a filesystem snapshot; copying them one at a time from
a running server gives four files from four different instants, which is
what ``check_store`` reports as inconsistent.

**Cloning a database needs ``STRATEGY = FILE_COPY``.** From PostgreSQL
15, ``CREATE DATABASE ... TEMPLATE`` defaults to ``STRATEGY = WAL_LOG``,
which copies relation forks through shared buffers and ignores anything
else in the directory.  The clone then has *no* circuit at all:

.. code-block:: postgresql

    -- The clone carries the circuit:
    CREATE DATABASE b TEMPLATE a STRATEGY = FILE_COPY;

    -- The clone does not (the PostgreSQL 15+ default):
    CREATE DATABASE b TEMPLATE a;

``ALTER DATABASE ... SET TABLESPACE`` copies the whole directory, so it
carries the store; a database created in a non-default tablespace is
found there too.

**``pg_upgrade`` does not carry the circuit** either: it transfers
relation files by their ``pg_class`` entry and ignores everything else in
the database directories.  An in-place ``ALTER EXTENSION provsql UPDATE``
keeps the store; a major-version upgrade needs the four files copied by
hand into the new cluster's database directories, and only from
PostgreSQL 15 onwards, where database OIDs are preserved.

Replication
-----------

By default a physical standby holds whatever its base backup copied and
never receives anything more, while its own backends keep creating gates
of their own -- so the two stores diverge, and after a promotion they
cannot be reconciled.

From PostgreSQL 15, ProvSQL can write each mutation of the store to the
WAL through a resource manager of its own, which the standby's startup
process replays:

.. code-block:: postgresql

    -- in postgresql.conf, or per session as a superuser
    provsql.synchronous_commit = on
    provsql.wal_logging = on

``provsql.wal_logging`` requires ``provsql.synchronous_commit``: what
keeps replay complete is that the store on disk is never behind the WAL,
and the at-commit barrier is what guarantees that.  With it on, a
hot-standby backend **refuses** to write to the store, which means
provenance queries do not run on the standby -- they create gates as they
go, reads included.  The standby carries the provenance of what the
primary computed; computing new provenance stays the primary's job.

Both settings are off by default, and turning them on changes what the
cluster writes to its WAL, so a replica that has never seen these records
should get a fresh base backup after the change.

Two further cautions.  ProvSQL's resource-manager id is 151, reserved on
the `PostgreSQL wiki
<https://wiki.postgresql.org/wiki/CustomWALResourceManagers>`_; a cluster
must not load two extensions claiming the same id, since replay would
hand one extension's records to the other.  And a PostgreSQL fork whose
storage replays WAL offline -- Amazon Aurora, Neon, Google AlloyDB,
Microsoft HorizonDB -- does not run extension resource managers, so
``provsql.wal_logging`` must stay off there.

.. _persistence-cleanup:

Reclaiming space
----------------

Because nothing is ever removed, the store grows with every dropped
table, every :sqlfunc:`remove_provenance`, every reloaded dataset, every
rolled-back transaction and every exploratory query.
:sqlfunc:`circuit_cleanup` is the complement of that -- the one operation
allowed to remove gates, the way ``VACUUM FULL`` is the complement of
MVCC:

.. code-block:: postgresql

    SELECT * FROM provsql.circuit_cleanup(dry_run => true);
    SELECT * FROM provsql.circuit_cleanup();

It keeps every gate reachable from a token stored in the database and
rewrites the four files compactly, which also makes it the repair tool
for a store an interrupted write or an inconsistent copy left damaged.

It **takes the database to itself**: it holds the lock ``DROP DATABASE``
holds, so sessions connecting from then on wait, and it refuses to run
while another session is already connected.  That is unavoidable.  Gates
are re-created idempotently, so a query running alongside can adopt an
orphan gate a moment before the sweep removes it -- and a backend's own
cache answers "this gate exists" without asking the store at all.  Run it
after a bulk reload or a round of experiments, not on a schedule.

A **root** is every value of a ``uuid``, ``agg_token`` or
``random_variable`` column, and of arrays of those, in every table and
materialised view of the database -- not only columns named ``provsql``
-- plus the semiring constants ``gate_zero`` and ``gate_one``.  A token
that lives only *outside* the database is **not** a root: one kept in a
notebook cell, a deep link, a file, a ``text`` column or a ``jsonb``
document.  Content-addressed gates come back by re-running the query that
built them; freshly minted ones -- a ``random_variable`` leaf, the update
gate of a deleted log row, the input gate of a row deleted from an
untracked copy -- do not.  Tokens on *foreign* tables are not scanned
either; the function says so at ``NOTICE`` level when it finds any.

Upgrading from before 1.13.0
-----------------------------

The per-relation metadata used to live in a fifth memory-mapped file,
``provsql_table_info.mmap``, and now lives in ``provsql.table_info``.
The upgrade script imports it; :sqlfunc:`migrate_table_info` does the
same by hand and is a no-op on a database that has already been
migrated or never had the file.

The ``gates`` file gained a format version.  A file written by an older
ProvSQL is read as before, with one leniency: it cannot distinguish a
probability written as 1 from one never written, so it treats 1 as never
written and lets it be written once more.  The first
:sqlfunc:`circuit_cleanup` normalises that away, after which the store is
unambiguous.  An older ProvSQL cannot read a store this version has
written to.
