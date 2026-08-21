Data Modification Tracking
===========================

ProvSQL can track the provenance of data-modification operations –
``INSERT``, ``UPDATE``, and ``DELETE`` – when run on provenance-enabled
tables :cite:`DBLP:conf/sigmod/BourhisDM20`.

.. note::

   Data modification tracking requires **PostgreSQL ≥ 14**.

Enabling Update Provenance
---------------------------

Update-provenance tracking is disabled by default. Enable it for a session:

.. code-block:: postgresql

    SET provsql.update_provenance = on;

Or permanently in ``postgresql.conf``:

.. code-block:: ini

    provsql.update_provenance = on

INSERT
-------

Inserting a row into a provenance-enabled table always creates a new
``input`` gate for that row. When ``update_provenance`` is enabled, the
statement additionally creates an ``update`` gate, logged in the
``update_provenance`` table, that is multiplied into each inserted
row's provenance -- so the insertion as a whole can later be undone:

.. code-block:: sql

    INSERT INTO employees(name, dept)
    VALUES ('Alice', 'R&D');

    -- The new row already has a provenance token
    SELECT name, provenance() FROM employees WHERE name = 'Alice';

DELETE
-------

Deleting a row does not remove it from the table, but the provenance is changed to mark the deletion, allowing hypothetical reasoning.
The
:sqlfunc:`undo` mechanism (see below) relies on this.

.. code-block:: sql

    DELETE FROM employees WHERE name = 'Alice';

UPDATE
-------

An ``UPDATE`` is modelled as a ``DELETE`` followed by an ``INSERT``. The
new row gets a fresh provenance token; the old token continues to exist in
the circuit.

.. code-block:: sql

    UPDATE employees SET dept = 'Sales' WHERE name = 'Bob';

Undoing Updates
----------------

ProvSQL provides an :sqlfunc:`undo` function that rolls back the
provenance effects of a specific logged modification. Every
provenance-enabled DML statement is recorded in the ``update_provenance``
table; pass its ``provsql`` token to :sqlfunc:`undo` to reverse its effect:

.. code-block:: sql

    CREATE TABLE t(id INT PRIMARY KEY);
    SELECT add_provenance('t');

    INSERT INTO t VALUES (1), (2), (3);
    DELETE FROM t WHERE id = 3;

    -- Row 3's provenance is now zeroed; undo the DELETE to restore it
    SELECT undo(provsql)
    FROM update_provenance
    WHERE query = 'DELETE FROM t WHERE id = 3;';

Transactions
-------------

Each statement mints an ``update`` gate of its own, and every statement of
one transaction also names the **transaction's** gate
(:sqlfunc:`transaction_token`): what a statement does to a row is recorded
as ``times(transaction, statement)``.  So the
``update_provenance`` table says which statements were one transaction --
its ``tx_token`` column names the transaction's gate and its ``xid``
column the PostgreSQL transaction id -- and :sqlfunc:`undo` works at
either granularity:

.. code-block:: sql

    BEGIN;
    DELETE FROM t WHERE id = 1;
    UPDATE t SET v = 'B' WHERE id = 2;
    COMMIT;

    -- Reverse the whole transaction, both statements at once
    SELECT undo(provsql)
    FROM update_provenance
    WHERE query_type = 'TRANSACTION'
    ORDER BY ts DESC LIMIT 1;

A transaction's own row has no ``query`` text -- a transaction is not a
statement -- so looking a statement up by its text finds the statement and
not the transaction that carried it.  Its validity is the universal range,
the identity of the temporal semiring: it multiplies into every effect of
the transaction, so anything narrower would intersect itself into all of
them.  When the transaction rolls back, nothing of it remains: the log
rows and the token rewrites are ordinary heap writes.

``ts`` and the start of ``valid_time`` are stamped **at commit**, not when
the statement ran.  ``CURRENT_TIMESTAMP`` is the transaction's start time,
so two overlapping transactions could otherwise commit in the opposite
order of the validity they recorded; a deferred trigger on
``update_provenance`` moves both to the commit instant.

The two notions of undo remain distinct and consistent: PostgreSQL's
``ROLLBACK`` removes a modification *and its record* -- the transaction
never happened -- while :sqlfunc:`undo` appends a compensating ``update``
gate and keeps the history: the modification happened and was reversed.

Limitations
------------

Update tracking is still experimental, both in terms of operation support
and of performance.

The "deleted rows stay in the table" model is implemented as a physical
delete followed by a re-insert carrying the ``monus`` token.  Under
``READ COMMITTED``, a concurrent transaction blocked on the same row
therefore sees, once the first commits, the original row version gone and
the re-inserted copy invisible to its snapshot: its own ``DELETE``
affects zero rows but still fires the statement trigger, logging an
``update`` gate that touches nothing.  Under ``REPEATABLE READ`` it gets a
serialization failure instead.  This is ordinary PostgreSQL behaviour for
a row rewritten under a concurrent reader, but it is worth knowing.
