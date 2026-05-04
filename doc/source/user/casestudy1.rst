Case Study: The Intelligence Agency
=====================================

This case study demonstrates ProvSQL's custom semiring capability,
where-provenance, probability computation with multiple algorithms, and
circuit export through a security-classification scenario.

The Scenario
------------

.. warning::

   The personnel data in this case study are entirely fictional and
   created solely to illustrate ProvSQL features.

An intelligence agency maintains a database of seven employees spread
across three cities. Every employee holds a security clearance ranging
from *unclassified* to *top secret*. Your tasks:

* identify which cities are served by more than one agent,
* determine the minimum clearance level needed to infer each result,
* find cities with exactly one agent (sensitive: if the city leaks,
  the sole agent is exposed),
* track where in the database each output value originated,
* compute the probability that a city remains a single-agent post
  after accounting for possible-world uncertainty.


Setup
-----

This case study assumes a working ProvSQL installation (see
:doc:`getting-provsql`). Download :download:`setup.sql <../../casestudy1/setup.sql>`
and load it into a fresh PostgreSQL database::

    psql -d mydb -f setup.sql

This creates:

* ``classification_level`` – an ordered ENUM
  (``unclassified`` < ``restricted`` < ``confidential`` < ``secret`` < ``top_secret``)
* ``personnel`` – 7 agents with name, position, city, and clearance level


Step 1: Explore the Database
-----------------------------

Inspect the ``personnel`` table::

    SELECT * FROM personnel ORDER BY id;

You should see seven rows: Juma (Director, Nairobi), Paul (Janitor, Nairobi),
David (Analyst, Paris), Ellen (Field agent, Beijing), Aaheli (Double agent,
Paris), Nancy (HR, Paris), and Jing (Analyst, Beijing).


Step 2: Enable Provenance and Create a Name Mapping
----------------------------------------------------

Enable provenance tracking on ``personnel`` and create a mapping so that
provenance tokens can be labelled with agent names::

    SELECT add_provenance('personnel');
    SELECT create_provenance_mapping('personnel_name', 'personnel', 'name');

After ``add_provenance``, every row of ``personnel`` has a unique UUID
token in its hidden ``provsql`` column. The mapping ``personnel_name``
associates each token with the corresponding agent's name.


Step 3: Cities Shared by Multiple Agents
-----------------------------------------

Which cities have at least two agents? Use a self-join with an ``id``
inequality to generate all unordered pairs, then apply
:sqlfunc:`sr_formula` to see which agents contribute to each city:

.. code-block:: postgresql

    SELECT DISTINCT p1.city,
           sr_formula(provenance(), 'personnel_name') AS formula
    FROM personnel p1
    JOIN personnel p2 ON p1.city = p2.city AND p1.id < p2.id
    GROUP BY p1.city
    ORDER BY p1.city;

The formula for Nairobi is ``Juma ⊗ Paul``: both agents must be present
for Nairobi to appear. Beijing similarly shows ``Ellen ⊗ Jing``. Paris,
with three agents, shows all three pairwise products joined by ``⊕``.


Step 4: Minimum Security Clearance (Custom Semiring)
-----------------------------------------------------

For each shared city, what is the *minimum clearance level* required to
have inferred that the city has multiple agents? An analyst who knows
the city only needs to see the lowest-cleared agent there.

This is a custom semiring over ``classification_level``:

* ``⊕`` (OR combination) = ``MAX``: the minimum clearance to know
  *either* agent was involved is the higher of the two (you need to be
  cleared to see both possibilities).
* ``⊗`` (AND/join) = ``MIN``: to know that *both* agents are present
  you only need clearance for the less-classified one (the higher-cleared
  agent already constrains you via the join).

.. code-block:: postgresql

    CREATE FUNCTION security_min_state(
        state classification_level,
        level classification_level)
      RETURNS classification_level AS $$
        SELECT CASE
          WHEN state IS NULL THEN level
          WHEN state < level THEN state
          ELSE level END
    $$ LANGUAGE SQL IMMUTABLE;

    CREATE FUNCTION security_max_state(
        state classification_level,
        level classification_level)
      RETURNS classification_level AS $$
        SELECT CASE
          WHEN state IS NULL THEN level
          WHEN state < level THEN level
          ELSE state END
    $$ LANGUAGE SQL IMMUTABLE;

    CREATE AGGREGATE security_min(classification_level) (
        sfunc    = security_min_state,
        stype    = classification_level,
        initcond = 'top_secret'
    );

    CREATE AGGREGATE security_max(classification_level) (
        sfunc    = security_max_state,
        stype    = classification_level,
        initcond = 'unclassified'
    );

    CREATE FUNCTION security_clearance(token UUID, token2value regclass)
      RETURNS classification_level AS $$
    BEGIN
      RETURN provenance_evaluate(
        token, token2value,
        'unclassified'::classification_level,
        'security_min', 'security_max');
    END
    $$ LANGUAGE plpgsql;

    SELECT create_provenance_mapping('personnel_level',
                                     'personnel', 'classification');

    SELECT p1.city,
           security_clearance(provenance(), 'personnel_level') AS min_clearance
    FROM personnel p1
    JOIN personnel p2 ON p1.city = p2.city AND p1.id < p2.id
    GROUP BY p1.city
    ORDER BY p1.city;

