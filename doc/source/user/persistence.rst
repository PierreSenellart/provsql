Persistence, backups and replication
====================================

Everything ProvSQL keeps in tables (the ``provsql`` column of a tracked
table, the per-relation metadata in ``provsql.table_info``, the
data-modification log ``provsql.update_provenance``, the tool registry)
behaves like the rest of your database: it rolls back, is written to the
WAL, replicates, and is carried by ``pg_dump``.

The **provenance circuit** does not: it lives in four memory-mapped files
in the database's directory, outside PostgreSQL's WAL, with the
consequences below.

.. _persistence-transactions:

What a transaction does to the circuit
--------------------------------------

* **A gate is never removed and never changes.** A gate created by a
  transaction that rolls back stays, unreferenced, until
  :ref:`circuit_cleanup <persistence-cleanup>` reclaims it.

* **A probability is written once.** :sqlfunc:`set_prob` writes one on a
  gate that has none, accepts the identical value again (so setup scripts
  stay re-runnable), and refuses a different one. A write made by a
  transaction that rolls back is cleared.

  To give a tuple a *different* probability, give it a new input gate
  with :sqlfunc:`replace_input`:

  .. code-block:: postgresql

      UPDATE s SET provsql = provsql.replace_input(provsql, 0.3) WHERE id = 42;

  This is an ordinary row update, so it rolls back, replicates and is
  dumped like any other. A table materialised earlier keeps the old
  tokens and the old probability. :sqlfunc:`replace_block` does the same
  for a whole :sqlfunc:`repair_key` block, and :sqlfunc:`replace_update`
  for a recorded data modification.

* **Per-relation metadata follows the transaction**, like any table.

The circuit itself is not isolated: another session can read a
probability between its write and the rollback that clears it. Every
provenance query **writes** to the circuit, reads included, so the store
grows with ad hoc exploration, even in a ``READ ONLY`` transaction. A
transaction that has written a probability refuses ``PREPARE
TRANSACTION``.

Durability
----------

A crash of PostgreSQL loses no gate; a crash of the *machine* can lose
the gates of transactions committed a fraction of a second earlier, the
way ``synchronous_commit = off`` can lose rows. A lost gate silently
reads back as an independent input with probability 1. To remove that
window, at the cost of one flush per transaction that writes to the
circuit:

.. code-block:: postgresql

    SET provsql.store_synchronous_commit = on;

:sqlfunc:`check_store` reports whether the four files agree with each
other:

.. code-block:: postgresql

    SELECT * FROM provsql.check_store();

Every count is 0 for a healthy store (``unclean_shutdown``, true after a
crash, is not by itself a problem). Otherwise, the files were typically
copied at different instants; :ref:`circuit_cleanup
<persistence-cleanup>` rebuilds the store from what is still reachable.

Backups
-------

* ``pg_dump`` **and logical replication do not carry the circuit**, only
  the tokens. Restored elsewhere, every token reads back as an input
  gate: base tables survive this, anything derived (a ``CREATE TABLE AS
  SELECT provenance()``, a data-modification history, a
  :sqlfunc:`repair_key` block) is lost.

* **File-level backups must include the store**: the files
  ``provsql_gates.mmap``, ``provsql_wires.mmap``,
  ``provsql_mapping.mmap`` and ``provsql_extra.mmap`` in each database's
  directory. Copy them with the server stopped, or within a filesystem
  snapshot of the whole data directory.

* **Cloning a database needs** ``STRATEGY = FILE_COPY``; the PostgreSQL
  15+ default copies no circuit:

  .. code-block:: postgresql

      CREATE DATABASE b TEMPLATE a STRATEGY = FILE_COPY;

  ``ALTER DATABASE ... SET TABLESPACE`` carries the store.

* ``pg_upgrade`` **does not carry the circuit.** ``ALTER EXTENSION
  provsql UPDATE`` keeps it; a major-version PostgreSQL upgrade needs the
  four files copied by hand into the new cluster's database directories,
  which works only from PostgreSQL 15 onwards (database OIDs are
  preserved).

Replication
-----------

By default, a physical standby holds only what its base backup copied,
and its own queries create gates of their own: after a promotion, the
two stores cannot be reconciled. From PostgreSQL 15, the circuit can be
WAL-logged and replayed on the standby:

.. code-block:: postgresql

    -- in postgresql.conf, or per session as a superuser
    provsql.store_synchronous_commit = on
    provsql.store_wal_logging = on

The standby then carries the provenance the primary computed, but
refuses to write to the circuit, so provenance queries, reads included,
run only on the primary. After turning this on, give existing replicas a
fresh base backup. ProvSQL's WAL resource-manager id is 151, which no
other loaded extension may claim. ``provsql.store_wal_logging`` must stay
off on PostgreSQL forks that replay WAL in their storage layer (Amazon
Aurora, Neon, Google AlloyDB, Microsoft HorizonDB).

.. _persistence-cleanup:

Reclaiming space
----------------

The store grows with every dropped table, :sqlfunc:`remove_provenance`,
reloaded dataset, rolled-back transaction and exploratory query.
:sqlfunc:`circuit_cleanup` removes the unreachable gates, much as
``VACUUM FULL`` reclaims space, and repairs a damaged store:

.. code-block:: postgresql

    SELECT * FROM provsql.circuit_cleanup(dry_run => true);
    SELECT * FROM provsql.circuit_cleanup();

It needs the database to itself: it refuses to run while another session
is connected, and makes new connections wait. Run it after a bulk reload
or a round of experiments.

It keeps every gate reachable from a value of a ``uuid``,
``agg_token`` or ``random_variable`` column (or an array of those) of any
table or materialised view. A token kept only *outside* these (in a
notebook, a file, a ``text`` or ``jsonb`` value, a foreign table) is not
kept: re-running the query that built it brings it back, except for
freshly created leaves such as a ``random_variable``.

Upgrading from before 1.13.0
-----------------------------

The per-relation metadata, previously in a fifth file
``provsql_table_info.mmap``, is imported into ``provsql.table_info`` by
the upgrade script, or by hand with :sqlfunc:`migrate_table_info`. In a
store written by an older version, a probability of 1 can be written once
more, until the first :sqlfunc:`circuit_cleanup`. An older ProvSQL
cannot read a store this version has written to.
