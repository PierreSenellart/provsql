Case Study: The Wildlife Photo Archive
========================================

This case study, loosely inspired by a ProvSQL demonstration at EDBT 2025
:cite:`DBLP:conf/edbt/YunusKSAB25`, applies ProvSQL to a database of wildlife
photographs annotated by a species-detection model. It demonstrates the
``VALUES`` clause, :sqlfunc:`repair_key` and the ``mulinput`` gate, ranking
by probability versus thresholding, ``EXCEPT``, common table expressions, and
:sqlfunc:`expected` aggregates.

The Scenario
------------

A naturalist organisation maintains a database of wildlife photographs taken
at four field stations in the Scottish Highlands. Each photo has been
processed by a species-detection model that draws one or more *bounding
boxes* around things it thinks are animals, and for each box reports a
list of candidate species with a confidence score. A box can therefore
appear with several species candidates (e.g. a partly-occluded shape
might score 0.40 as red deer and 0.30 as roe deer); a photo can contain
several boxes of the same species (e.g. three deer in a meadow shot).

Your tasks:

* find photos that contain specific combinations of species,
* rank results by the probability that the combination is truly present,
* compare probabilistic ranking against naive confidence thresholding,
* exclude photos that contain unwanted species,
* compute expected species counts per photo.

Setup
-----

This case study assumes a working ProvSQL installation (see
:doc:`getting-provsql`). Download :download:`setup.sql <../../casestudy5/setup.sql>`
and load it into a fresh PostgreSQL database::

    psql -d mydb -f setup.sql

This creates three tables:

* ``photo`` – 30 wildlife photographs, each tagged with a station name
  (Loch Torridon, Glen Affric, Rannoch Moor, or Cairngorms) and a date
* ``species`` – 13 species across mammals, birds, and reptiles
* ``detection`` – about 60 model-produced (bounding-box, species) candidate
  rows, each linking a photo and a bounding-box index to a candidate
  species with a confidence score; multiple rows for the same
  (``photo_id``, ``bbox_id``) pair represent the classifier's
  alternative species hypotheses for that single bounding box


Step 1: Explore the Database
-----------------------------

At the start of every session, set the search path:

.. code-block:: postgresql

    SET search_path TO public, provsql;

Inspect the tables. Note that ``detection`` is *not* keyed on
(``photo_id``, ``bbox_id``): a single bounding box can appear in
several rows, one per candidate species the classifier considered.

.. code-block:: postgresql

    SELECT * FROM photo  ORDER BY id LIMIT 5;
    SELECT * FROM species ORDER BY id;
    SELECT photo_id, bbox_id, species_id, confidence
    FROM detection
    WHERE photo_id IN (5, 9, 14, 22)
    ORDER BY photo_id, bbox_id, confidence DESC;


Step 2: Enable Provenance and Create a Name Mapping
----------------------------------------------------

Enable provenance tracking on ``detection``. Each row receives a UUID
circuit token that propagates through any downstream query.

.. code-block:: postgresql

    SELECT add_provenance('detection');

To get readable formulas, we want to associate each detection's
provenance token with its species name. A *provenance mapping* in
ProvSQL is nothing more than a regular table with two columns named
``value`` and ``provenance`` (plus, for performance, an index on
``provenance``). The convenience function :sqlfunc:`create_provenance_mapping`
builds such a table from one column of a provenance-enabled relation,
but nothing prevents us from constructing the table by hand:

.. code-block:: postgresql

    CREATE TABLE species_mapping AS
      SELECT s.name AS value, d.provsql AS provenance
      FROM detection d JOIN species s ON s.id = d.species_id;

    SELECT remove_provenance('species_mapping');
    CREATE INDEX ON species_mapping(provenance);

The ``CREATE TABLE AS`` query inherits a ``provsql`` column from
``detection`` via ProvSQL's planner hook;
:sqlfunc:`remove_provenance` strips that extra column so only the
``(value, provenance)`` pair remains. Because the schema is fully
under our control, we can populate the table from any expression –
combine columns, filter rows, derive computed values – and any
semiring-evaluation function (``sr_formula``, ``sr_why``, …) will
happily consume the result.


