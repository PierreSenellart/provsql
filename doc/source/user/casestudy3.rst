Case Study: Île-de-France Public Transit
==========================================

This case study, extending the scenario introduced in
:cite:`DBLP:journals/pvldb/SenellartJMR18`, applies ProvSQL to the
real-world GTFS dataset for Île-de-France public transit. It demonstrates
Boolean provenance at scale for wheelchair accessibility reasoning, then
journeys with changes as a ``WITH RECURSIVE`` query over the whole network,
whose rows the same query evaluates in several semirings: shortest travel
time, accessibility, the equations of the recursion and the journeys they
describe, the fastest journey itself, step by step, which of the metro and
the train is faster, and the probability of reaching a station when parts
of the network are disrupted.

.. note::

   Unlike the other case studies, this one is **not available in the ProvSQL
   Playground**: it loads the large Île-de-France GTFS dataset, which is
   not bundled and must be fetched separately (see the :ref:`Playground note
   <playground-note>`). Run it against a local ProvSQL installation.

The Scenario
------------

Île-de-France is the region around Paris. Its network combines the
*metro*, inside Paris; the *RER*, express trains crossing the city and
running out to the suburbs (lines A to E); suburban trains; trams; and
buses, including the night buses (*Noctilien*, lines numbered ``N…``).

The `STIF GTFS dataset <https://www.data.gouv.fr/datasets/horaires-prevus-sur-les-lignes-de-transport-en-commun-dile-de-france-gtfs-datahub/>`_
describes hundreds of transit routes and tens of thousands of stops, in
GTFS, the General Transit Feed Specification: a set of CSV files whose
columns the `GTFS reference
<https://gtfs.org/documentation/schedule/reference/>`_ documents.

Starting from **Luxembourg** station, in the centre of Paris (served by
RER B and several bus lines), you want to know:

* which stops and lines are reachable without changing, and whether the
  journey is *fully* wheelchair-accessible: Boolean provenance answers this,
  a result token evaluating to ``true`` if and only if *every* record along
  the trip has the wheelchair flag set;
* which stations are reachable at all, changing as often as needed, how
  long it takes, whether some journey is accessible, and how likely the
  journey remains when lines are disrupted: a recursive query over the
  network answers all four.

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
and run it from the directory containing the four GTFS files:

.. code-block:: bash

    cd /path/to/gtfs-files
    psql -d mydb -f /path/to/setup.sql

Loading the files, millions of rows of timetable among them, takes a
couple of minutes. This creates four tables:

* ``routes`` -- transit lines (RER A, B, M1, bus 91…)
* ``stops`` -- individual stop points with GPS coordinates and a
  ``wheelchair_boarding`` flag
* ``trips`` -- individual scheduled journeys, each with a
  ``wheelchair_accessible`` flag
* ``stop_times`` -- arrival and departure times at each stop for each trip

The script also adds provenance tracking to ``trips`` and ``stops``, and
normalises their wheelchair columns to 1 (accessible) or 0.

.. note::

   The setup script already creates the most important indexes
   (on ``stop_id``, ``trip_id``, and parent station), which are
   essential for acceptable performance on this large dataset.


Step 1: Explore the Database
-----------------------------

The setup script has called :sqlfunc:`setup_search_path`, which adds
``provsql`` to the database's ``search_path``: every new session calls
ProvSQL's functions without the ``provsql.`` prefix.

Inspect the four tables:

.. code-block:: postgresql

    SELECT COUNT(*) FROM routes;
    SELECT COUNT(*) FROM stops;
    SELECT COUNT(*) FROM trips;
    SELECT COUNT(*) FROM stop_times;

The ``stops`` table has two kinds of rows, told apart by
``location_type``. A *stop* (``location_type`` 0) is where a vehicle
calls: a platform, or a bus stop; it is what ``stop_times`` refers to. A
*station* (``location_type`` 1) groups the stops of one place: each of
its stops names it in its ``parent_station`` column. Luxembourg station
thus groups its RER platforms and the bus stops around it. To find it and
its stops:

