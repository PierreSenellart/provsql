Case Study: Île-de-France Public Transit
==========================================

This case study, extending the scenario introduced in
:cite:`DBLP:journals/pvldb/SenellartJMR18`, applies ProvSQL to the
real-world GTFS dataset for Île-de-France public transit. It demonstrates
Boolean provenance at scale for wheelchair accessibility reasoning, then
journeys with changes as a ``WITH RECURSIVE`` query over the whole network,
whose rows the same query evaluates in several semirings: shortest travel
time, accessibility, the equations of the recursion, and the probability of
reaching a station when parts of the network are disrupted.

.. note::

   Unlike the other case studies, this one is **not available in the ProvSQL
   Playground**: it loads the large Île-de-France GTFS dataset, which is
   not bundled and must be fetched separately (see the :ref:`Playground note
   <playground-note>`). Run it against a local ProvSQL installation.

The Scenario
------------

The `STIF GTFS dataset <https://www.data.gouv.fr/datasets/horaires-prevus-sur-les-lignes-de-transport-en-commun-dile-de-france-gtfs-datahub/>`_
describes hundreds of transit routes and tens of thousands of stops.
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

This creates four tables:

* ``routes`` -- transit lines (RER A, B, M1, bus 91…)
* ``stops`` -- individual stop points with GPS coordinates and a
  ``wheelchair_boarding`` flag
* ``trips`` -- individual scheduled journeys, each with a
  ``wheelchair_accessible`` flag
* ``stop_times`` -- arrival and departure times at each stop for each trip

The script also adds provenance tracking and creates a combined
``wheelchair`` mapping table from both the trip and stop wheelchair columns.

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

To find the stop IDs for Luxembourg station and its platforms:

.. code-block:: postgresql

    SELECT * FROM stops WHERE stop_name = 'Luxembourg';

Two stations carry the name: ``IDFM:71161``, the RER B station in Paris,
and ``IDFM:65716``, a bus stop elsewhere in the region. The queries below
start from the first.


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


Step 3: Reachable Stops from Luxembourg
-------------------------------------

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

This returns some 430 distinct (stop, route) pairs, on RER B and the bus
lines through the station (the exact number depends on the GTFS dataset
version).


Step 4: Boolean Provenance -- Full Wheelchair Accessibility
-----------------------------------------------------------

Add Boolean provenance evaluation to mark which results are fully
wheelchair-accessible along *every* leg.  Because the query returns one
row per trip (each with its own provenance circuit), materialise the
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
      WHERE s0.stop_id = 'IDFM:71161';

    SELECT remove_provenance('bagneux_b');

    SELECT stop_name, route_long_name, bool_or(accessible) AS accessible
    FROM bagneux_b
    GROUP BY stop_name, route_long_name
    ORDER BY route_long_name, stop_name;

The materialised table still carries the ``provsql`` provenance column,
so :sqlfunc:`remove_provenance` drops tracking first and the
``bool_or`` aggregation runs outside ProvSQL.

:sqlfunc:`sr_boolean` evaluates the provenance token under the Boolean
semiring, looking up each leaf token in the ``wheelchair`` table.
A result of ``true`` means every record along *some* trip from Luxembourg
to that stop has the wheelchair flag set; ``false`` means no fully
accessible trip exists. The table holds some 200,000 rows, one per trip and
stop, so this step takes a few seconds; every stop of RER B comes out
accessible, while a few dozen bus stops do not.


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
earlier stop being present. Here the tokens stand for wheelchair
accessibility and every stop exists, so which stop comes next is a plain
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
accessibly. Computing the table needs no provenance, so turn the rewriting
off with ``provsql.active`` while it is built; the table then gets tokens
of its own, as a new input to the queries below:

.. code-block:: postgresql

    DROP TABLE IF EXISTS hop_minutes, hop_access, hop_name, hop_one, hop;
    SET provsql.active = off;
    CREATE TABLE hop AS
    WITH seq AS (
      SELECT r.route_short_name AS line, r.route_type,
             coalesce(nullif(s.parent_station, ''), s.stop_id) AS station,
             lead(coalesce(nullif(s.parent_station, ''), s.stop_id))
               OVER w AS next_station,
             split_part(st.departure_time, ':', 1)::int * 60
               + split_part(st.departure_time, ':', 2)::int AS departure,
             lead(split_part(st.arrival_time, ':', 1)::int * 60
                  + split_part(st.arrival_time, ':', 2)::int) OVER w AS arrival,
             t.wheelchair_accessible = 1 AND s.wheelchair_boarding = 1
               AND lead(s.wheelchair_boarding) OVER w = 1 AS accessible
      FROM stop_times st
      JOIN trips t  ON t.trip_id = st.trip_id
      JOIN routes r ON r.route_id = t.route_id
      JOIN stops s  ON s.stop_id = st.stop_id
      WINDOW w AS (PARTITION BY st.trip_id ORDER BY st.stop_sequence)
    )
    SELECT station AS from_station, next_station AS to_station, line,
           route_type,
           round(avg(arrival - departure), 1)::float8 AS minutes,
           bool_or(accessible)::int AS accessible
    FROM seq
    WHERE next_station IS NOT NULL AND next_station <> station
    GROUP BY station, next_station, line, route_type
    HAVING avg(arrival - departure) IS NOT NULL;
    RESET provsql.active;

    SELECT add_provenance('hop');
    SELECT create_provenance_mapping('hop_minutes', 'hop', 'minutes');
    SELECT create_provenance_mapping('hop_access', 'hop', 'accessible');

The table has some 75,000 rows and takes under a minute to build, over the
13 million rows of ``stop_times``. The schedule gives times to the minute,
so a single trip often shows a hop as 0 or 1 minute; the average over the
trips evens this rounding out. Hops with no scheduled time are left out.


Step 8: Every Station Reachable from Luxembourg
-----------------------------------------------

A journey with changes is a path in this network: a recursive query
follows the hops from Luxembourg, a line change being just another hop out
of the same station:

.. code-block:: postgresql

    WITH RECURSIVE reach(station) AS (
        SELECT 'IDFM:71161'::text
      UNION
        SELECT h.to_station FROM hop h JOIN reach r ON h.from_station = r.station)
    SELECT count(*) FROM reach;

Some 15,400 stations, the whole network, in a few seconds. The network is
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
        SELECT h.to_station FROM hop h JOIN reach r ON h.from_station = r.station)
    SELECT d.name,
           sr_tropical(provenance(), 'hop_minutes', nonnegative => true)
             AS minutes,
           sr_boolean(provenance(), 'hop_access') AS accessible
    FROM reach r
    JOIN (VALUES ('IDFM:71264', 'Châtelet'), ('IDFM:71410', 'Gare du Nord'),
                 ('IDFM:71517', 'La Défense'),
                 ('IDFM:73721', 'Versailles Château Rive Gauche'),
                 ('IDFM:73699', 'Aéroport CDG (Terminal 2)'),
                 ('IDFM:68385', 'Marne-la-Vallée - Chessy'))
           AS d(station, name)
      USING (station)
    ORDER BY minutes;

.. code-block:: text

                 name              | minutes | accessible
    -------------------------------+---------+------------
    Châtelet                       |     3.5 | t
    Gare du Nord                   |     6.1 | t
    La Défense                     |    11.1 | t
    Versailles Château Rive Gauche |    17.7 | t
    Aéroport CDG (Terminal 2)      |    28.9 | t
    Marne-la-Vallée - Chessy       |    35.5 | t

The times are time on board, without waiting or changing: Versailles
comes in under 18 minutes through a direct train from Gare Montparnasse.

Each semiring solves the equations by the method its properties allow.
The Boolean semiring, and the tropical one over the nonnegative costs that
``nonnegative => true`` declares, are *absorptive* (a cycle never improves
a journey) and *selective* (an alternative is one of the two), which
licenses Dijkstra's algorithm; without ``nonnegative``, a cost might be
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
        SELECT h.to_station FROM hop h JOIN reach r ON h.from_station = r.station)
    SELECT sr_counting(provenance(), 'hop_one')
    FROM reach WHERE station = 'IDFM:71264';

