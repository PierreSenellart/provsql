Adding Provenance to a Table
============================

Before ProvSQL can track provenance, the extension must be loaded and
provenance must be enabled on each relevant table. ProvSQL represents
provenance as a circuit of gates following the model of
:cite:`DBLP:conf/icdt/DeutchMRT14`.

Loading the Extension
---------------------

In every database where you want provenance support:

.. code-block:: postgresql

    CREATE EXTENSION provsql CASCADE;

The ``CASCADE`` option installs the required ``uuid-ossp`` dependency
automatically.

To call ProvSQL functions without the ``provsql.`` prefix, add ``provsql``
to the search path at the start of each session:

.. code-block:: postgresql

    SET search_path TO public, provsql;

To make this permanent for a specific database:

.. code-block:: postgresql

    ALTER DATABASE mydb SET search_path TO public, provsql;

Most examples in this documentation omit the ``provsql.`` prefix, assuming
``provsql`` is in the search path.

Disabling Provenance Temporarily
----------------------------------

Setting ``provsql.active`` to ``off`` makes ProvSQL silently drop all
provenance annotations for the current session, as if the extension were
not loaded:

.. code-block:: postgresql

    SET provsql.active = off;

This is useful for running queries without provenance overhead while keeping
the extension installed. See :doc:`configuration` for all configuration
variables.

Enabling Provenance on a Table
-------------------------------

Use :sqlfunc:`add_provenance` to add provenance tracking to an existing table:

.. code-block:: postgresql

    SELECT provsql.add_provenance('mytable');

This adds a hidden ``provsql`` column of type ``uuid`` to the table. Each
row receives a freshly generated UUID that identifies a leaf (``input``) gate
in the provenance circuit.

After enabling provenance, every query that reads from ``mytable`` will
automatically carry provenance annotations in its result set.

.. note::

   :sqlfunc:`add_provenance` must be called on the base table, not on a view.

Accessing the Provenance Token
-------------------------------

The ``provsql`` column is intentionally opaque – it is silently removed
from ``WHERE`` or ``ORDER BY`` clauses. To refer to the current row's
provenance token, use the :sqlfunc:`provenance()` function:

.. code-block:: sql

    SELECT name, provenance() FROM mytable;

Within a query result, the ``provsql`` attribute carries a UUID value that represents the
provenance circuit gate for that tuple.

Removing Provenance
--------------------

To stop tracking provenance for a table (and drop the ``provsql`` column),
use :sqlfunc:`remove_provenance`:

.. code-block:: postgresql

    SELECT provsql.remove_provenance('mytable');

Provenance Mappings
--------------------

A *provenance mapping* associates provenance tokens with values from a table
column. Mappings are the bridge between abstract circuit tokens and
domain-meaningful labels used by semiring evaluation functions.
Use :sqlfunc:`create_provenance_mapping` to create one:

.. code-block:: postgresql

    SELECT create_provenance_mapping('my_mapping', 'mytable', 'column_name');

The mapping is stored as an ordinary PostgreSQL table called ``my_mapping``
with two columns: ``token`` (uuid) and ``value`` (text or numeric, depending
on the source column type).

Alternatively, :sqlfunc:`create_provenance_mapping_view` creates a *view*
instead of a table.  The view always reflects the current state of the
source table, which is useful when the table is frequently updated:

.. code-block:: postgresql

    SELECT create_provenance_mapping_view('my_mapping_view', 'mytable', 'column_name');

The view can be used anywhere a table-based mapping is expected (e.g., as
the second argument to semiring evaluation functions).

Inspecting the Circuit
-----------------------

:sqlfunc:`get_gate_type` and :sqlfunc:`get_children` let you inspect the
structure of the provenance circuit:

.. code-block:: postgresql

    SELECT provsql.get_gate_type(token);  -- returns the gate type
    SELECT provsql.get_children(token);   -- returns the child tokens

Gate types include ``input``, ``plus``, ``times``, ``monus``, ``project``,
``eq``, ``agg``, ``semimod``, ``zero``, ``one``, and others.
