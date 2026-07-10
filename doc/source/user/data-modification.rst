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

Limitations
------------

Update tracking is still experimental, both in terms of operation support
and of performance.