Step 3: Inline Lookup with ``VALUES``
---------------------------------------

A ``VALUES`` clause defines an inline relation directly inside a query.
ProvSQL's planner hook treats it like any other source: rows it produces
have no provenance of their own, but they carry through joins so that the
result inherits provenance from the joined provenance-enabled rows.

Here we use ``VALUES`` to define a small ad-hoc watchlist: a couple of
species we want to look up by hand together with a human-readable
label, instead of pulling them from the ``species`` table. Suppose we
are interested in Red Deer (``species_id`` 1, the dominant grazer
whose density we want to track) and Red Fox (``species_id`` 3, a
generalist predator), and we want to tag the rows accordingly:

.. code-block:: postgresql

    SELECT v.label, p.id, p.station, p.date,
           sr_formula(provenance(), 'species_mapping') AS formula
    FROM (VALUES (1, 'mammal of interest'),
                 (3, 'predator of interest')) AS v(species_id, label),
         detection d, photo p
    WHERE d.species_id = v.species_id AND d.photo_id = p.id
    ORDER BY p.id, v.label;

Each output row carries the provenance of the underlying ``detection``
row alone: the formula is a single species token, even though the row
also references ``photo`` and ``VALUES``. Tables without provenance –
including the ``VALUES`` rows – contribute no tokens.


Step 4: Conjunctive Query (Naive)
-----------------------------------

Find photos that contain both Red Deer (``species_id`` 1) and Red Fox
(``species_id`` 3) using a self-join on ``detection``:

.. code-block:: postgresql

    SELECT p.id, p.station, p.date,
           sr_formula(provenance(), 'species_mapping') AS formula
    FROM detection d1
    JOIN detection d2 ON d1.photo_id = d2.photo_id
    JOIN photo p ON p.id = d1.photo_id
    WHERE d1.species_id = 1 AND d2.species_id = 3
    GROUP BY p.id, p.station, p.date
    ORDER BY p.id;

Look at photo 5: the classifier produced three Red Deer candidate rows
(in three different bounding boxes) and two Red Fox candidate rows (in
two more boxes). Its formula is the ⊕-sum of all six (deer, fox) pair
products – every candidate row is an independent input gate. This
matches the structure of the underlying ``detection`` table but
mis-models the data: each bounding box can correspond to *at most one
real animal*, so candidate rows that share a ``(photo_id, bbox_id)``
pair should be mutually exclusive rather than independent. Nothing in
the schema enforces that today, and the formula reflects the
mismatch.


Step 5: Mutually Exclusive Candidates with ``repair_key``
------------------------------------------------------------

:sqlfunc:`repair_key` rewrites the provenance so that rows sharing a key
become alternatives under a single ``mulinput`` (multivalued input) gate
– i.e. *exactly one* of them is true. Applied with the key
``(photo_id, bbox_id)``, every bounding box becomes one mulinput
variable whose values are the candidate species the classifier
considered for that box.

:sqlfunc:`repair_key` takes a single key attribute, so add a synthetic
key combining photo and bounding-box index first. ``repair_key``
reinstalls the ``provsql`` column itself, so also drop the old mapping
(whose tokens are about to become stale):

.. code-block:: postgresql

    DROP TABLE species_mapping;
    SELECT remove_provenance('detection');

    ALTER TABLE detection ADD COLUMN photo_bbox text;
    UPDATE detection SET photo_bbox = photo_id || '/' || bbox_id;

    SELECT repair_key('detection', 'photo_bbox');

    CREATE TABLE species_mapping AS
      SELECT s.name AS value, d.provsql AS provenance
      FROM detection d JOIN species s ON s.id = d.species_id;
    SELECT remove_provenance('species_mapping');
    CREATE INDEX ON species_mapping(provenance);

Re-running the conjunctive query from Step 4 with :sqlfunc:`sr_formula`
would not be illuminating: mutually exclusive events have no meaningful
representation in the symbolic-formula semiring (each ``mulinput`` just
collapses to ``𝟙``). To visualize them we use :sqlfunc:`sr_boolexpr`
instead, which renders the underlying Boolean formula with internal
variable names and exposes each ``mulinput`` explicitly:

