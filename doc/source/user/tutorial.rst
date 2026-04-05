Tutorial: Who Killed Daphine?
==============================

This tutorial introduces the core ProvSQL features through a self-contained
crime mystery.

The Scenario
------------

A group of 20 persons spent the night in a manor. In the morning, Daphine,
a young lady, went missing. Her dead body was found the following day in the
cellar. The autopsy revealed the following facts:

* She died of a head injury caused by a blunt-force instrument.
* Her death happened between midnight and 8 am.
* She was not killed in the cellar, but her body was moved there afterwards.
* Goose down found in her wound proves that she died in bed in one of the
  bedrooms; unfortunately all beds have the same down pillows, and
  pillowcases were changed in the morning, so it is impossible to identify
  which bedroom.

The police interviewed all 19 suspects and collected their statements about
who they saw in which room at which time. They also assessed each witness's
reliability through a psychological evaluation.

**Your mission:** help the police discover who killed Daphine, using the
power of provenance management and probabilistic databases.

Setup
-----

This tutorial assumes a working ProvSQL installation (see
:doc:`getting-provsql`). Download :download:`setup.sql <../../tutorial/setup.sql>`
and load it into a fresh PostgreSQL database::

    psql -d mydb -f setup.sql

This creates four tables:

* ``person`` – the 20 persons present at the manor
* ``room`` – the rooms of the manor
* ``sightings`` – witness statements: who was seen where, and when
* ``reliability`` – the reliability score (between 0 and 1) of each witness

Step 1: Explore the Database
-----------------------------

Familiarise yourself with the schema. In the ``psql`` client:

.. code-block:: text

    \d
    \d sightings

At the start of every new session, set the search path so that ProvSQL
functions can be called without the ``provsql.`` prefix:

.. code-block:: postgresql

    SET search_path TO public, provsql;

Step 2: Build a Sightings Table
--------------------------------

Design a query that retrieves, for every sighting: the time, the name of the
person seen, the name of the witness, and the name of the room. Store the
result in a new table ``s``.

.. raw:: html

   <details>
   <summary>Solution</summary>

.. code-block:: postgresql

    CREATE TABLE s AS
    SELECT
      time,
      person.name  AS person,
      p2.name      AS witness,
      room.name    AS room
    FROM sightings
      JOIN person       ON person  = person.id
      JOIN person AS p2 ON witness = p2.id
      JOIN room         ON room    = room.id;

.. raw:: html

   </details>

Step 3: Enable Provenance
--------------------------

Activate provenance tracking on the table ``s`` using
:sqlfunc:`add_provenance`:

.. code-block:: postgresql

    SELECT add_provenance('s');

A hidden ``provsql`` column is added that holds a UUID provenance token for
each tuple. Run a simple ``SELECT * FROM s`` – you will see this extra column
in the output.

.. note::

   The ``provsql`` column behaves specially: you cannot filter or sort on it
   directly. Use the :sqlfunc:`provenance()` function to obtain the current
   row's token in expressions.

Create a *provenance mapping* that associates each provenance token with the
name of the witness who made the sighting, using
:sqlfunc:`create_provenance_mapping`:

.. code-block:: postgresql

    SELECT create_provenance_mapping('witness_mapping', 's', 'witness');

The mapping is stored as an ordinary table – inspect it with
``SELECT * FROM witness_mapping;``.

Step 4: Find Contradictions
----------------------------

Some witnesses are unreliable: the same person may be reported in two
different rooms at the same time – an impossibility. Write a query that
identifies all such contradictions.

.. raw:: html

   <details>
   <summary>Solution</summary>

.. code-block:: postgresql

    SELECT s1.time, s1.person, s1.room
    FROM s AS s1, s AS s2
    WHERE s1.person = s2.person
      AND s1.time   = s2.time
      AND s1.room  <> s2.room;

.. raw:: html

   </details>

Step 5: Display Provenance Formulas
------------------------------------

Extend the previous query by adding
``sr_formula(provenance(), 'witness_mapping')`` to the ``SELECT`` clause to
see *which witnesses* are responsible for each contradiction.

:sqlfunc:`sr_formula` displays the provenance token as a formula,
substituting each leaf token with the mapped value from ``witness_mapping``.

.. raw:: html

   <details>
   <summary>Solution</summary>