.. code-block:: postgresql

    SELECT * FROM stops WHERE stop_name = 'Luxembourg';

Two stations carry the name: ``IDFM:71161``, Luxembourg in Paris, and
``IDFM:65716``, a namesake elsewhere in the region. The queries below
start from the first, and reach its trips through its stops.


Step 2: Provenance and Wheelchair Mapping
------------------------------------------

Provenance has already been added to ``trips`` and ``stops`` by
``setup.sql``: every row of a query over them carries the provenance of the
trip and stop records it reads. Their wheelchair columns become the values
of a provenance mapping, which associates each input token with a value.
Each call below builds a mapping for one table; their union covers the
tokens of both, and an index speeds up the lookups in this large mapping:

.. code-block:: postgresql

    DROP TABLE IF EXISTS w_trips, w_stops, wheelchair;
    SELECT create_provenance_mapping('w_trips', 'trips', 'wheelchair_accessible');
    SELECT create_provenance_mapping('w_stops', 'stops', 'wheelchair_boarding');
    CREATE TABLE wheelchair AS
      SELECT * FROM w_trips UNION SELECT * FROM w_stops;
    CREATE INDEX ON wheelchair(provenance);

A result token evaluates to ``true`` (1) under :sqlfunc:`sr_boolean` if
*every* contributing row has its wheelchair column set to 1. Inspect the
mapping:

.. code-block:: postgresql

    SELECT * FROM wheelchair LIMIT 10;


Step 3: Reachable Stops from Luxembourg
---------------------------------------

Find all stops reachable from Luxembourg on the same trip and later in the
sequence -- in other words, stops you can reach by boarding a vehicle at
Luxembourg without changing:

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
    WHERE s0.stop_id = 'IDFM:71161'
    ORDER BY r2.route_long_name, s2.stop_name;

This returns every (stop, line) pair reachable without changing, on RER B
and on the bus lines through the station.


Step 4: Boolean Provenance -- Full Wheelchair Accessibility
-----------------------------------------------------------

Add Boolean provenance evaluation to mark which results are fully
wheelchair-accessible along *every* leg.  Because the query returns one
row per trip (each with its own provenance circuit), materialise the
result first and then aggregate per destination:

.. code-block:: postgresql

    CREATE TEMP TABLE from_luxembourg AS
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
      WHERE s0.stop_id = 'IDFM:71161';

    SELECT remove_provenance('from_luxembourg');

    SELECT stop_name, route_long_name, bool_or(accessible) AS accessible
    FROM from_luxembourg
    GROUP BY stop_name, route_long_name
    ORDER BY route_long_name, stop_name;

:sqlfunc:`sr_boolean` evaluates the provenance token of each row under
the Boolean semiring, looking up each leaf token in the ``wheelchair``
table: ``true`` when every record the row reads (the station, its platform,
the stop, the trip) has the wheelchair flag set, i.e., when that trip
makes that journey accessibly. The table holds a row per trip and stop,
hundreds of thousands of them, so this step takes a little while.

The materialised table still carries the ``provsql`` provenance column,
so :sqlfunc:`remove_provenance` drops tracking first, and the final
aggregation runs outside ProvSQL. ``bool_or`` asks whether *some* trip
of the line reaches the stop accessibly from Luxembourg: one can wait for
an accessible vehicle. ``bool_and`` would ask that *every* trip does; on
this network, the two rarely differ.


Step 5: Inspect Individual Results with :sqlfunc:`sr_formula`
--------------------------------------------------------------

For a stop that is *not* fully accessible, use :sqlfunc:`sr_formula` to
identify which specific trip or stop is responsible.  Here we inspect
the ``Musée du Louvre`` stop on bus line 27 as an example:

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
    WHERE s0.stop_id = 'IDFM:71161'
      AND r2.route_long_name = '27'
      AND s2.stop_name = 'Musée du Louvre'
    LIMIT plain(1);

The formula shows which token carries a ``0`` wheelchair value,
pinpointing the accessibility barrier:

.. code-block:: text

    stop_name       | formula
    ----------------+---------------
    Musée du Louvre | 1 ⊗ 1 ⊗ 0 ⊗ 1

The four factors correspond to the four provenance-enabled table
instances in the join: the Luxembourg station record (``stops``, 1), its
platform record (``stops``, 1), the Musée du Louvre stop record
(``stops``, 0), and the trip record (``trips``, 1). The ``0`` on the third
factor pinpoints the specific Musée du Louvre stop served by line 27 as
the accessibility barrier. Several stops are named ``Musée du Louvre`` in
the dataset; the one served by line 27 has ``wheelchair_boarding = 0``, as
we can verify:

.. code-block:: postgresql

    SELECT DISTINCT s2.stop_id, s2.wheelchair_boarding
    FROM stops s0
    JOIN stops      s1 ON s1.parent_station = s0.stop_id
    JOIN stop_times t1 ON s1.stop_id = t1.stop_id
    JOIN stop_times t2 ON t1.trip_id = t2.trip_id
                      AND t1.stop_sequence < t2.stop_sequence
    JOIN stops      s2 ON s2.stop_id = t2.stop_id
    JOIN trips      u2 ON u2.trip_id = t2.trip_id
    JOIN routes     r2 ON r2.route_id = u2.route_id
    WHERE s0.stop_id = 'IDFM:71161'
      AND r2.route_long_name = '27'
      AND s2.stop_name = 'Musée du Louvre';


Step 6: The Next Stop on Each Line (``LATERAL``)
-------------------------------------------------

Step 3 listed *every* reachable stop. A ``LATERAL`` subquery asks a more
focused question -- for each line through Luxembourg, what is the *very
next* stop after it on that trip:

.. code-block:: postgresql

    SELECT DISTINCT r.route_long_name,
           nxt.stop_name AS next_stop,
           sr_boolean(provenance(), 'wheelchair') AS accessible
    FROM stops s0
    JOIN stops      s1 ON s1.parent_station = s0.stop_id
    JOIN stop_times t1 ON s1.stop_id = t1.stop_id
    JOIN trips      u  ON u.trip_id = t1.trip_id
    JOIN routes     r  ON r.route_id = u.route_id
    JOIN LATERAL (
      SELECT s2.stop_name
      FROM stop_times t2
      JOIN stops      s2 ON s2.stop_id = t2.stop_id
      WHERE t2.trip_id = t1.trip_id
        AND t2.stop_sequence > t1.stop_sequence
      ORDER BY t2.stop_sequence
      LIMIT plain(1)
    ) nxt ON true
    WHERE s0.stop_id = 'IDFM:71161'
    ORDER BY r.route_long_name, nxt.stop_name;

The ``LATERAL`` subquery runs once per outer row and may reference its
columns (``t1.trip_id``, ``t1.stop_sequence``); the ``ORDER BY … LIMIT
plain(1)`` keeps only the immediately following stop. ``stops`` and
``trips`` are provenance-tracked (the untracked ``stop_times`` and
``routes`` contribute *certain* provenance, as in an ordinary join), so
each ``(line, next stop)`` row carries the lineage of the records that
produced it, and :sqlfunc:`sr_boolean` reports whether the hop to the
next stop is wheelchair-accessible.

Without ``plain``, ``ORDER BY … LIMIT 1`` would be read in every
possible world: each later stop would be a candidate, conditioned on no
earlier stop being present. Here a token stands for an accessible record,
so this would ask for the next *accessible* stop, another question, and a
costly one: each row would carry a condition over all the later stops of
its trip, for every trip through Luxembourg, and the query does not finish
in reasonable time. Every stop exists, so which stop comes next is a plain
fact of the timetable: ``LIMIT plain(1)`` keeps the next stop of the
actual data, with the provenance its row has in the full result.
ProvSQL emits a ``WARNING`` about this ``LIMIT`` in a subquery, since
that provenance does not say that the stop is the next one; here, the
answer means what it says. See :ref:`limit` for the general rule.


