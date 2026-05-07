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

ProvSQL Studio
---------------

ProvSQL Studio's :ref:`schema panel <studio-schema-panel>` is an
interactive surface for the operations above:

* it lists every ``SELECT``-able relation, with a purple :sc:`prov`
  pill on tables whose ``provsql`` column is injected by the planner
  (provenance tracking is active) and a gold :sc:`mapping` pill on
  relations shaped ``(value <T>, provenance uuid)``;
* :guilabel:`+ prov` and :guilabel:`− prov` action chips on
  provenance-eligible plain tables prefill the corresponding
  ``SELECT add_provenance(...)`` / ``SELECT remove_provenance(...)``
  call into the query box;
* clicking a column on a tracked table prefills a
  ``SELECT create_provenance_mapping('<table>_<col>_mapping',
  '<schema>.<table>', '<col>');`` call, so a fresh provenance
  mapping is two clicks away.

.. _circuit-gates:

Inspecting the Circuit
-----------------------

ProvSQL represents provenance as a *circuit*: a directed acyclic graph
(DAG) of *gates*.  Each tuple in a provenance-tracked table is
associated with an ``input`` gate, created lazily the first time that
tuple appears in a query result.  Tuples may also carry more complex
provenance -- for instance, rows created by ``INSERT ... SELECT`` or
``CREATE TABLE AS`` inherit the provenance expression of the source
query.

As queries combine tuples, internal gates record the semiring
operations that were applied:

- ``plus`` (⊕): alternative derivations (``UNION``, ``DISTINCT``)
- ``times`` (⊗): combined use (``JOIN``, cross product)
- ``monus`` (⊖): difference (``EXCEPT``)
- ``delta`` (δ): aggregation boundary (``GROUP BY``)
- ``agg``, ``semimod``: aggregate provenance
- ``project``, ``eq``: where-provenance (column tracking, equijoin)
- ``cmp``: ``HAVING`` comparisons

Two constant gates represent the semiring identity elements:
:sqlfunc:`gate_zero` (additive identity, ``𝟘``) and
:sqlfunc:`gate_one` (multiplicative identity, ``𝟙``).

The following functions let you navigate and inspect the circuit:

- :sqlfunc:`get_gate_type` -- returns the type of a gate.
- :sqlfunc:`get_children` -- returns the child tokens of a gate.
- :sqlfunc:`identify_token` -- given a provenance token, returns the
  source table and row it originates from.
- :sqlfunc:`get_infos` -- returns the integer metadata attached to a gate
  (e.g., aggregate function OID, comparison operator OID).
- :sqlfunc:`get_extra` -- returns the text metadata attached to a gate
  (e.g., aggregate value, column positions for where-provenance).
- :sqlfunc:`get_nb_gates` -- returns the total number of gates in the
  circuit, useful for diagnosing circuit size and performance.

.. code-block:: postgresql

    SELECT provsql.get_gate_type(provenance()) FROM mytable;
    SELECT provsql.get_children(provenance()) FROM mytable;
