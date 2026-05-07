Temporal Features
==================

ProvSQL provides support for *temporal databases* – databases where data
validity is associated with time intervals
:cite:`DBLP:conf/pw/WidiaatmajaDDS25`. This feature is implemented on top
of the data-modification tracking infrastructure.

.. note::

   Temporal features require **PostgreSQL ≥ 14**.

Overview
--------

Temporal provenance allows you to track when each fact was valid, represent
intervals of validity, and query the database "as of" a given point in time.
The implementation uses the PostgreSQL
`tstzmultirange <https://www.postgresql.org/docs/current/rangetypes.html>`_
type to represent validity periods.

Temporal Tables
---------------

A temporal table is a provenance-enabled table augmented with a validity
interval column. Helper functions are provided to create and manage such
tables.

Valid-Time Queries
------------------

:sqlfunc:`get_valid_time` returns the validity interval of a fact as a
``tstzmultirange``, computed from the provenance circuit and the
modification history:

.. code-block:: postgresql

    SELECT *, get_valid_time(provsql, 'mytable') AS valid_time
    FROM mytable;

You can filter to only currently-valid facts:

.. code-block:: postgresql

    SELECT * FROM mytable
    WHERE get_valid_time(provsql, 'mytable') @> CURRENT_TIMESTAMP;

Union of Validity Intervals
-----------------------------

:sqlfunc:`union_tstzintervals` computes the union of validity intervals
associated with a query result via its provenance:

.. code-block:: postgresql

    SELECT entity_id,
           union_tstzintervals(provenance(), 'interval_mapping')
    FROM temporal_table;

For queries that involve HAVING clauses, aggregation, or
where-provenance, prefer the compiled :sqlfunc:`sr_temporal` evaluator
(see :doc:`semirings`); it computes the same quantity but supports the
full set of circuit gate types.

Temporal Query Functions
-------------------------

ProvSQL provides additional functions for time-travel queries:

:sqlfunc:`timetravel` returns all versions of a table that were valid
at a given point in time:

.. code-block:: postgresql

    SELECT * FROM timetravel('mytable', CURRENT_TIMESTAMP)
      AS t(id int, value int, valid_time tstzmultirange, provsql uuid);

:sqlfunc:`timeslice` returns all versions valid during a given interval:

.. code-block:: postgresql

    SELECT * FROM timeslice('mytable',
                            CURRENT_TIMESTAMP - INTERVAL '1 day',
                            CURRENT_TIMESTAMP)
      AS t(id int, value int, valid_time tstzmultirange, provsql uuid);

:sqlfunc:`history` returns the full modification history for a specific
entity, identified by key column values:

.. code-block:: postgresql

    SELECT * FROM history('mytable', ARRAY['id'], ARRAY['42'])
      AS t(id int, value int, valid_time tstzmultirange, provsql uuid);

Relationship to Data Modification Tracking
-------------------------------------------

Temporal support is built on top of data modification tracking
(see :doc:`data-modification`). The provenance circuit records the full
history of insertions and deletions, which is then interpreted temporally
by the interval-aware evaluation functions.