Results: Nairobi requires only ``unclassified`` (Juma), Beijing requires
``secret`` (both Ellen and Jing hold the same level), and Paris requires
``confidential`` (David is the least-classified of the three Paris agents).


Step 5: Cities with Exactly One Agent (EXCEPT / Monus)
-------------------------------------------------------

A city with a single agent is sensitive: knowing the city immediately
identifies the agent. Find cities where *all* agents are alone using
``EXCEPT``:

.. code-block:: postgresql

    SELECT DISTINCT city,
           sr_formula(provenance(), 'personnel_name') AS formula
    FROM (
        SELECT DISTINCT city FROM personnel
      EXCEPT
        SELECT p1.city
        FROM personnel p1
        JOIN personnel p2 ON p1.city = p2.city AND p1.id < p2.id
        GROUP BY p1.city
    ) t
    ORDER BY city;

.. note::

   ProvSQL's ``EXCEPT`` uses the *monus* operator ``⊖`` of the
   provenance semiring rather than plain set difference. Every city
   appears in the result with a provenance formula; the formula
   evaluates to ``𝟘`` for cities that are definitely shared and to a
   non-trivial expression for cities that *could* be single-agent in
   some possible world. Nairobi's formula
   ``(Juma ⊕ Paul) ⊖ (Juma ⊗ Paul)`` reads: "at least one of Juma
   or Paul is present, minus the event where both are."


Step 6: Where-Provenance
-------------------------

Where-provenance tracks *which column of which input row* produced each
output value. Enable it and re-run the shared-city query:

.. code-block:: postgresql

    SET provsql.where_provenance = on;

    SELECT DISTINCT p1.city,
           where_provenance(provenance()) AS source
    FROM personnel p1
    JOIN personnel p2 ON p1.city = p2.city AND p1.id < p2.id
    GROUP BY p1.city
    ORDER BY p1.city;

    SET provsql.where_provenance = off;

Each city output value is traced back to the ``city`` column (column 4)
of the ``personnel`` table for every agent in that city. The notation
``[personnel:〈token〉:4;...]`` shows the token and column index of each
contributing row; the trailing ``[]`` is the untracked ``source``
column itself.


Step 7: Assign Probabilities
-----------------------------

Suppose the existence of each agent in the database is uncertain.
Assign each agent a probability equal to ``id / 10.0``::

    ALTER TABLE personnel ADD COLUMN probability DOUBLE PRECISION;
    UPDATE personnel SET probability = id / 10.0;

    DO $$ BEGIN
      PERFORM set_prob(provenance(), probability) FROM personnel;
    END $$;

Now Juma has probability 0.1, Paul 0.2, …, Jing 0.7.


Step 8: Probability – Exact (Possible-Worlds)
----------------------------------------------

Compute the exact probability that each city is a single-agent city:

.. code-block:: postgresql

    SELECT city,
           ROUND(probability_evaluate(provenance())::numeric, 4) AS prob
    FROM (
        SELECT DISTINCT city FROM personnel
      EXCEPT
        SELECT p1.city
        FROM personnel p1
        JOIN personnel p2 ON p1.city = p2.city AND p1.id < p2.id
        GROUP BY p1.city
    ) t
    ORDER BY city;

Nairobi (agents with probabilities 0.1 and 0.2) has probability
``0.1 × 0.8 + 0.9 × 0.2 = 0.26`` of having exactly one agent. Beijing
(0.4 and 0.7) scores ``0.54``. Paris (0.3, 0.5, 0.6) gives ``0.41``.


Step 9: Probability – Monte Carlo
-----------------------------------

For larger circuits, exact evaluation can be expensive. Monte Carlo
sampling gives an approximate answer:

.. code-block:: postgresql

    SELECT city,
           ROUND(probability_evaluate(
               provenance(), 'monte-carlo', '10000')::numeric, 1) AS prob
    FROM (
        SELECT DISTINCT city FROM personnel
      EXCEPT
        SELECT p1.city
        FROM personnel p1
        JOIN personnel p2 ON p1.city = p2.city AND p1.id < p2.id
        GROUP BY p1.city
    ) t
    ORDER BY city;