.. code-block:: postgresql

    SELECT s1.time, s1.person, s1.room,
           sr_formula(provenance(), 'witness_mapping')
    FROM s AS s1, s AS s2
    WHERE s1.person = s2.person
      AND s1.time   = s2.time
      AND s1.room  <> s2.room;

.. raw:: html

   </details>

Step 6: Build a Consistent Sightings Table
-------------------------------------------

Create a table ``consistent_s`` containing all sightings *except* those
identified as contradictions. Display its content along with the provenance
formula for each tuple.

.. note::

   ProvSQL does not support ``NOT IN``; use ``EXCEPT`` to express negation.

.. raw:: html

   <details>
   <summary>Solution</summary>

.. code-block:: postgresql

    CREATE TABLE consistent_s AS
    SELECT time, person, room FROM s
    EXCEPT
    SELECT s1.time, s1.person, s1.room
    FROM s AS s1, s AS s2
    WHERE s1.person = s2.person
      AND s1.time   = s2.time
      AND s1.room  <> s2.room;

    SELECT *, sr_formula(provenance(), 'witness_mapping')
    FROM consistent_s;

.. raw:: html

   </details>

Step 7: Identify Suspects
--------------------------

The murder happened between midnight and 8 am in a bedroom. Create a
``suspects`` table containing every person who was seen (in a consistent
sighting) in a bedroom during that window. Display the suspects along with
their provenance formula.

.. raw:: html

   <details>
   <summary>Solution</summary>

.. code-block:: postgresql

    CREATE TABLE suspects AS
    SELECT DISTINCT person
    FROM consistent_s
    WHERE room LIKE '% bedroom'
      AND time BETWEEN '00:00:00' AND '08:00:00';

    SELECT *, sr_formula(provenance(), 'witness_mapping')
    FROM suspects;

.. raw:: html

   </details>

Step 8: Count Confirming Sightings
------------------------------------

Use the counting m-semiring to find how many sightings confirm that each
person is a suspect. Add an integer column ``count`` to ``s``, set it to
``1`` for all rows, and create a ``count_mapping``. Then use
:sqlfunc:`sr_counting` to display the count for each suspect.

.. raw:: html

   <details>
   <summary>Solution</summary>

.. code-block:: postgresql

    ALTER TABLE s ADD COLUMN count int;
    UPDATE s SET count = 1;

    SELECT create_provenance_mapping('count_mapping', 's', 'count');

    SELECT *, sr_counting(provenance(), 'count_mapping') AS c
    FROM suspects
    ORDER BY c;

.. raw:: html

   </details>

Step 9: Assign Reliability Probabilities
-----------------------------------------

Add a ``reliability`` float column to ``s`` and populate it with the
reliability score of each sighting's witness. Then assign these scores as
the probability of each provenance token using :sqlfunc:`set_prob`.

.. raw:: html

   <details>
   <summary>Solution</summary>

.. code-block:: postgresql

    ALTER TABLE s ADD COLUMN reliability float;

    UPDATE s
    SET reliability = score
    FROM reliability, person
    WHERE reliability.person = person.id
      AND person.name = s.witness;

    SELECT set_prob(provenance(), reliability) FROM s;

.. raw:: html

   </details>

Step 10: Find the Murderer
---------------------------

The police needs a confidence of at least 0.99 before making an arrest.
Use :sqlfunc:`probability_evaluate` to compute the probability that each
suspect was truly present, and identify those above the threshold.

:sqlfunc:`probability_evaluate` accepts an optional second argument for
the computation method:

* ``'possible-worlds'`` – exact, by exhaustive enumeration
* ``'monte-carlo'`` – approximate sampling (add a sample count as third argument)
* ``'tree-decomposition'`` – exact, via tree decomposition of the Boolean circuit
* ``'compilation'`` – d-DNNF compilation (add the tool name, e.g. ``'d4'``, as third argument)

.. raw:: html

   <details>
   <summary>Solution</summary>

.. code-block:: postgresql

    SELECT *,
           sr_formula(provenance(), 'witness_mapping'),
           probability_evaluate(provenance(), 'possible-worlds')
    FROM suspects
    WHERE probability_evaluate(provenance(), 'possible-worlds') > 0.99
      AND person <> 'Daphine';

.. raw:: html

   </details>