Step 7: The Network as a Table
------------------------------

Steps 3 to 6 stayed on one vehicle. To change lines, describe the network
itself: one row per pair of consecutive stations on a line, with the
average scheduled time between them and whether some trip makes that hop
accessibly. This is plain SQL over the timetable, a long computation that
needs no provenance: turn the rewriting off with ``provsql.active`` while
it runs, rather than tracking the provenance of millions of rows for
nothing.

.. code-block:: postgresql

    SET provsql.active = off;
    DROP TABLE IF EXISTS hop_minutes, hop_access, hop_name, hop_one, hop;
    DROP VIEW IF EXISTS trip_hops, calls;

First, every call of a trip at a stop, with its line, the station of the
stop, its arrival time in seconds since midnight, and whether both the
trip and the stop are accessible:

.. code-block:: postgresql

    CREATE TEMP VIEW calls AS
      SELECT st.trip_id, st.stop_sequence, r.route_short_name AS line,
             r.route_type,
             coalesce(nullif(s.parent_station, ''), s.stop_id) AS station,
             split_part(st.arrival_time, ':', 1)::int * 3600
               + split_part(st.arrival_time, ':', 2)::int * 60
               + split_part(st.arrival_time, ':', 3)::int AS arrival,
             t.wheelchair_accessible = 1 AND s.wheelchair_boarding = 1
               AS accessible
      FROM stop_times st
      JOIN trips t  ON t.trip_id = st.trip_id
      JOIN routes r ON r.route_id = t.route_id
      JOIN stops s  ON s.stop_id = st.stop_id;

Then every hop of a trip, a call and the next call of the same trip, which
the window function ``lead`` reads in the order of the trip. Its time runs
from arrival to arrival, so that it counts the stop at the station, still
on board; the hop is accessible when both its calls are:

.. code-block:: postgresql

    CREATE TEMP VIEW trip_hops AS
      SELECT line, route_type, station AS from_station,
             lead(station) OVER w AS to_station,
             (lead(arrival) OVER w - arrival) / 60.0 AS minutes,
             accessible AND lead(accessible) OVER w AS accessible
      FROM calls
      WINDOW w AS (PARTITION BY trip_id ORDER BY stop_sequence);

Finally, one row per hop of a line, over all its trips: the average time,
and whether some trip makes it accessibly. Bus schedules give times to the
minute, so a single trip often shows a hop as 0 or 1 minute; the average
evens this rounding out. The last call of a trip has no next one, and a
hop with no scheduled time is left out:

.. code-block:: postgresql

    CREATE TABLE hop AS
      SELECT from_station, to_station, line, route_type,
             round(avg(minutes), 1)::float8 AS minutes,
             bool_or(accessible)::int AS accessible
      FROM trip_hops
      WHERE to_station IS NOT NULL AND to_station <> from_station
      GROUP BY from_station, to_station, line, route_type
      HAVING avg(minutes) IS NOT NULL;
    RESET provsql.active;

The table has tens of thousands of rows and takes up to a minute to build,
over the millions of rows of ``stop_times``. Since the rewriting was off,
its rows carry no provenance at all: the link with the tokens of ``trips``
and ``stops`` is lost, and ``accessible`` is just a copied value, no longer
a formula over the trips and stops it came from. ``add_provenance`` makes
``hop`` a base relation of its own, each hop with a fresh token, which the
queries below combine like those of any other input; map these tokens to
the times and accessibility of their hops:

.. code-block:: postgresql

    SELECT add_provenance('hop');
    SELECT create_provenance_mapping('hop_minutes', 'hop', 'minutes');
    SELECT create_provenance_mapping('hop_access', 'hop', 'accessible');


Step 8: Every Station Reachable from Luxembourg
-----------------------------------------------

A journey with changes is a path in this network: a recursive query
follows the hops from Luxembourg, a line change being just another hop out
of the same station:

