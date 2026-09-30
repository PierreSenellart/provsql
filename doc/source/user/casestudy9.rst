.. nb:name: cs9
.. nb:database: cs9

Case Study: A Sales Forecast Dashboard
======================================

This case study runs the queries of an ordinary sales dashboard over a
pipeline of deals that may or may not close. Every figure the dashboard
shows becomes a random quantity, and ProvSQL answers the same SQL with its
distribution. It demonstrates aggregates and their expected values,
``ROLLUP`` subtotals, shares of a total, ranking and top-k over aggregates,
comparisons with a target, percentages with a ``NULLIF`` divisor,
aggregates of aggregates, grouping by an aggregate's value, ``DISTINCT
ON``, and :sqlfunc:`plain` for a value meant without provenance.

The Scenario
------------

A sales team tracks twelve open deals in three regions over two quarters.
The CRM gives each deal a *win probability* from its stage in the pipeline;
deals close independently of each other. Management wants the usual
dashboard: revenue per region and quarter, each region's share, which
region leads, whether each region meets its target, the typical deal size.

Your tasks:

* forecast revenue per region, with and without the regions that close
  nothing,
* find which region is likely to lead, and each region's likely rank,
* check each region against its target,
* describe the deals that close: their share of big deals, their spread,
  the largest one per region.

Setup
-----

.. nb:skip
.. tip::

   **Prefer not to install? Use the Playground.** You can skip the manual
   setup below: open this case study `as a runnable notebook in the ProvSQL
   Playground <https://provsql.org/playground/?nb=cs9>`_ (every query is a
   cell, and the opening cells set up the database for you), or open the bare
   `cs9 database <https://provsql.org/playground/?db=cs9>`_ and run the queries
   as you read. See the :ref:`Playground note <playground-note>`.

.. nb:omit-begin

This case study assumes a working ProvSQL installation (see
:doc:`getting-provsql`). Download :download:`setup.sql <../../casestudy9/setup.sql>`
and load it into a fresh PostgreSQL database:

.. code-block:: bash

    psql -d mydb -f setup.sql


.. nb:omit-end

.. nb:setup: ../../casestudy9/setup.sql

This creates two tables:

* ``region`` -- the three regions (North, South, West) and each one's
  revenue target, in thousands of euros
* ``deal`` -- twelve open deals, each with a customer, a region, a quarter,
  an amount in thousands of euros, and its win probability ``win_prob``


Step 1: The Pipeline
--------------------

.. nb:omit-begin

At the start of every session, set the search path:

.. code-block:: postgresql

    SET search_path TO public, provsql;

.. nb:omit-end

.. code-block:: postgresql

    SELECT * FROM region ORDER BY name;
    SELECT * FROM deal ORDER BY region, quarter, id;

Track the deals, and give each one its win probability:

.. code-block:: postgresql

    SELECT add_provenance('deal');
    SELECT set_prob(provenance(), win_prob) FROM deal;

Each set of closing deals is now a possible world, with the product of
the probabilities of its deals closing and the others not. ``region`` is
not tracked: the targets are certain.


Step 2: Expected Revenue per Region
-----------------------------------

.. code-block:: postgresql

    SELECT region,
           sum(amount) AS pipeline,
           round(expected(sum(amount))::numeric, 2) AS if_any_closes,
           round(expected(coalesce(sum(amount), 0))::numeric, 2) AS forecast,
           round(avg(amount), 1) AS avg_deal
    FROM deal
    GROUP BY region
    ORDER BY region;

``pipeline`` is the value when every deal closes: 275
for North. The marker ``(*)`` says it is an aggregate result, whose value
depends on the world.

The two expectations answer two questions. A region that closes no deal has
no row in a ``GROUP BY``, so ``expected(sum(amount))`` is the expected
revenue *given that the region closes something*: 155.59 for North. Written
with ``coalesce``, the sum is 0 in the worlds where the region closes
nothing, and ``expected`` averages over every world: the forecast, 154.50
for North, which is ``80 × 0.9 + 45 × 0.5 + 120 × 0.3 + 30 × 0.8``, each
amount times the probability of its deal closing.

``round(avg(amount), 1)`` rounds the average in every world: the rounding
is carried with the aggregate, not applied to its value on the data as it
is.


Step 3: Subtotals with ``ROLLUP``
---------------------------------

.. code-block:: postgresql

    SELECT region, quarter,
           round(expected(coalesce(sum(amount), 0))::numeric, 2) AS forecast
    FROM deal
    GROUP BY ROLLUP (region, quarter)
    ORDER BY region, quarter;

Each subtotal is a sum of its own, and the forecasts add up: North's
Q1 (94.50) and Q2 (60.00) make 154.50, and the three regions 405.00, the
grand total.


Step 4: Each Region's Share
---------------------------

.. code-block:: postgresql

    SELECT region,
           round(expected(sum(amount)::numeric
                          / (SELECT sum(amount) FROM deal))::numeric, 4)
             AS share
    FROM deal
    GROUP BY region
    ORDER BY region;

The share is a ratio of two sums that vary together: in each world, a
region's revenue over the total revenue of that same world. Its expected
value (0.3891 for North) is not the ratio of the forecasts (154.50 /
405.00 = 0.3815), since a world where North closes a large deal is also a
world with a larger total.


