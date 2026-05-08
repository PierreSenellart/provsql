Case Study: Île-de-France Public Transit
==========================================

This case study, extending the scenario introduced in
:cite:`DBLP:journals/pvldb/SenellartJMR18`, applies ProvSQL to the
real-world GTFS dataset for Île-de-France public transit, demonstrating
Boolean provenance at scale for wheelchair accessibility reasoning.

The Scenario
------------

The `STIF GTFS dataset <https://www.data.gouv.fr/datasets/horaires-prevus-sur-les-lignes-de-transport-en-commun-dile-de-france-gtfs-datahub/>`_
describes hundreds of transit routes and tens of thousands of stops.  You
want to answer: starting from **Bagneux** station (served by RER B and
several bus lines), which stops and routes are reachable, and is the
entire journey from Bagneux to each destination *fully*
wheelchair-accessible?

Boolean provenance answers the second question: a result token evaluates
to ``true`` if and only if *every* record along the path has the
wheelchair flag set.

.. warning::

   This case study requires external data files.  The dataset is **not**
   bundled with ProvSQL due to its size (the compressed download is
   several hundred megabytes).  Download instructions are in
   :ref:`stif-setup` below.

.. _stif-setup:

Setup
-----

**Download the GTFS data.**  Obtain the Île-de-France GTFS archive from
`data.gouv.fr <https://www.data.gouv.fr/datasets/horaires-prevus-sur-les-lignes-de-transport-en-commun-dile-de-france-gtfs-datahub/>`_
(direct download: `IDFM-gtfs.zip <https://eu.ftp.opendatasoft.com/stif/GTFS/IDFM-gtfs.zip>`_).
Extract the archive; you will need the four text files
``routes.txt``, ``stops.txt``, ``trips.txt``, and ``stop_times.txt``.

**Load the schema and data.**  Download
:download:`setup.sql <../../casestudy3/setup.sql>`
and run it from the directory containing the four GTFS files::

    cd /path/to/gtfs-files
    psql -d mydb -f /path/to/setup.sql

This creates four tables:

* ``routes`` – transit lines (RER A, B, M1, bus 91, …)
* ``stops`` – individual stop points with GPS coordinates and a
  ``wheelchair_boarding`` flag
* ``trips`` – individual scheduled journeys, each with a
  ``wheelchair_accessible`` flag
* ``stop_times`` – arrival and departure times at each stop for each trip

The script also adds provenance tracking and creates a combined
``wheelchair`` mapping table from both the trip and stop wheelchair columns.

.. note::

   The setup script already creates the most important indexes
   (on ``stop_id``, ``trip_id``, and parent station), which are
   essential for acceptable performance on this large dataset.


Step 1: Explore the Database
-----------------------------

At the start of every session, set the search path so that ProvSQL functions
can be called without the ``provsql.`` prefix:

.. code-block:: postgresql

    SET search_path TO public, provsql;

Inspect the four tables:

.. code-block:: postgresql

    SELECT COUNT(*) FROM routes;
    SELECT COUNT(*) FROM stops;
    SELECT COUNT(*) FROM trips;
    SELECT COUNT(*) FROM stop_times;

To find the stop IDs for Bagneux station and its platforms:

.. code-block:: postgresql

    SELECT * FROM stops WHERE stop_name = 'Bagneux';


Step 2: Provenance and Wheelchair Mapping
------------------------------------------

Provenance has already been added by ``setup.sql``.  The ``wheelchair``
mapping table combines ``wheelchair_accessible`` from ``trips`` and
``wheelchair_boarding`` from ``stops``.  A result token evaluates to
``true`` (1) under :sqlfunc:`sr_boolean` if *every* contributing row
has its wheelchair column set to 1.

Inspect the mapping:

.. code-block:: postgresql

    SELECT * FROM wheelchair LIMIT 10;


Step 3: Reachable Stops from Bagneux
-------------------------------------

Find all stops reachable from Bagneux on the same trip and later in the
sequence – in other words, stops you can reach by boarding a vehicle at
Bagneux without changing:

.. code-block:: postgresql

    SELECT DISTINCT s2.stop_name, r2.route_long_name
    FROM stops s0
    JOIN stops      s1 ON s1.parent_station = s0.stop_id
    JOIN stop_times t1 ON s1.stop_id = t1.stop_id
    JOIN stop_times t2 ON t1.trip_id = t2.trip_id
                      AND t1.stop_sequence < t2.stop_sequence
    JOIN stops      s2 ON s2.stop_id = t2.stop_id
    JOIN trips      u2 ON u2.trip_id = t2.trip_id
    JOIN routes     r2 ON r2.route_id = u2.route_id
    WHERE s0.stop_name = 'Bagneux'
    ORDER BY r2.route_long_name, s2.stop_name;

