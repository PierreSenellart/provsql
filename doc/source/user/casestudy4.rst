Case Study: Government Ministers Over Time
==========================================

This case study applies ProvSQL's temporal extension to a database of
French and Singaporean government ministers, demonstrating how provenance
tracks the *validity interval* of every fact and supports time-travel,
history, and data-modification undo.

The Scenario
------------

A database records which person held which governmental position and when.
Each row has a ``validity`` column of type ``tstzmultirange`` that describes
the time intervals during which the fact was true. Your tasks:

* find the full history of a person's positions,
* see who held a given role at a specific date,
* track all versions of a position through time,
* fire an official and then undo that action.


Setup
-----

This case study assumes a working ProvSQL installation on PostgreSQL 14 or
later (see :doc:`getting-provsql`).  The data files are distributed with
ProvSQL under ``doc/casestudy4/data/``.  Run the setup script from that
directory::

    cd /path/to/provsql/doc/casestudy4/data
    psql -d mydb -f /path/to/provsql/doc/casestudy4/setup.sql

This creates three tables:

* ``person`` – politicians with name, gender, birth/death dates, and a
  ``validity tstzmultirange`` column.
* ``holds`` – which person held which position in which country, with a
  ``validity tstzmultirange`` column.
* ``party`` – party memberships.

The script also:

* enables ``provsql.update_provenance`` so that every INSERT/UPDATE/DELETE
  is recorded in the ``update_provenance`` audit table,
* calls :sqlfunc:`add_provenance` on ``person`` and ``holds``,
* creates ``person_validity`` and ``holds_validity`` views via
  :sqlfunc:`create_provenance_mapping_view`, and
* extends ProvSQL's ``time_validity_view`` to incorporate both.


Step 1: Explore the Database
-----------------------------

Inspect the tables::

    SELECT * FROM person LIMIT 5;
    SELECT * FROM holds  LIMIT 5;

Every row carries a ``validity`` column (a ``tstzmultirange``) indicating
the period during which the row was true.

The convenience view ``person_position`` joins ``person`` and ``holds`` for
French officials::

    SELECT * FROM person_position LIMIT 10;


Step 2: Union of Temporal Intervals
-------------------------------------

:sqlfunc:`union_tstzintervals` is a custom semiring evaluation that computes
the *union of all temporal validity intervals* across the provenance circuit
of a result row.  Use it to reconstruct the full history of François Bayrou's
positions:

.. code-block:: postgresql

    SELECT position,
           union_tstzintervals(provenance(), 'time_validity_view') AS valid
    FROM person
    JOIN holds ON person.id = holds.id
    WHERE name = 'François Bayrou'
    ORDER BY valid;

Each row shows a position together with the union of all time windows during
which Bayrou held it.  Repeated or overlapping intervals are merged.


Step 3: Timeslice – Who Was in Government During Macron's First Term?
----------------------------------------------------------------------

:sqlfunc:`timeslice` returns all rows of a view that were valid during a
given time window:

.. code-block:: postgresql

    SELECT name, validity FROM
      timeslice('person_position', '2017-05-16', '2022-05-13')
      AS (name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
    ORDER BY validity;

Every minister whose tenure overlapped Macron's first presidential term
(May 2017 – May 2022) appears, together with their validity interval
intersected with the query window.

.. note::

   :sqlfunc:`timeslice` uses the ``time_validity_view`` mapping set up by
   ``setup.sql`` to look up the validity interval for each provenance token.
   The returned ``validity`` is the *union* over all provenance sources, which
   for a simple view equals the row's own validity column.


Step 4: History – All Holders of the Minister of Justice Role
--------------------------------------------------------------

:sqlfunc:`history` returns all versions of rows that match a set of column
filters, showing the full temporal evolution of a role:

.. code-block:: postgresql

    SELECT name, validity FROM
      history('person_position',
              ARRAY['position'],
              ARRAY['Minister of Justice'])
      AS (name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
    ORDER BY validity;

The result lists every person who served as Minister of Justice, ordered
by their validity interval.


Step 5: Timetravel – The Government on 19 June 1981
----------------------------------------------------

:sqlfunc:`timetravel` returns a snapshot of a table or view as it was at a
single point in time:

.. code-block:: postgresql

    SELECT name, position FROM
      timetravel('person_position', '1981-06-19')
      AS tt(name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
    ORDER BY position;

This reconstructs the French government on the day the Socialist Party
swept to power in the 1981 legislative elections.


Step 6: Data Modification – Replace the Prime Minister
-------------------------------------------------------

With ``provsql.update_provenance = on``, ProvSQL intercepts every
DML statement and records it in ``update_provenance``.  Dismiss
François Bayrou from the Prime Minister post and appoint a placeholder::

    DELETE FROM holds
    WHERE position = 'Prime Minister of France'
      AND id = (SELECT id FROM person WHERE name = 'François Bayrou');

    INSERT INTO person (id, name, gender)
      VALUES (100000, 'Jane Doe', 'female');
    INSERT INTO holds (id, position, country)
      VALUES (100000, 'Prime Minister of France', 'FR');

Verify the change::

    SELECT name, position FROM timetravel('person_position', NOW())
      AS tt(name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
    WHERE position = 'Prime Minister of France';

You should see Jane Doe, not François Bayrou.

Also inspect the Bayrou history post-firing::

    SELECT position,
           union_tstzintervals(provenance(), 'time_validity_view') AS valid
    FROM person
    JOIN holds ON person.id = holds.id
    WHERE name = 'François Bayrou'
    ORDER BY valid;

His Prime Minister interval now has a finite upper bound (the deletion
timestamp).


Step 7: Undo – Reinstate the Original Prime Minister
-----------------------------------------------------

The ``update_provenance`` table records every DML query with its
provenance token.  :sqlfunc:`undo` reverses any single recorded operation:

.. code-block:: postgresql

    SELECT undo(provenance()) FROM update_provenance;

This replays all recorded operations in reverse, restoring the original
state.  Re-query to confirm Bayrou is back:

.. code-block:: postgresql

    SELECT name, position FROM timetravel('person_position', NOW())
      AS tt(name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
    WHERE position = 'Prime Minister of France';

And verify his interval is again open-ended::

    SELECT position,
           union_tstzintervals(provenance(), 'time_validity_view') AS valid
    FROM person
    JOIN holds ON person.id = holds.id
    WHERE name = 'François Bayrou'
    ORDER BY valid;

.. note::

   :sqlfunc:`undo` reverses operations in the order they were recorded.
   The ``update_provenance`` table persists across sessions; clear it
   with ``DELETE FROM update_provenance`` when it is no longer needed.