Step 5: Which Region Leads?
---------------------------

The top region is the first row of a sort by revenue:

.. code-block:: postgresql

    SELECT region,
           round(probability_evaluate(provenance())::numeric, 4) AS p_top
    FROM (SELECT region, sum(amount) AS revenue
          FROM deal
          GROUP BY region
          ORDER BY revenue DESC
          FETCH FIRST 1 ROW WITH TIES) t
    ORDER BY region;

Each region is kept in the worlds where it leads: North with probability
0.4516, South 0.2790, West 0.2960. When every deal closes, West leads, with
310; but West's lead rests on Sierra, a 150 deal at 0.2.

A ``rank()`` over the revenue gives each region its rank in every world:

.. code-block:: postgresql

    SELECT region, rk, round(expected(rk)::numeric, 4) AS expected_rank
    FROM (SELECT region, rank() OVER (ORDER BY sum(amount) DESC) AS rk
          FROM deal
          GROUP BY region) t
    ORDER BY region;

North's expected rank, 1.7398, is the best of the three.


Step 6: On Target?
------------------

.. code-block:: postgresql

    SELECT d.region, sum(d.amount) >= r.target AS on_target,
           round(probability_evaluate(provenance())::numeric, 4) AS p
    FROM deal d JOIN region r ON r.name = d.region
    GROUP BY d.region, r.target
    ORDER BY d.region, on_target;

The comparison has one truth value per world, so each region comes out
once per truth value, with its probability: North meets its target of 150
with probability 0.5490 and misses it with probability 0.4440. The two do
not add up to 1: in the remaining worlds, North closes nothing and has no
row. A ``NULL`` target would make the comparison unknown, in a row of its
own; the targets are declared ``NOT NULL``, so there is none.


Step 7: The Share of Big Deals
------------------------------

.. code-block:: postgresql

    SELECT region,
           round(expected(100.0 * count(*) FILTER (WHERE amount >= 60)
                          / NULLIF(count(*), 0))::numeric, 2) AS pct_big
    FROM deal
    GROUP BY region
    ORDER BY region;

The percentage of closed deals of at least 60 is read in every world, the
``FILTER`` and the ``NULLIF`` divisor included: 49.35% expected for North.


Step 8: The Best Region and the Average Region
----------------------------------------------

An aggregate of the regional revenues reads them in every world:

.. code-block:: postgresql

    SELECT round(expected(max(revenue))::numeric, 2) AS best_region,
           round(expected(avg(revenue))::numeric, 2) AS average_region
    FROM (SELECT region, sum(amount) AS revenue
          FROM deal GROUP BY region) t;

The best region's revenue is 194.73 on average, well above any single
region's forecast: whichever region is lucky in a world, that one counts.


Step 9: How Many Deals Does a Region Close?
-------------------------------------------

Grouping by an aggregate's value groups the regions by the number of deals
they close, in every world:

.. code-block:: postgresql

    SELECT n AS deals_closed,
           round(probability_evaluate(provenance())::numeric, 4) AS p
    FROM (SELECT region, count(*) AS n FROM deal GROUP BY region) t
    GROUP BY n
    ORDER BY n;

A row for ``n`` is there in the worlds where *some* region closes exactly
``n`` deals: most likely 2 (0.7629) or 3 (0.7458). Several values can
hold at once, one per region, so the probabilities add up to more than 1.


Step 10: The Largest Deal of Each Region
----------------------------------------

``DISTINCT ON`` keeps the first row of each region in the given order:

.. code-block:: postgresql

    SELECT region, customer, amount,
           round(probability_evaluate(provenance())::numeric, 4) AS p
    FROM (SELECT DISTINCT ON (region) region, customer, amount
          FROM deal
          ORDER BY region, amount DESC) t
    ORDER BY region, amount DESC;

Each deal is the largest of its region in the worlds where it closes and
no larger one does. Arctis (80) is North's largest with probability
``0.9 × (1 - 0.3) = 0.63``: it closes, and Fjordline (120) does not.


Step 11: The Spread of Deal Sizes
---------------------------------

.. code-block:: postgresql

    SELECT region,
           round(stddev(amount), 1) AS sd,
           round(expected(stddev(amount))::numeric, 2) AS expected_sd
    FROM deal
    GROUP BY region
    ORDER BY region;

A standard deviation needs two deals: ``expected`` averages it over the
worlds where the region closes at least two, 33.31 for North, against
40.1 when every deal closes.


Step 12: A Value Meant Without Provenance
-----------------------------------------

A dashboard label turns the revenue into text:

.. code-block:: postgresql

    SELECT region, sum(amount)::text || ' k€' AS label
    FROM deal
    GROUP BY region
    ORDER BY region;

A text has no value per world: the label is computed without provenance,
and a ``WARNING`` says so, naming what read the value (``reader:
operator``). When that value is what is meant, :sqlfunc:`plain` says so,
and the warning goes away:

.. code-block:: postgresql

    SELECT region, plain(sum(amount))::text || ' k€' AS label
    FROM deal
    GROUP BY region
    ORDER BY region;

See :doc:`aggregation` for the aggregates and their values in every world,
and :ref:`plain-sql` for :sqlfunc:`plain`.