.. code-block:: postgresql

    WITH RECURSIVE reach(station) AS (
        SELECT 'IDFM:71161'::text
      UNION
        SELECT h.to_station FROM reach r JOIN hop h ON h.from_station = r.station)
    SELECT count(*) FROM reach;

The whole network, in a few seconds. The network is
full of cycles (every line runs both ways), so a station has infinitely
many derivations: ProvSQL records the provenance of each row as one
unknown of a system of equations over the rows of the cycle, which every
evaluation then solves (see :ref:`recursive-queries`).


Step 9: How Long, and Is There an Accessible Journey?
-----------------------------------------------------

The same query, evaluated in two semirings. In the tropical semiring of
:sqlfunc:`sr_tropical`, alternatives take the minimum and a journey the sum
of its hops: the shortest travel time. In the Boolean semiring of
:sqlfunc:`sr_boolean`, over the ``accessible`` column: whether some journey
uses accessible hops only. A ``VALUES`` list names a few destinations;
it has no provenance of its own:

.. code-block:: postgresql

    WITH RECURSIVE reach(station) AS (
        SELECT 'IDFM:71161'::text
      UNION
        SELECT h.to_station FROM reach r JOIN hop h ON h.from_station = r.station)
    SELECT d.name,
           round(sr_tropical(provenance(), 'hop_minutes',
                             nonnegative => true)::numeric, 1) AS minutes,
           sr_boolean(provenance(), 'hop_access') AS accessible
    FROM reach r
    JOIN (VALUES ('IDFM:474151', 'Châtelet - Les Halles'),
                 ('IDFM:71410', 'Gare du Nord'),
                 ('IDFM:71517', 'La Défense'),
                 ('IDFM:73721', 'Versailles Château Rive Gauche'),
                 ('IDFM:73699', 'Aéroport CDG (Terminal 2)'),
                 ('IDFM:68385', 'Marne-la-Vallée - Chessy'))
           AS d(station, name)
      USING (station)
    ORDER BY minutes;

Châtelet - Les Halles, two stops away on RER B, comes first; the airport
and Chessy, at the far ends of the network, last.

These are times on board only, well below what a traveller would see. In
this network, changing lines is instant, and a train is always there to
board: a journey is free to hop from line to line at every station, and
the fastest ones do, catching a bus or a night bus for a single stop when
it saves a minute. Real journeys add, at every change, the walk between
platforms, sometimes several minutes in a large station, and, at every
boarding, the wait for the next departure. The dataset has what it takes
to account for both: the departure times are in ``stop_times``, and a file
this case study does not load, ``transfers.txt``, gives the minimum time to
change between two stops (another, ``pathways.txt``, even gives the walks
from the entrances of a station, with their stairs). A model using them would
have a node per station and line, with change hops between them, or, to
count the waits, a node per departure; the queries stay the same recursion
over a larger table.

Each semiring solves the equations by the method its properties allow.
The Boolean semiring, and the tropical one over the nonnegative costs that
``nonnegative => true`` declares, are *absorptive* (a cycle never improves
a journey) and *selective* (an alternative is one of the two), which
makes Dijkstra's algorithm correct; without ``nonnegative``, a cost might be
negative and a cycle could lower it, and the equations are solved by
Gaussian elimination instead, for the same values here.


Step 10: Not Every Semiring Has an Answer
-----------------------------------------

Counting the derivations of a station in the counting semiring:

.. code-block:: postgresql

    SELECT create_provenance_mapping('hop_one', 'hop', '1');

    WITH RECURSIVE reach(station) AS (
        SELECT 'IDFM:71161'::text
      UNION
        SELECT h.to_station FROM reach r JOIN hop h ON h.from_station = r.station)
    SELECT sr_counting(provenance(), 'hop_one')
    FROM reach WHERE station = 'IDFM:474151';

is refused: going round a cycle any number of times gives infinitely many
derivations, which no integer counts.


Step 11: The Equations of a Recursion
-------------------------------------

