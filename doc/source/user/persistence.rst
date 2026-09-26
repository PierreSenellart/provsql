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
manager and catalog (see :doc:`../dev/memory` for the design), with the
consequences below.

.. _persistence-transactions:

What a transaction does to the circuit
--------------------------------------

The circuit is **append-only**, so a rollback never leaves it
inconsistent:

* **A gate is never removed and never changes.** A gate created by a
  transaction that rolls back is an *orphan*: no committed row references
  it, and computing the same expression again reuses it.
  :ref:`circuit_cleanup <persistence-cleanup>` reclaims orphans.

* **A probability is written once.** :sqlfunc:`set_prob` writes one on a
  gate that has none, accepts the identical value again (so setup scripts
  and notebook cells stay re-runnable), and refuses a different one.  A
  write made by a transaction that rolls back is cleared, also at
  savepoint granularity.

  To give a tuple a *different* probability, give it a different input
  gate with :sqlfunc:`replace_input`:

  .. code-block:: postgresql

      UPDATE s SET provsql = provsql.replace_input(provsql, 0.3) WHERE id = 42;

  The update is an ordinary heap write, so MVCC, WAL, replication and
  ``pg_dump`` all apply to it, and the new gate and its probability roll
  back with it.  Re-running a query over the base table sees the new
  probability, while a table materialised earlier keeps the old tokens
  and the old probability.  *A derived table reflects the base tables as
  they were when it was built* -- the same rule a ``DELETE`` under
  :doc:`data-modification tracking <data-modification>` follows.

  The block counterpart is :sqlfunc:`replace_block`, which re-mints a
  whole :sqlfunc:`repair_key` block, and :sqlfunc:`replace_update`, which
  gives a recorded data modification a different probability.

* **Per-relation metadata follows the transaction.** A rolled-back
  :sqlfunc:`add_provenance` leaves no record; a rolled-back ``DROP TABLE``
  keeps one; a concurrent session sees a change only once it commits.

The circuit itself is not isolated: another session reads a probability
between the write and the rollback that clears it, and a
``REPEATABLE READ`` transaction that started before a concurrent
:sqlfunc:`set_prob` still sees the new value.  For the usual "load once, query many" use,
this does not arise.

Two further limits apply.  Every provenance query **writes**, reads included,
since computing an answer's provenance creates the gates that represent
it: a ``READ ONLY`` transaction writes to the store, and ad hoc
exploration makes it grow.  And a transaction that has written a
probability refuses ``PREPARE TRANSACTION``.

Durability
----------

A committed transaction's rows are in the WAL, fsynced.  Its gates are in
the kernel's page cache, and reach the disk shortly afterwards.  A crash
of PostgreSQL loses nothing (the page cache outlives the processes); a
crash of the *machine* can lose whatever had not been written back, and a
gate lost that way reads back as an independent input with probability 1
-- silently, because an unknown token is a valid input gate.

ProvSQL flushes the store a fraction of a second after the last write,
which bounds that loss the way ``synchronous_commit = off`` bounds the
heap's.  To remove it:

.. code-block:: postgresql

    SET provsql.synchronous_commit = on;

A transaction that has written to the store then forces it to stable
storage before it commits.  The cost is one flush per store-writing
transaction, which includes read-only queries.

:sqlfunc:`check_store` reports whether the files still agree with each
other:

.. code-block:: postgresql

    SELECT * FROM provsql.check_store();

Every count is 0 for a healthy store.  ``unclean_shutdown`` is true after
an immediate shutdown or a server crash, and is not by itself a
problem.  A non-zero ``dangling_indices``, ``bad_wires`` or ``bad_extra``
means the four files do not agree -- typically because they were copied
at different instants.  :ref:`circuit_cleanup <persistence-cleanup>`
rebuilds the store from what is still reachable.

Backups
-------

