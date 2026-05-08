Case Study: The Intelligence Agency
=====================================

This case study, largely extending the scenario introduced in
:cite:`DBLP:journals/pvldb/SenellartJMR18`, demonstrates ProvSQL's
custom semiring capability, where-provenance, probability computation
with multiple algorithms, and circuit export through a
security-classification scenario.

The Scenario
------------

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
  (``unclassified`` < ``restricted`` < ``confidential`` < ``secret`` < ``top_secret`` < ``unavailable``)
  where ``unavailable`` is a sentinel representing the semiring 𝟘 (no derivation possible)
* ``personnel`` – 7 agents with name, position, city, and clearance level


Step 1: Explore the Database
-----------------------------

At the start of every session, set the search path so that ProvSQL functions
can be called without the ``provsql.`` prefix:

.. code-block:: postgresql

    SET search_path TO public, provsql;

Inspect the ``personnel`` table:

.. code-block:: postgresql

    SELECT * FROM personnel ORDER BY id;

You should see seven rows: Juma (Director, Nairobi), Paul (Janitor, Nairobi),
David (Analyst, Paris), Ellen (Field agent, Beijing), Aaheli (Double agent,
Paris), Nancy (HR, Paris), and Jing (Analyst, Beijing).


Step 2: Enable Provenance and Create a Name Mapping
----------------------------------------------------

Enable provenance tracking on ``personnel`` and create a mapping so that
provenance tokens can be labelled with agent names:

.. code-block:: postgresql

    SELECT add_provenance('personnel');
    SELECT create_provenance_mapping('personnel_name', 'personnel', 'name');

After :sqlfunc:`add_provenance`, every row of ``personnel`` has a unique UUID
token in its hidden ``provsql`` column. The mapping ``personnel_name``
associates each token with the corresponding agent's name.


Step 3: Cities Shared by Multiple Agents
-----------------------------------------

Which cities have at least two agents? Use a self-join with an ``id``
inequality to generate all unordered pairs, then apply
:sqlfunc:`sr_formula` to see which agents contribute to each city:

.. code-block:: postgresql

    SELECT p1.city,
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

* ``⊕`` (OR combination) = ``MIN``: to infer *either* agent was
  involved, you only need clearance for the less-classified one (one
  witness suffices to establish the disjunction).
* ``⊗`` (AND/join) = ``MAX``: to confirm *both* agents are present,
  you need clearance for the more-classified one (you must be able to
  access both records to establish the join).

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
        initcond = 'unavailable'
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

Results: Nairobi requires ``restricted`` (Paul is the more-classified of
the two agents, and both must be accessed to confirm the pair). Beijing
requires ``secret`` (both Ellen and Jing hold the same level). Paris
requires ``confidential``: the pair David–Nancy has MAX clearance
``confidential``, which is the lowest maximum among all Paris pairs,
so ``confidential`` clearance suffices to confirm at least one pair.


Step 5: Cities with Exactly One Agent (EXCEPT / Monus)
-------------------------------------------------------

A city with a single agent is sensitive: knowing the city immediately
identifies the agent. Find cities where *all* agents are alone using
``EXCEPT``:

.. code-block:: postgresql

    SELECT city,
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

    SELECT p1.city,
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
Assign each agent a probability equal to ``id / 10.0``:

.. code-block:: postgresql

    ALTER TABLE personnel ADD COLUMN probability DOUBLE PRECISION;
    UPDATE personnel SET probability = id / 10.0;

    DO $$ BEGIN
      PERFORM set_prob(provenance(), probability) FROM personnel;
    END $$;

Now Juma has probability 0.1, Paul 0.2, …, Jing 0.7.


Step 8: Probability – Exact
---------------------------

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
           probability_evaluate(
               provenance(), 'monte-carlo', '10000') AS prob
    FROM (
        SELECT DISTINCT city FROM personnel
      EXCEPT
        SELECT p1.city
        FROM personnel p1
        JOIN personnel p2 ON p1.city = p2.city AND p1.id < p2.id
        GROUP BY p1.city
    ) t
    ORDER BY city;

With 10 000 samples the result is accurate to roughly ±0.01
(see `Margin of error <https://en.wikipedia.org/wiki/Margin_of_error>`_).
Results will vary slightly between runs due to sampling.
Use ``\timing`` in psql to compare the runtime against the exact method.