.. code-block:: postgresql

    SELECT p.id, sr_boolexpr(provenance()) AS bexpr
    FROM detection d1
    JOIN detection d2 ON d1.photo_id = d2.photo_id
    JOIN photo p ON p.id = d1.photo_id
    WHERE d1.species_id = 1 AND d2.species_id = 3 AND p.id IN (2, 5)
    GROUP BY p.id
    ORDER BY p.id;

Each input now appears as a ``mulinput`` (the ``{i=v}[p]`` notation
denotes "variable ``i`` takes value ``v`` with probability ``p``"). In
this query every variable happens to have a single value, so the
mutually-exclusive structure is not visible yet. We will see a genuine
multi-valued ``mulinput`` in the next step, where the deer/roe-deer
candidates of one bounding box compose under the mutex constraint.

.. note::

   :sqlfunc:`view_circuit` cannot render ``mulinput`` gates either and
   refuses to evaluate. Use :sqlfunc:`sr_boolexpr` to inspect circuits
   that contain ``mulinput`` gates, and :sqlfunc:`probability_evaluate`
   to score them.


Step 6: Assign Probabilities and Verify Mutual Exclusion
----------------------------------------------------------

Each candidate row's confidence becomes the probability that that
classifier candidate is the true species for its bounding box:

.. code-block:: postgresql

    DO $$ BEGIN
      PERFORM set_prob(provenance(), confidence) FROM detection;
    END $$;

To see that ``repair_key`` made a numerical difference, ask: *what is
the probability that bounding box 1 of photo 5 corresponds to a
deer-like animal* (``species_id`` 1 = Red Deer or 2 = Roe Deer)? In the
data, that bounding box has both candidate species recorded with
confidences 0.40 and 0.30:

.. code-block:: postgresql

    SELECT photo_id, bbox_id,
        sr_boolexpr(provenance()) AS bexpr,
        ROUND(probability_evaluate(provenance(), 'tree-decomposition')::numeric, 4) AS p
    FROM detection
    WHERE photo_id = 5 AND bbox_id = 1 AND species_id IN (1, 2)
    GROUP BY photo_id, bbox_id;

The Boolean expression is now ``({0=1}[0.400000] ∨ {0=2}[0.300000])``:
a single mulinput variable (variable 0) with two mutually exclusive
values, value 1 (Red Deer) with probability 0.40 and value 2
(Roe Deer) with probability 0.30. Probability evaluation gives
``0.7000``, the sum of the two confidences, since combining mutually
exclusive events with ⊕ is just addition. Had we kept the original
:sqlfunc:`add_provenance` setup with each row as an independent input
gate, the same query would have given
``1 - (1 - 0.40) × (1 - 0.30) = 0.58`` instead. The 0.12 gap is the
practical effect of telling the engine "these candidates cannot both
be true at once".


Step 7: Probabilistic Ranking vs. Threshold Filtering
-------------------------------------------------------

Run the conjunctive query under two ranking strategies. First, by
probability that *both* species are truly present:

.. code-block:: postgresql

    SELECT p.id, p.station, p.date,
           ROUND(probability_evaluate(provenance())::numeric, 4) AS prob
    FROM detection d1
    JOIN detection d2 ON d1.photo_id = d2.photo_id
    JOIN photo p ON p.id = d1.photo_id
    WHERE d1.species_id = 1 AND d2.species_id = 3
    GROUP BY p.id, p.station, p.date
    ORDER BY prob DESC, p.id;

Second, by raw confidence threshold (every contributing detection must score
at least 0.5):

.. code-block:: postgresql

    SELECT DISTINCT p.id, p.station, p.date
    FROM detection d1
    JOIN detection d2 ON d1.photo_id = d2.photo_id
    JOIN photo p ON p.id = d1.photo_id
    WHERE d1.species_id = 1 AND d2.species_id = 3
      AND d1.confidence >= 0.5 AND d2.confidence >= 0.5
    ORDER BY p.id;