**``pg_dump`` does not carry the circuit.** A dump carries the tokens --
they are ``uuid`` values in ordinary columns -- and the per-relation
metadata, but not the gates behind them.  Restored elsewhere, every token
reads back as an input gate: a base table's rows survive this, while
anything derived (a ``CREATE TABLE AS SELECT
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
which copies only relation files.  The clone then has *no* circuit at
all:

.. code-block:: postgresql

    -- The clone carries the circuit:
    CREATE DATABASE b TEMPLATE a STRATEGY = FILE_COPY;

    -- The clone does not (the PostgreSQL 15+ default):
    CREATE DATABASE b TEMPLATE a;

``ALTER DATABASE ... SET TABLESPACE`` copies the whole directory, so it
carries the store; a database created in a non-default tablespace is
found there too.

**``pg_upgrade`` does not carry the circuit** either: it transfers only
relation files.  An in-place ``ALTER EXTENSION provsql UPDATE``
keeps the store; a major-version upgrade needs the four files copied by
hand into the new cluster's database directories, and only from
PostgreSQL 15 onwards, where database OIDs are preserved.

Replication
-----------

By default a physical standby holds whatever its base backup copied and
never receives anything more, while its own backends keep creating gates
of their own: the two stores diverge, and after a promotion they cannot
be reconciled.

From PostgreSQL 15, ProvSQL can write every change to the store to the
WAL, and the standby replays it:

.. code-block:: postgresql

    -- in postgresql.conf, or per session as a superuser
    provsql.synchronous_commit = on
    provsql.wal_logging = on

``provsql.wal_logging`` requires ``provsql.synchronous_commit``.  With
it on, a hot-standby backend **refuses** to write to the store, so
provenance queries, reads included, do not run on the standby.  The
standby carries the provenance of what the
primary computed; computing new provenance stays the primary's job.

Both settings are off by default, and turning them on changes what the
cluster writes to its WAL, so a replica that has never seen these records
should get a fresh base backup after the change.

Two further cautions.  ProvSQL's WAL resource-manager id is 151,
reserved on the `PostgreSQL wiki
<https://wiki.postgresql.org/wiki/CustomWALResourceManagers>`_; a cluster
must not load two extensions claiming the same id.  And a PostgreSQL fork whose
storage replays WAL offline -- Amazon Aurora, Neon, Google AlloyDB,
Microsoft HorizonDB -- does not run extension resource managers, so
``provsql.wal_logging`` must stay off there.

.. _persistence-cleanup:

Reclaiming space
----------------

Because nothing is ever removed, the store grows with every dropped
table, every :sqlfunc:`remove_provenance`, every reloaded dataset, every
rolled-back transaction and every exploratory query.
:sqlfunc:`circuit_cleanup` is the one operation that removes gates, much
as ``VACUUM FULL`` reclaims space under MVCC:

.. code-block:: postgresql

    SELECT * FROM provsql.circuit_cleanup(dry_run => true);
    SELECT * FROM provsql.circuit_cleanup();

It keeps every gate reachable from a token stored in the database and
rewrites the four files compactly; it also repairs a store damaged by an
interrupted write or an inconsistent copy.

It **takes the database to itself**: it holds the lock ``DROP DATABASE``
holds, so sessions connecting from then on wait, and it refuses to run
while another session is already connected.  Run it after a bulk reload or a round of experiments, not on a schedule.

A **root** is every value of a ``uuid``, ``agg_token`` or
``random_variable`` column, and of arrays of those, in every table and
materialised view of the database -- not only columns named ``provsql``
-- plus the constants ``gate_zero``, ``gate_one`` and ``gate_null`` (the
value gate of the NULL value).  A token
that lives only *outside* the database is **not** a root: one kept in a
notebook cell, a deep link, a file, a ``text`` column or a ``jsonb``
document.  Content-addressed gates come back by re-running the query that
built them; freshly minted ones -- a ``random_variable`` leaf, the update
gate of a deleted log row, the input gate of a row deleted from an
untracked copy -- do not.  Tokens on *foreign* tables are not scanned
either; the function says so at ``NOTICE`` level when it finds any.

Upgrading from before 1.13.0
-----------------------------

Before 1.13.0, the per-relation metadata was kept in a fifth
memory-mapped file, ``provsql_table_info.mmap``.  The upgrade script
imports it into ``provsql.table_info``; :sqlfunc:`migrate_table_info`
does the same by hand and is a no-op on a database that has already
been migrated or never had the file.

A ``gates`` file written by an older ProvSQL cannot distinguish a
probability written as 1 from one never written, so a probability of 1
there is treated as never written and can be written once more.  The
first :sqlfunc:`circuit_cleanup` removes that ambiguity.  An older
ProvSQL cannot read a store this version has written to.