is refused: going round a cycle any number of times gives infinitely many
derivations, which no integer counts.


Step 11: The Equations of a Recursion
-------------------------------------

:sqlfunc:`sr_formula` shows the equations themselves. On the whole network
they run to tens of kilobytes per station; restricted to RER B between
Luxembourg and its two neighbours, with each hop labelled by its stations,
they read. ``plain(NULL::stops)`` reads the station names without their
provenance, so that the stops' own tokens stay out of the labels and of the
result:

.. code-block:: postgresql

    CREATE TABLE hop_name AS
      SELECT h.provsql AS provenance, f.stop_name || ' → ' || t.stop_name AS value
      FROM hop h JOIN plain(NULL::stops) f ON f.stop_id = h.from_station
                 JOIN plain(NULL::stops) t ON t.stop_id = h.to_station
      WHERE h.line = 'B';

    WITH RECURSIVE reach(station) AS (
        SELECT 'IDFM:71161'::text
      UNION
        SELECT h.to_station FROM hop h JOIN reach r ON h.from_station = r.station
        WHERE h.line = 'B'
          AND h.to_station IN ('IDFM:71161', 'IDFM:71106', 'IDFM:73620'))
    SELECT s.stop_name, sr_formula(provenance(), 'hop_name')
    FROM reach r JOIN plain(NULL::stops) s ON s.stop_id = r.station;

For Port Royal:

.. code-block:: text

    x₁ where x₁ = Luxembourg → Port Royal ⊗ x₂,
             x₂ = 𝟙 ⊕ (Port Royal → Luxembourg ⊗ x₁)
                    ⊕ (Saint-Michel Notre-Dame → Luxembourg ⊗ x₃),
             x₃ = Luxembourg → Saint-Michel Notre-Dame ⊗ x₂

One unknown per station: Luxembourg (``x₂``) holds from the start (``𝟙``)
or by coming back from either neighbour, and each neighbour by a hop from
Luxembourg.


Step 12: A Day of Rail Disruptions
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
a Boolean circuit of over two billion gates, and is refused:

.. code-block:: postgresql

    WITH RECURSIVE reach(station) AS (
        SELECT 'IDFM:71161'::text
      UNION
        SELECT h.to_station FROM hop h JOIN reach r ON h.from_station = r.station)
    SELECT probability_evaluate(provenance())
    FROM reach WHERE station = 'IDFM:73699';

An approximation with an *additive* guarantee (see
:ref:`probability-guarantees`) samples instead, solving the equations in
each sampled world. With the buses, every destination is reached in
practically every world; on the rail network alone, the ends of the
branches show (the seed makes the sampling reproducible):

.. code-block:: postgresql

    SET provsql.monte_carlo_seed = 1;
    WITH RECURSIVE reach(station) AS (
        SELECT 'IDFM:71161'::text
      UNION
        SELECT h.to_station FROM hop h JOIN reach r ON h.from_station = r.station
        WHERE h.route_type <> 3)
    SELECT d.name,
           round(probability_evaluate(provenance(), 'additive',
                                      'eps=0.01,delta=0.05')::numeric, 3) AS p
    FROM reach r
    JOIN (VALUES ('IDFM:71264', 'Châtelet'), ('IDFM:71517', 'La Défense'),
                 ('IDFM:73721', 'Versailles Château Rive Gauche'),
                 ('IDFM:73699', 'Aéroport CDG (Terminal 2)'),
                 ('IDFM:68385', 'Marne-la-Vallée - Chessy'))
           AS d(station, name)
      USING (station)
    ORDER BY p DESC;

.. code-block:: text

                 name              |   p
    -------------------------------+-------
    La Défense                     | 0.996
    Châtelet                       | 0.995
    Aéroport CDG (Terminal 2)      | 0.988
    Versailles Château Rive Gauche | 0.897
    Marne-la-Vallée - Chessy       | 0.843

The centre of the network has many alternatives; Chessy, at the end of a
branch of RER A, depends on every hop of that branch. Each estimate is
within 0.01 of the true probability with probability 95%.