This returns several dozen distinct (stop, route) pairs covering the reachable
network (the exact number depends on the GTFS dataset version).


Step 4: Boolean Provenance – Full Wheelchair Accessibility
----------------------------------------------------------

Add Boolean provenance evaluation to mark which results are fully
wheelchair-accessible along *every* leg.  Because the query returns one
row per trip (each with its own provenance circuit), materialize the
result first and then aggregate per destination:

.. code-block:: postgresql

    CREATE TEMP TABLE bagneux_b AS
      SELECT s2.stop_name,
             r2.route_long_name,
             sr_boolean(provenance(), 'wheelchair') AS accessible
      FROM stops s0
      JOIN stops      s1 ON s1.parent_station = s0.stop_id
      JOIN stop_times t1 ON s1.stop_id = t1.stop_id
      JOIN stop_times t2 ON t1.trip_id = t2.trip_id
                        AND t1.stop_sequence < t2.stop_sequence
      JOIN stops      s2 ON s2.stop_id = t2.stop_id
      JOIN trips      u2 ON u2.trip_id = t2.trip_id
      JOIN routes     r2 ON r2.route_id = u2.route_id
      WHERE s0.stop_name = 'Bagneux';

    SELECT stop_name, route_long_name, bool_or(accessible) AS accessible
    FROM bagneux_b
    GROUP BY stop_name, route_long_name
    ORDER BY route_long_name, stop_name;

:sqlfunc:`sr_boolean` evaluates the provenance token under the Boolean
semiring, looking up each leaf token in the ``wheelchair`` table.
A result of ``true`` means every record along *some* path from Bagneux
to that stop has the wheelchair flag set; ``false`` means no fully
accessible path exists.


Step 5: Inspect Individual Results with :sqlfunc:`sr_formula`
--------------------------------------------------------------

For a stop that is *not* fully accessible, use :sqlfunc:`sr_formula` to
identify which specific trip or stop is responsible.  Here we inspect
the ``Paul Bert`` stop on route 391 as an example:

.. code-block:: postgresql

    SELECT s2.stop_name,
           sr_formula(provenance(), 'wheelchair') AS formula
    FROM stops s0
    JOIN stops      s1 ON s1.parent_station = s0.stop_id
    JOIN stop_times t1 ON s1.stop_id = t1.stop_id
    JOIN stop_times t2 ON t1.trip_id = t2.trip_id
                      AND t1.stop_sequence < t2.stop_sequence
    JOIN stops      s2 ON s2.stop_id = t2.stop_id
    JOIN trips      u2 ON u2.trip_id = t2.trip_id
    JOIN routes     r2 ON r2.route_id = u2.route_id
    WHERE s0.stop_name = 'Bagneux'
      AND r2.route_long_name = '391'
      AND s2.stop_name = 'Paul Bert'
    LIMIT 1;

The formula shows which token carries a ``0`` wheelchair value,
pinpointing the accessibility barrier.  For example::

    stop_name | formula
    ----------+-----------------
    Paul Bert | (1 ⊗ 1 ⊗ 0 ⊗ 1)

The four factors correspond to the four provenance-enabled table
instances in the join: the Bagneux station record (``stops``, 1), its
platform record (``stops``, 1), the Paul Bert stop record
(``stops``, 0), and the trip record (``trips``, 1). The ``0`` on the
third factor pinpoints the specific Paul Bert stop served by route 391
as the accessibility barrier. Note that there are several stops named
``Paul Bert`` in the dataset; the one served by route 391 has
``wheelchair_boarding = 0``, as we can verify:

.. code-block:: postgresql

    SELECT DISTINCT s2.stop_name, s2.wheelchair_boarding
    FROM stops s0
    JOIN stops      s1 ON s1.parent_station = s0.stop_id
    JOIN stop_times t1 ON s1.stop_id = t1.stop_id
    JOIN stop_times t2 ON t1.trip_id = t2.trip_id
                      AND t1.stop_sequence < t2.stop_sequence
    JOIN stops      s2 ON s2.stop_id = t2.stop_id
    JOIN trips      u2 ON u2.trip_id = t2.trip_id
    JOIN routes     r2 ON r2.route_id = u2.route_id
    WHERE s0.stop_name = 'Bagneux'
      AND r2.route_long_name = '391'
      AND s2.stop_name = 'Paul Bert';