:sqlfunc:`sr_formula` shows the equations themselves, and
:sqlfunc:`sr_regex` their solution, as a regular expression over the hops.
On the whole network they are far too long to read; here they are
restricted to RER B between Luxembourg and its two neighbours. Label
every metro and RER hop (``route_type`` 1 and 2) with its line and
stations. ``plain(NULL::stops)`` reads the station names without their
provenance, so that ``provenance()`` is the token of the hop alone, and
the stops' own tokens stay out of the result as well:

.. code-block:: postgresql

    CREATE TABLE hop_name AS
      SELECT provenance(), h.line || ': ' || f.stop_name || ' → ' || t.stop_name
               AS value
      FROM hop h JOIN plain(NULL::stops) f ON f.stop_id = h.from_station
                 JOIN plain(NULL::stops) t ON t.stop_id = h.to_station
      WHERE h.route_type IN (1, 2);

    WITH RECURSIVE reach(station) AS (
        SELECT 'IDFM:71161'::text
      UNION
        SELECT h.to_station FROM reach r JOIN hop h ON h.from_station = r.station
        WHERE h.line = 'B'
          AND h.to_station IN ('IDFM:71161', 'IDFM:71106', 'IDFM:73620'))
    SELECT s.stop_name, sr_formula(provenance(), 'hop_name'),
           sr_regex(provenance(), 'hop_name')
    FROM reach r JOIN plain(NULL::stops) s ON s.stop_id = r.station;

For Port Royal, the equations are:

.. code-block:: text

    x₁ where x₁ = x₂ ⊗ B: Luxembourg → Port Royal,
             x₂ = 𝟙 ⊕ (x₁ ⊗ B: Port Royal → Luxembourg)
                    ⊕ (x₃ ⊗ B: Saint-Michel Notre-Dame → Luxembourg),
             x₃ = x₂ ⊗ B: Luxembourg → Saint-Michel Notre-Dame

One unknown per station: Luxembourg (``x₂``) holds from the start (``𝟙``)
or by coming back from either neighbour, and each neighbour by a hop from
Luxembourg. The regular expression lists the journeys themselves, any
number of round trips from Luxembourg (``*``, with ``|`` between the two
neighbours), then the hop to Port Royal:

.. code-block:: text

    (B: Luxembourg → Port Royal · B: Port Royal → Luxembourg
     | B: Luxembourg → Saint-Michel Notre-Dame
       · B: Saint-Michel Notre-Dame → Luxembourg)*
    · B: Luxembourg → Port Royal

Unlike the other semirings so far, the concatenation ``·`` of
:sqlfunc:`sr_regex` does not commute, and follows the order of the query:
``reach`` comes first in the recursive ``FROM`` clause, so that each
journey reads in the order it is made.


Step 12: The Fastest Journey Itself
-----------------------------------

:sqlfunc:`sr_tropical` gives the shortest travel time, not the journey
that takes it. :sqlfunc:`sr_tropical_paths` gives both: the least cost,
from a first mapping, and the journeys of that cost, as a regular
expression over the labels of a second one, alternatives of the same cost
joined by ``|``. With the labels of Step 11, look for La Défense, on
metro and RER only:

.. code-block:: postgresql

    WITH RECURSIVE reach(station) AS (
        SELECT 'IDFM:71161'::text
      UNION
        SELECT h.to_station FROM reach r JOIN hop h ON h.from_station = r.station
        WHERE h.route_type IN (1, 2))
    SELECT (sr_tropical_paths(provenance(), 'hop_minutes', 'hop_name')).*
    FROM reach WHERE station = 'IDFM:71517';

The fastest journey takes RER B to Châtelet - Les Halles, and changes
there to RER A:

.. code-block:: text

    B: Luxembourg → Saint-Michel Notre-Dame
    · B: Saint-Michel Notre-Dame → Châtelet - Les Halles
    · A: Châtelet - Les Halles → Auber · A: Auber → Charles de Gaulle - Étoile
    · A: Charles de Gaulle - Étoile → La Défense


Step 13: One Journey, Step by Step
----------------------------------

