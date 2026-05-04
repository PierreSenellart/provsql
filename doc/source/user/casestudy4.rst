Case Study: Government Ministers Over Time
==========================================

This case study, introduced in :cite:`DBLP:conf/pw/WidiaatmajaDDS25`,
applies ProvSQL's temporal extension to a database of French and
Singaporean government ministers, demonstrating how provenance tracks
the *validity interval* of every fact and supports time-travel,
history, and data-modification undo.

.. note::

   The data was imported semi-automatically from
   `Wikidata <https://www.wikidata.org>`_ and may contain imprecisions.
   It was current as of early 2026 and does not reflect subsequent political
   appointments.

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
later (see :doc:`getting-provsql`).  The data files are included in the
ProvSQL source distribution under ``doc/casestudy4/data/``.  Run the setup script from that
directory::

    cd /path/to/provsql/doc/casestudy4/data
    psql -d mydb -f ../setup.sql

This creates three tables:

* ``person`` – politicians with name, gender, birth/death dates, and a
  ``validity tstzmultirange`` column.
* ``holds`` – which person held which position in which country, with a
  ``validity tstzmultirange`` column.
* ``party`` – party memberships.

The script also:

* calls :sqlfunc:`add_provenance` on ``person`` and ``holds``,
* creates ``person_validity`` and ``holds_validity`` views via
  :sqlfunc:`create_provenance_mapping_view`, and
* extends ProvSQL's ``time_validity_view`` to incorporate both.


Step 1: Explore the Database
-----------------------------

At the start of every session, set the search path and timezone:

.. code-block:: postgresql

    SET search_path TO public, provsql;
    SET timezone TO 'UTC';

Inspect the tables:

.. code-block:: postgresql

    SELECT * FROM person LIMIT 5;
    SELECT * FROM holds  LIMIT 5;

Every row carries a ``validity`` column (a ``tstzmultirange``) indicating
the period during which the row was true.

The convenience view ``person_position`` joins ``person`` and ``holds`` for
French officials. Due to imprecisions in the Wikidata import, the view may
include entries for people who held positions in other countries; filtering
by a well-known position gives cleaner results:

.. code-block:: postgresql

    SELECT * FROM person_position
    WHERE position = 'Prime Minister of France'
    ORDER BY name;


Step 2: Union of Temporal Intervals
-------------------------------------

:sqlfunc:`union_tstzintervals` is a custom semiring evaluation that computes
the *union of all temporal validity intervals* across the provenance circuit
of a result row.  Use it to reconstruct the full history of Jacques Chirac's
positions:

.. code-block:: postgresql

    SELECT position,
           union_tstzintervals(provenance(), 'time_validity_view') AS valid
    FROM person
    JOIN holds ON person.id = holds.id
    WHERE name = 'Jacques Chirac'
    GROUP BY position
    ORDER BY valid;

Each row shows a position together with the union of all time windows during
which `Chirac <https://en.wikipedia.org/wiki/Jacques_Chirac>`_ held it.
His two terms as Prime Minister (1974–1976 and the
`1986–1988 cohabitation <https://en.wikipedia.org/wiki/Cohabitation_(government)>`_)
appear as two disjoint intervals in the multirange.


Step 3: Timeslice – Who Was in Government During Macron's First Term?
----------------------------------------------------------------------

:sqlfunc:`timeslice` returns all rows of a view that were valid during a
given time window:

.. code-block:: postgresql

    SELECT name, validity FROM
      timeslice('person_position', '2017-05-16', '2022-05-13')
      AS (name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
    ORDER BY validity;

Every minister whose tenure overlapped
`Macron's first presidential term <https://en.wikipedia.org/wiki/Presidency_of_Emmanuel_Macron>`_
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


Step 5: Timetravel – The Socialist Government of July 1981
-----------------------------------------------------------

:sqlfunc:`timetravel` returns a snapshot of a table or view as it was at a
single point in time:

.. code-block:: postgresql

    SELECT name, position FROM
      timetravel('person_position', '1981-07-01')
      AS tt(name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
    ORDER BY position;

This reconstructs the French government installed after the
`Socialist Party's victory in the June 1981 legislative elections
<https://en.wikipedia.org/wiki/1981_French_legislative_election>`_,
with ministers such as
`Robert Badinter <https://en.wikipedia.org/wiki/Robert_Badinter>`_ (Justice),
`Jacques Delors <https://en.wikipedia.org/wiki/Jacques_Delors>`_ (Economy),
and `Jack Lang <https://en.wikipedia.org/wiki/Jack_Lang_(French_politician)>`_ (Culture).


Step 6: Data Modification – Replace the Prime Minister
-------------------------------------------------------

.. important::

   Steps 6 and 7 require ``provsql.update_provenance`` to be enabled.
   Run this before proceeding:

   .. code-block:: postgresql

       SET provsql.update_provenance = on;

ProvSQL intercepts every DML statement and records it in
``update_provenance``.  First, record who currently holds the position,
then dismiss them and appoint a placeholder:

.. code-block:: postgresql

    CREATE TEMP TABLE fired_pm AS
      SELECT person.id, name FROM person
      JOIN holds ON person.id = holds.id
      WHERE position = 'Prime Minister of France'
        AND holds.validity @> now()::timestamptz;

    DELETE FROM holds
    WHERE position = 'Prime Minister of France'
      AND holds.validity @> now()::timestamptz;

    INSERT INTO person (id, name, gender)
      VALUES (100000, 'Jeanne Dupont', 'female');
    INSERT INTO holds (id, position, country)
      VALUES (100000, 'Prime Minister of France', 'FR');

Verify the change:

.. code-block:: postgresql

    SELECT name, position FROM timetravel('person_position', NOW())
      AS tt(name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
    WHERE position = 'Prime Minister of France';

You should see Jeanne Dupont, not the original Prime Minister.

Also inspect the fired PM's history post-firing:

.. code-block:: postgresql

    SELECT position,
           union_tstzintervals(provenance(), 'time_validity_view') AS valid
    FROM person
    JOIN holds ON person.id = holds.id
    JOIN fired_pm ON person.id = fired_pm.id
    GROUP BY position;

Their Prime Minister interval now has a finite upper bound (the deletion
timestamp).


Step 7: Undo – Reinstate the Original Prime Minister
-----------------------------------------------------

The ``update_provenance`` table records every DML query with its
provenance token.  :sqlfunc:`undo` reverses any single recorded operation:

.. code-block:: postgresql

    SELECT undo(provenance()) FROM update_provenance;

This replays all recorded operations in reverse, restoring the original
state.  Re-query to confirm the original Prime Minister is back:

.. code-block:: postgresql

    SELECT name, position FROM timetravel('person_position', NOW())
      AS tt(name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
    WHERE position = 'Prime Minister of France';

And verify their interval is again open-ended:

.. code-block:: postgresql

    SELECT position,
           union_tstzintervals(provenance(), 'time_validity_view') AS valid
    FROM person
    JOIN holds ON person.id = holds.id
    JOIN fired_pm ON person.id = fired_pm.id
    GROUP BY position;

.. note::

   :sqlfunc:`undo` reverses each recorded operation independently.
   The ``update_provenance`` table persists across sessions; clear it
   with ``DELETE FROM update_provenance`` when it is no longer needed.