Step 10: Probability – Knowledge Compiler
------------------------------------------

.. note::

   This step requires an external knowledge compiler such as ``d4`` or
   ``dsharp`` to be installed and on your ``PATH`` (or in a directory
   listed in the ``provsql.tool_search_path`` GUC, see
   :doc:`configuration`). Skip it if neither is available.

A knowledge compiler converts the provenance circuit to a *d-DNNF*
representation, which enables efficient exact probability evaluation
on large circuits of specific forms:

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
Monte Carlo methods. On this small example, the external knowledge
compiler will be slower than the other methods: invoking an external
process and compiling the circuit carries significant overhead that
only pays off on much larger circuits.


Step 11: Visualise a Provenance Circuit
-----------------------------------------

:sqlfunc:`view_circuit` renders the provenance circuit as an ASCII
box-art diagram using
`graph-easy <https://metacpan.org/dist/Graph-Easy>`_
(must be installed and on your ``PATH``):

.. code-block:: postgresql

    SELECT city, view_circuit(provenance(), 'personnel_name') AS circuit
    FROM (
        SELECT DISTINCT city FROM personnel
      EXCEPT
        SELECT p1.city
        FROM personnel p1
        JOIN personnel p2 ON p1.city = p2.city AND p1.id < p2.id
        GROUP BY p1.city
    ) t
    WHERE city = 'Nairobi';

The result shows the monus gate at the top, with ``Juma`` and
``Paul`` as leaf inputs, rendered in box-art notation in the terminal.


Step 12: Export to XML
-----------------------

:sqlfunc:`to_provxml` serialises the provenance circuit to
`PROV-XML <https://www.w3.org/TR/prov-xml/>`_, the W3C standard for
provenance interchange, which can be processed by any standard XML tool:

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
100 × 100 random-probability matrix and enable provenance tracking on it:

.. code-block:: postgresql

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

Now run the same path query with each method in turn (use ``\timing`` in
psql to record runtimes):

.. code-block:: postgresql

    -- Default method (independent evaluation, tree decomposition, or d4)
    SELECT m1.x, m2.y,
           probability_evaluate(provenance()) AS prob
    FROM matrix m1, matrix m2
    WHERE m2.x = m1.y AND m1.x > 90 AND m2.x > 90 AND m2.y > 90
    GROUP BY m1.x, m2.y
    ORDER BY m1.x, m2.y;

.. code-block:: postgresql

    -- Exact enumeration over all possible worlds
    SELECT m1.x, m2.y,
           probability_evaluate(provenance(), 'possible-worlds') AS prob
    FROM matrix m1, matrix m2
    WHERE m2.x = m1.y AND m1.x > 90 AND m2.x > 90 AND m2.y > 90
    GROUP BY m1.x, m2.y
    ORDER BY m1.x, m2.y;

.. code-block:: postgresql

    -- Monte Carlo sampling (9604 samples ≈ 1 % error at 95 % confidence)
    SELECT m1.x, m2.y,
           probability_evaluate(provenance(), 'monte-carlo', '9604') AS prob
    FROM matrix m1, matrix m2
    WHERE m2.x = m1.y AND m1.x > 90 AND m2.x > 90 AND m2.y > 90
    GROUP BY m1.x, m2.y
    ORDER BY m1.x, m2.y;

.. code-block:: postgresql

    -- Tree-decomposition-based exact compilation (built-in, no external tool)
    SELECT m1.x, m2.y,
           probability_evaluate(provenance(), 'tree-decomposition') AS prob
    FROM matrix m1, matrix m2
    WHERE m2.x = m1.y AND m1.x > 90 AND m2.x > 90 AND m2.y > 90
    GROUP BY m1.x, m2.y
    ORDER BY m1.x, m2.y;

.. code-block:: postgresql

    -- Knowledge compilation via external tool d4
    SELECT m1.x, m2.y,
           probability_evaluate(provenance(), 'compilation', 'd4') AS prob
    FROM matrix m1, matrix m2
    WHERE m2.x = m1.y AND m1.x > 90 AND m2.x > 90 AND m2.y > 90
    GROUP BY m1.x, m2.y
    ORDER BY m1.x, m2.y;