Two journeys may take the same time. From Luxembourg to La Motte-Picquet -
Grenelle, the fastest journeys part at Gare Montparnasse: metro 6 over
four stops, or metro 13 then 10 over three, take the same time, and
:sqlfunc:`sr_tropical_paths` lists both:

.. code-block:: postgresql

    WITH RECURSIVE reach(station) AS (
        SELECT 'IDFM:71161'::text
      UNION
        SELECT h.to_station FROM reach r JOIN hop h ON h.from_station = r.station
        WHERE h.route_type IN (1, 2))
    SELECT (sr_tropical_paths(provenance(), 'hop_minutes', 'hop_name')).paths
    FROM reach WHERE station = 'IDFM:71199';

.. code-block:: text

    B: Luxembourg → Port Royal · B: Port Royal → Denfert-Rochereau
    · 6: Denfert-Rochereau → Raspail · 4: Raspail → Vavin
    · 4: Vavin → Gare Montparnasse
    · (6: Gare Montparnasse → Pasteur · 6: Pasteur → Sèvres-Lecourbe
       · 6: Sèvres-Lecourbe → Cambronne
       · 6: Cambronne → La Motte-Picquet - Grenelle
     | 13: Gare Montparnasse → Duroc · 10: Duroc → Ségur
       · 10: Ségur → La Motte-Picquet - Grenelle)

:sqlfunc:`sr_best_path` takes the same two mappings and keeps one journey
of least time, the one with the fewest hops (then the first in
alphabetical order of its hops), here through Duroc. It returns its hops
as an array, in the order they are made, which ``unnest`` turns into an
itinerary. To visit Provins, a medieval town in the east of the region:

.. code-block:: postgresql

    WITH RECURSIVE reach(station) AS (
        SELECT 'IDFM:71161'::text
      UNION
        SELECT h.to_station FROM reach r JOIN hop h ON h.from_station = r.station
        WHERE h.route_type IN (1, 2))
    SELECT round(j.cost::numeric, 1) AS minutes, step, hop
    FROM (SELECT (sr_best_path(provenance(), 'hop_minutes', 'hop_name')).*
          FROM reach WHERE station = 'IDFM:62139') j,
         unnest(j.path) WITH ORDINALITY AS u(hop, step)
    ORDER BY step;

.. code-block:: text

     minutes | step |                         hop
    ---------+------+------------------------------------------------------
        79.3 |    1 | B: Luxembourg → Saint-Michel Notre-Dame
        79.3 |    2 | B: Saint-Michel Notre-Dame → Châtelet - Les Halles
        79.3 |    3 | B: Châtelet - Les Halles → Gare du Nord
        79.3 |    4 | 4: Gare du Nord → Gare de l'Est
        79.3 |    5 | TER: Gare de l'Est → Longueville
        79.3 |    6 | P: Longueville → Sainte-Colombe Septveilles
        79.3 |    7 | P: Sainte-Colombe Septveilles → Champbenoist Poigny
        79.3 |    8 | P: Champbenoist Poigny → Provins

Over the whole network, buses included, journeys of the same time
are the rule rather than the exception, and the regular expression of
:sqlfunc:`sr_tropical_paths` grows long, where :sqlfunc:`sr_best_path`
still returns one journey.


Step 14: Metro or Train?
------------------------

From Gare de Lyon, which of the other railway terminals of Paris are
faster to reach by metro than by train (the RER and the suburban trains,
``route_type`` 2)? Each kind of journey is a recursive query, and
``EXCEPT`` takes the train journeys away from the metro ones:

.. code-block:: postgresql

    WITH RECURSIVE
      metro(station) AS (
          SELECT 'IDFM:73626'::text
        UNION
          SELECT h.to_station FROM metro m JOIN hop h ON h.from_station = m.station
          WHERE h.route_type = 1),
      train(station) AS (
          SELECT 'IDFM:73626'::text
        UNION
          SELECT h.to_station FROM train t JOIN hop h ON h.from_station = t.station
          WHERE h.route_type = 2)
    SELECT d.name, round(j.cost::numeric, 1) AS minutes, cardinality(j.path) AS hops
    FROM (SELECT station,
                 (sr_best_path(provenance(), 'hop_minutes', 'hop_name')).*
          FROM (SELECT station FROM metro EXCEPT SELECT station FROM train) t) j
    JOIN (VALUES ('IDFM:71135', 'Gare d''Austerlitz'),
                 ('IDFM:71359', 'Gare de l''Est'),
                 ('IDFM:71410', 'Gare du Nord'),
                 ('IDFM:71139', 'Gare Montparnasse'),
                 ('IDFM:71370', 'Gare Saint-Lazare'))
           AS d(station, name)
      USING (station)
    ORDER BY minutes;

.. code-block:: text

           name        | minutes  | hops
    -------------------+----------+------
     Gare d'Austerlitz |      5.9 |    3
     Gare Saint-Lazare |      8.4 |    4
     Gare de l'Est     |     10.3 |    7
     Gare Montparnasse |     12.0 |    8
     Gare du Nord      | Infinity |

All five are reached both ways, so that plain SQL would return none of
them. In :sqlfunc:`sr_best_path`, the difference of two journeys is the
first one where it is better than the second (faster or, as fast, with
fewer hops, then first in alphabetical order of its hops), and nothing
otherwise: the metro wins to four of the terminals, with the journey that
does, while the train is faster to Gare du Nord (RER A then B, through
Châtelet - Les Halles), which has no journey left.


Step 15: A Day of Rail Disruptions
----------------------------------

Suppose each rail hop runs with probability 0.95, and each bus hop with
probability 0.9:

.. code-block:: postgresql

    SELECT set_prob(provenance(),
                    CASE WHEN route_type = 3 THEN 0.9 ELSE 0.95 END)
    FROM hop;

(The query returns one empty row per hop.)

The probability of still reaching a station is a network-reliability
problem, hard in general. An exact answer would expand the equations into
a Boolean circuit of billions of gates, and is refused:

.. code-block:: postgresql

    WITH RECURSIVE reach(station) AS (
        SELECT 'IDFM:71161'::text
      UNION
        SELECT h.to_station FROM reach r JOIN hop h ON h.from_station = r.station)
    SELECT probability_evaluate(provenance())
    FROM reach WHERE station = 'IDFM:73699';

An approximation with an *additive* guarantee (see
:ref:`probability-guarantees`) samples instead, solving the equations in
each sampled world. With the buses, every destination is reached in
practically every world; on the rail network alone, the ends of the
branches show:

.. code-block:: postgresql

    WITH RECURSIVE reach(station) AS (
        SELECT 'IDFM:71161'::text
      UNION
        SELECT h.to_station FROM reach r JOIN hop h ON h.from_station = r.station
        WHERE h.route_type <> 3)
    SELECT d.name,
           round(probability_evaluate(provenance(), 'additive',
                                      'eps=0.01,delta=0.05')::numeric, 3) AS p
    FROM reach r
    JOIN (VALUES ('IDFM:474151', 'Châtelet - Les Halles'),
                 ('IDFM:71517', 'La Défense'),
                 ('IDFM:73721', 'Versailles Château Rive Gauche'),
                 ('IDFM:73699', 'Aéroport CDG (Terminal 2)'),
                 ('IDFM:68385', 'Marne-la-Vallée - Chessy'))
           AS d(station, name)
      USING (station)
    ORDER BY p DESC;

The centre of the network, with many alternatives, stays almost certain to
be reached; Chessy, at the end of a branch of RER A, depends on every hop
of that branch, and is markedly less likely. Each estimate is within 0.01
of the true probability with probability 95%.

These estimates assume that every hop runs or not independently of all
the others. In practice, the hops of the same line are correlated: a
signal failure or a strike stops a whole stretch of a line at once, and
an express train and the stopping service between the same stations, two
alternatives here, share the same track. Such correlations would lower
these probabilities a lot.
