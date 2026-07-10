Where-Provenance
=================

*Where-provenance* :cite:`DBLP:conf/icdt/BunemanKT01` tracks, for each
value in a query result, the specific cell (table, row, and column) in the
database from which it originated. This is a finer-grained form of
provenance than token-based lineage.

Enabling Where-Provenance
--------------------------

Where-provenance is disabled by default. Enable it for a session with:

.. code-block:: postgresql

    SET provsql.provenance = 'where';

Or enable it permanently in ``postgresql.conf``:

.. code-block:: ini

    provsql.provenance = 'where'

When enabled, each output cell in a provenance-tracked query carries an
annotation identifying its source.

.. note::

   Where-provenance adds overhead to every query. Enable it only when
   needed.

Using Where-Provenance
-----------------------

With where-provenance enabled, you can query the source location of a
value using the :sqlfunc:`where_provenance` function:

.. code-block:: postgresql

    SET provsql.provenance = 'where';

.. code-block:: sql

    SELECT name, where_provenance(provenance())
    FROM employees;

The function takes the row's provenance token and returns a text
annotation describing, for each output column of the row, the source
cell(s) it originated from.

For interactive exploration, see Studio's
:ref:`Where mode <studio-where-mode>`. It runs your query, displays
the result alongside the source relations, and highlights the
contributing cells when you hover over an output value: no explicit
call to :sqlfunc:`where_provenance` required.

Projection Gates
-----------------

Where-provenance introduces additional gate types in the circuit:

* ``project`` – tracks which column a value was projected from.
* ``eq`` – tracks equijoin conditions that constrained a value.

These gates appear alongside the usual ``plus``/``times`` gates when
where-provenance is active.

Example
--------

.. code-block:: sql

    SELECT add_provenance('person');

    -- Track where each output value came from
    SELECT p.name, where_provenance(provenance()) AS source
    FROM person p
    JOIN sightings s ON p.id = s.person;

Limitations
------------

Where-provenance is an experimental feature. Not all SQL constructs are
fully supported when where-provenance is enabled. In particular,
aggregate queries may not annotate all output cells correctly.