The Monte Carlo query uses ``9604`` samples, which gives roughly 1 %
additive error with 95 % confidence (by the formula
:math:`n = z^2 / (4\varepsilon^2)` with :math:`z = 1.96`,
:math:`\varepsilon = 0.01`;
see `Margin of error <https://en.wikipedia.org/wiki/Margin_of_error>`_).

The ``'tree-decomposition'`` method is exact and built into ProvSQL (no
external binary required). It is often the fastest exact method on simple
queries, but it fails on circuits with high treewidth – when that happens,
fall back to ``'compilation'`` or one of the other methods.


Step 14: The Boolean Expression Behind a Token
------------------------------------------------

:sqlfunc:`sr_boolexpr` returns the abstract Boolean formula of a provenance
circuit. Without a mapping it uses internal variable names ``x0``,
``x1``, …; with an optional second argument naming a provenance
mapping table the leaves are labelled by the mapping's ``value``
column instead. This is the same expression ProvSQL hands to its
d-DNNF compilers internally to compute probabilities.

.. code-block:: postgresql

    SELECT city, sr_boolexpr(provenance()) AS boolexpr
    FROM (
        SELECT DISTINCT city FROM personnel
      EXCEPT
        SELECT p1.city
        FROM personnel p1
        JOIN personnel p2 ON p1.city = p2.city AND p1.id < p2.id
        GROUP BY p1.city
    ) t
    WHERE city = 'Nairobi';

For Nairobi, the result is the circuit ``(Juma ⊕ Paul) ⊖ (Juma ⊗ Paul)``
from Step 5, interpreted in the Boolean function semiring – every
provenance gate is mapped to its Boolean counterpart (``⊕`` to ``∨``,
``⊗`` to ``∧``, ``⊖`` to ``∧¬``) – and the resulting Boolean function
rendered as a formula over anonymous variables. Unlike
:sqlfunc:`sr_formula`, the provenance mapping is optional: the
expression captures the circuit's logical structure independently of
any naming, and a mapping can be supplied later if you want the
leaves labelled.


Step 15: Programmatic Circuit Inspection
------------------------------------------

What :sqlfunc:`view_circuit` renders, you can also walk programmatically
with the low-level circuit API. Capture Nairobi's monus token first:

.. code-block:: postgresql

    CREATE TEMP TABLE nairobi_token AS
    SELECT provenance() AS prov
    FROM (
        SELECT DISTINCT city FROM personnel
      EXCEPT
        SELECT p1.city
        FROM personnel p1
        JOIN personnel p2 ON p1.city = p2.city AND p1.id < p2.id
        GROUP BY p1.city
    ) t
    WHERE city = 'Nairobi';

:sqlfunc:`get_nb_gates` reports how many gates have been materialized in the
current database's circuit:

.. code-block:: postgresql

    SELECT get_nb_gates();

:sqlfunc:`get_gate_type` and :sqlfunc:`get_children` give a single-step view
of the gate structure: they return the operator and direct children of a
gate.

.. code-block:: postgresql

    SELECT get_gate_type(prov)            AS root_type,
           get_children(prov)             AS root_children
    FROM nairobi_token;

For Nairobi, the root is a ``monus`` gate with two children: the ⊕
sub-circuit (``Juma ⊕ Paul``) and the ⊗ sub-circuit (``Juma ⊗ Paul``).
Recurse to inspect the children:

.. code-block:: postgresql

    SELECT (get_children(prov))[1]                        AS plus_token,
           get_gate_type((get_children(prov))[1])         AS plus_type,
           get_children((get_children(prov))[1])          AS plus_children
    FROM nairobi_token;

The leaves of the circuit are input gates that originate from the
``personnel`` table. :sqlfunc:`identify_token` performs a reverse lookup,
returning the table and column count for an input token:

.. code-block:: postgresql

    SELECT identify_token(child) AS source
    FROM nairobi_token, unnest(get_children((get_children(prov))[1])) AS child;

Both leaves resolve to ``(personnel, 6)`` – the ``personnel`` table with
its six non-provenance columns (``id``, ``name``, ``position``, ``city``,
``classification``, and the ``probability`` column added in Step 7). This
is exactly the traversal :sqlfunc:`view_circuit` performs to render the
box-art diagram.