Photo 5 is missed by thresholding (every individual candidate there
scores below 0.5) but ranks reasonably under
:sqlfunc:`probability_evaluate`, because the weak deer candidates in
several boxes plus the weak fox candidates combine into a non-trivial
probability that *some* deer and *some* fox are truly there.
Conversely, a photo whose top candidates only barely cross 0.5 passes
the threshold but ends up low in the probability ranking.


Step 8: Absence Constraint with ``EXCEPT``
--------------------------------------------

Find photos that contain a Red Deer but no Domestic Dog (``species_id``
13). ``EXCEPT`` is implemented in ProvSQL via the ⊖ (monus) operator on
the provenance circuit:

.. code-block:: postgresql

    SELECT p.id, p.station, p.date,
           ROUND(probability_evaluate(provenance())::numeric, 4) AS prob
    FROM (
        SELECT photo_id FROM detection WHERE species_id = 1
      EXCEPT
        SELECT photo_id FROM detection WHERE species_id = 13
    ) t
    JOIN photo p ON p.id = t.photo_id
    GROUP BY p.id, p.station, p.date
    ORDER BY prob DESC, p.id;

Photos that contain a dog still appear in the output – ``EXCEPT`` is
*not* a hard filter. Photo 9, with a high-confidence dog detection,
ranks lower because the monus discounts strongly. Photo 14, where the
dog detection has very low confidence, ranks higher: it is *probably*
in the result, but not certainly. ProvSQL preserves both possibilities
in the circuit and lets :sqlfunc:`probability_evaluate` weigh them.


Step 9: Multi-Condition Query via a CTE
-----------------------------------------

Combine Steps 7 and 8: photos with both Red Deer and Red Fox, with no
Domestic Dog, ranked by probability. The query has three logical layers
(co-occurrence, absence, ranking) and reads naturally as a CTE:

.. code-block:: postgresql

    WITH deer_and_fox AS (
      SELECT d1.photo_id
      FROM detection d1
      JOIN detection d2 ON d1.photo_id = d2.photo_id
      WHERE d1.species_id = 1 AND d2.species_id = 3
      GROUP BY d1.photo_id
    ),
    no_dogs AS (
      SELECT photo_id FROM deer_and_fox
      EXCEPT
      SELECT photo_id FROM detection WHERE species_id = 13
    )
    SELECT p.id, p.station, p.date,
           ROUND(probability_evaluate(provenance())::numeric, 4) AS prob
    FROM no_dogs t
    JOIN photo p ON p.id = t.photo_id
    ORDER BY prob DESC, p.id;

ProvSQL's planner hook fires on the expanded query: CTEs are inlined and
provenance propagates through them transparently. The same answer can be
written with nested subqueries; the CTE form is purely a readability
choice.


Step 10: Expected Species Counts with :sqlfunc:`expected`
-----------------------------------------------------------

How many distinct detections do we *expect* to be true positives in each
photo?

.. code-block:: postgresql

    SELECT p.id, p.station,
           ROUND(expected(COUNT(*))::numeric, 4) AS exp_detections
    FROM detection d
    JOIN photo p ON p.id = d.photo_id
    GROUP BY p.id, p.station
    ORDER BY exp_detections DESC, p.id;

By linearity of expectation, ``expected(COUNT(*))`` over a group is
:math:`\sum_i P(\text{detection}_i \text{ is true})`. The same linearity
applies to ``SUM`` aggregates: the expected total confidence mass per
photo:

.. code-block:: postgresql

    SELECT p.id, p.station,
           ROUND(expected(SUM(d.confidence))::numeric, 4) AS exp_total_conf
    FROM detection d
    JOIN photo p ON p.id = d.photo_id
    GROUP BY p.id, p.station
    ORDER BY exp_total_conf DESC, p.id;

Both queries use ProvSQL's :sqlfunc:`expected` operator, which computes
the expected value of a SQL aggregate over the probabilistic database
defined by the per-row probabilities set in Step 6. Photos with many
high-confidence detections rank highest on both metrics.