With 10 000 samples the result is accurate to roughly ±0.01.
Use ``\timing`` in psql to compare the runtime against the exact method.


Step 10: Probability – Knowledge Compiler
------------------------------------------

.. note::

   This step requires an external knowledge compiler such as ``d4`` or
   ``dsharp`` to be installed and on your ``PATH``. Skip it if neither
   is available.

A knowledge compiler converts the provenance circuit to a *d-DNNF*
representation, enabling exact probability evaluation in time
proportional to circuit size:

.. code-block:: postgresql

    SELECT city,
           ROUND(probability_evaluate(
               provenance(), 'compilation', 'd4')::numeric, 4) AS prob
    FROM (
        SELECT DISTINCT city FROM personnel
      EXCEPT
        SELECT p1.city
        FROM personnel p1
        JOIN personnel p2 ON p1.city = p2.city AND p1.id < p2.id
        GROUP BY p1.city
    ) t
    ORDER BY city;

Compare the runtime with ``\timing`` against the possible-worlds and
Monte Carlo methods.


Step 11: Visualise a Provenance Circuit
-----------------------------------------

:sqlfunc:`view_circuit` returns the provenance circuit as a
`Graphviz <https://graphviz.org>`_ DOT string, which you can pipe to
``dot -Tpng`` for a PNG image:

.. code-block:: postgresql

    SELECT city, view_circuit(provenance(), 'personnel_name') AS dot
    FROM (
        SELECT DISTINCT city FROM personnel
      EXCEPT
        SELECT p1.city
        FROM personnel p1
        JOIN personnel p2 ON p1.city = p2.city AND p1.id < p2.id
        GROUP BY p1.city
    ) t
    WHERE city = 'Nairobi';

Copy the DOT text and run::

    echo '<DOT text>' | dot -Tpng -o nairobi_circuit.png

The resulting PNG shows the monus gate at the top, with ``Juma`` and
``Paul`` as leaf inputs.


Step 12: Export to XML
-----------------------

:sqlfunc:`to_provxml` serialises the provenance circuit to XML, which
can be processed by standard XML tools:

.. code-block:: postgresql

    SELECT city,
           to_provxml(provenance(), 'personnel_name') AS provxml
    FROM (
        SELECT DISTINCT city FROM personnel
      EXCEPT
        SELECT p1.city
        FROM personnel p1
        JOIN personnel p2 ON p1.city = p2.city AND p1.id < p2.id
        GROUP BY p1.city
    ) t
    WHERE city = 'Nairobi';


Step 13: Large Circuit Benchmark
----------------------------------

To compare the three probability algorithms at scale, create a synthetic
100 × 100 random-probability matrix and run a path query:

.. code-block:: psql

    CREATE TABLE matrix AS
    SELECT ones.n + 10 * tens.n AS x,
           other.n + 10 * tens2.n AS y,
           random() AS prob
    FROM (VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9)) ones(n),
         (VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9)) tens(n),
         (VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9)) other(n),
         (VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9)) tens2(n);

    SELECT add_provenance('matrix');
    DO $$ BEGIN
      PERFORM set_prob(provenance(), prob) FROM matrix;
    END $$;

    \timing
    SELECT m1.x, m2.y,
           probability_evaluate(provenance(), 'possible-worlds') AS prob
    FROM matrix m1, matrix m2
    WHERE m2.x = m1.y AND m1.x > 90 AND m2.x > 90 AND m2.y > 90
    GROUP BY m1.x, m2.y
    ORDER BY m1.x, m2.y;

    SELECT m1.x, m2.y,
           probability_evaluate(provenance(), 'monte-carlo', '9604') AS prob
    FROM matrix m1, matrix m2
    WHERE m2.x = m1.y AND m1.x > 90 AND m2.x > 90 AND m2.y > 90
    GROUP BY m1.x, m2.y
    ORDER BY m1.x, m2.y;

    SELECT m1.x, m2.y,
           probability_evaluate(provenance(), 'compilation', 'd4') AS prob
    FROM matrix m1, matrix m2
    WHERE m2.x = m1.y AND m1.x > 90 AND m2.x > 90 AND m2.y > 90
    GROUP BY m1.x, m2.y
    ORDER BY m1.x, m2.y;

The Monte Carlo query uses ``9604`` samples, which gives roughly 1 %
additive error with 95 % confidence (by the formula
:math:`n = z^2 / (4\varepsilon^2)` with :math:`z = 1.96`,
:math:`\varepsilon = 0.01`).
