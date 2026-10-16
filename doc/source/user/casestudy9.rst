.. nb:name: cs9
.. nb:database: cs9

Case Study: A Sales Forecast Dashboard
======================================

This case study runs the queries of an ordinary sales dashboard over a
pipeline of deals that may or may not close. Every figure on the dashboard
then depends on which deals close, and ProvSQL answers the same SQL with
the distribution of that figure. It demonstrates aggregates and their expected values,
``ROLLUP`` subtotals, aggregates over windows, shares of a total, ranking
and top-k over aggregates, comparisons with a target, arithmetic between
aggregates, Boolean aggregates and ``EXISTS`` as values, percentages with a
``NULLIF`` divisor, aggregates of aggregates, grouping by an aggregate's
value, ``DISTINCT ON``, :sqlfunc:`plain` for a value meant without
provenance, and the purposes an answer may serve under the customers'
consent.

The Scenario
------------

A sales team tracks twelve open deals in three regions over two quarters.
The CRM gives each deal a *win probability* from its stage in the pipeline;
deals close independently of each other. Management wants the usual
dashboard: revenue per region and quarter, each region's share, which
region leads, whether each region meets its target, the typical deal size.

Your tasks:

* forecast revenue per region, counting or not the regions where no deal
  is made,
* follow revenue through the quarters, and each deal's share of it,
* find which region is likely to lead, and each region's likely rank,
* check each region against its target, and its growth from Q1 to Q2,
* describe the deals that close: whether a big one does, their share of
  big deals, their spread, the largest one per region,
* use each answer only for the purposes its customers consented to.

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

The setup script calls :sqlfunc:`setup_search_path`, which adds
``provsql`` to the database's ``search_path``, so that every new session
can call ProvSQL's functions without the ``provsql.`` prefix.

.. nb:omit-end

.. nb:setup: ../../casestudy9/setup.sql

This creates three tables:

* ``region`` -- the three regions (North, South, West) and each one's
  revenue target, in thousands of euros
* ``deal`` -- twelve open deals, each with a customer, a region, a quarter,
  an amount in thousands of euros, and its win probability ``win_prob``
* ``consent`` -- the purposes (``forecasting``, ``analytics``,
  ``marketing``) for which each customer allows their data to be used


Step 1: The Pipeline
--------------------

Look at the regions and the deals:

.. code-block:: postgresql

    SELECT * FROM region ORDER BY name;
    SELECT * FROM deal ORDER BY region, quarter, id;

Track the deals, and give each one its win probability:

.. code-block:: postgresql

    SELECT add_provenance('deal');
    SELECT set_prob(provenance(), win_prob) FROM deal;

Each set of deals that close is now a possible world. Its probability is
the product of the probabilities of its deals closing and of the other
deals not closing. ``region`` is not tracked: its targets are certain.


Step 2: Expected Revenue per Region
-----------------------------------

.. code-block:: postgresql

    SELECT region,
           sum(amount) AS pipeline,
           round(expected(sum(amount))::numeric, 2) AS if_any_deal,
           round(expected(coalesce(sum(amount), 0))::numeric, 2) AS forecast,
           round(avg(amount), 1) AS avg_deal
    FROM deal
    GROUP BY region
    ORDER BY region;

``pipeline`` is the revenue if every deal closes: 275 for North. The marker
``(*)`` shows an aggregate result, whose value depends on the world.
``round(avg(amount), 1)`` rounds the average in each world, not only its
value on the data as it is.

The two expectations answer two questions. In a world where no deal is
made in a region, its group is empty and ``sum(amount)`` is NULL, as SQL's
``sum`` is over no rows. :sqlfunc:`expected` averages over the worlds where its
argument has a value, so ``expected(sum(amount))`` is the expected revenue
*given that some deal is made in the region*: 155.59 for North.
``coalesce`` turns that NULL into 0, and ``expected`` then averages over
every world: the forecast, 154.50 for North, which is
``80 × 0.9 + 45 × 0.5 + 120 × 0.3 + 30 × 0.8``, each amount times the
probability of its deal closing. The two are consistent. North's row only
exists in the worlds where some deal is made in North, which have
probability ``probability_evaluate(provenance())`` = 0.9930, and
155.59 × 0.9930 = 154.50.

A dashboard counts a region, or a quarter, where no deal is made as 0. Most
of the following steps read the revenue from two views that do so.
``quarter_revenue`` has a row for each region and quarter: it crosses
``region`` with the list of quarters and joins the deals with a ``LEFT
JOIN``, so that each pair keeps its row even when no deal is made in it,
and ``coalesce`` turns the revenue of such a row into 0.
``region_revenue`` adds up the two quarters of each region.

.. code-block:: postgresql

    CREATE OR REPLACE VIEW quarter_revenue AS
      SELECT r.name AS region, r.target, q.quarter,
             coalesce(sum(d.amount), 0) AS revenue
      FROM region r CROSS JOIN (VALUES ('Q1'), ('Q2')) AS q(quarter)
           LEFT JOIN deal d ON d.region = r.name AND d.quarter = q.quarter
      GROUP BY r.name, q.quarter;

    CREATE OR REPLACE VIEW region_revenue AS
      SELECT region, target, sum(revenue) AS revenue
      FROM quarter_revenue
      GROUP BY region, target;


Step 3: Subtotals with ``ROLLUP``
---------------------------------

.. code-block:: postgresql

    SELECT region, quarter,
           round(expected(sum(revenue))::numeric, 2) AS forecast
    FROM quarter_revenue
    GROUP BY ROLLUP (region, quarter)
    ORDER BY region, quarter;

Each subtotal is a sum in its own right, and the forecasts add up: North's
Q1 (94.50) and Q2 (60.00) make 154.50, and the three regions 405.00, the
grand total.


Step 4: Revenue to Date and Each Deal's Share
---------------------------------------------

An aggregate used as a window function is computed in each world, over the
rows of its frame that exist there. Each deal gets the revenue of its region
up to its quarter, and its share of its region's revenue:

.. code-block:: postgresql

    SELECT customer, region, quarter, amount,
           sum(amount) OVER (PARTITION BY region ORDER BY quarter) AS to_date,
           round(expected(sum(amount) OVER (PARTITION BY region
                                            ORDER BY quarter),
                          provenance())::numeric, 2) AS expected_to_date,
           round(expected(100.0 * amount
                          / sum(amount) OVER (PARTITION BY region),
                          provenance())::numeric, 2) AS pct_of_region
    FROM deal
    ORDER BY region, quarter, id;

``to_date`` is the value if every deal closes, 275 for Fjordline: all of
North's deals, Q1 and Q2. Each row has the provenance of its deal, so a
value computed for that row only makes sense in the worlds where the deal
closes: the second argument of :sqlfunc:`expected` restricts the average to
those worlds. Given that Fjordline closes, North's revenue to date is
``120 + 80 × 0.9 + 45 × 0.5 + 30 × 0.8 = 238.50``, and Sierra makes
63.39% of West's revenue on average.

Without that argument, :sqlfunc:`expected` also averages over the worlds
where Sierra does not close: Sierra's share would come out as 166.87%, its
150 divided by revenues that do not include it. Studio adds this condition
itself when it evaluates such a value.


Step 5: Each Region's Share
---------------------------

.. code-block:: postgresql

    SELECT region,
           round(expected(revenue::numeric
                          / (SELECT sum(amount) FROM deal))::numeric, 4)
             AS share
    FROM region_revenue
    ORDER BY region;

The share is a ratio of two sums that vary together: in each world, a
region's revenue over the total revenue of that same world. Its expected
value (0.3863 for North) is not the ratio of the forecasts (154.50 /
405.00 = 0.3815): in a world where North closes a large deal, the total is
larger too.


Step 6: Which Region Leads?
---------------------------

The top region is the first row of a sort by revenue:

.. code-block:: postgresql

    SELECT region,
           round(probability_evaluate(provenance())::numeric, 4) AS p_top
    FROM (SELECT region
          FROM region_revenue
          ORDER BY revenue DESC
          FETCH FIRST 1 ROW WITH TIES) t
    ORDER BY region;

``FETCH FIRST 1 ROW`` is the SQL-standard way of writing ``LIMIT 1``; its
``WITH TIES`` option, which ``LIMIT`` lacks, also keeps the rows that tie
with the last one, here a region with the same revenue as the leader.

Each region comes out with the probability that it leads: North 0.4516,
South 0.2790, West 0.2960. If every deal closes, West leads with 310, but
that lead rests on Sierra, a deal of 150 that closes with probability 0.2.

``rank()`` gives each region its rank in each world:

.. code-block:: postgresql

    SELECT region, rk, round(expected(rk)::numeric, 4) AS expected_rank
    FROM (SELECT region, rank() OVER (ORDER BY revenue DESC) AS rk
          FROM region_revenue) t
    ORDER BY region;

North's expected rank, 1.7483, is the best of the three.


Step 7: On Target?
------------------

Does each region meet its revenue target? Compare its revenue with the
target in ``region``:

.. code-block:: postgresql

    SELECT region, revenue >= target AS on_target,
           round(probability_evaluate(provenance())::numeric, 4) AS p
    FROM region_revenue
    ORDER BY region, on_target;

The comparison is true in some worlds and false in others, so each region
comes out once per truth value, with its probability: North meets its
target of 150 with probability 0.5490 and misses it with probability
0.4510, including the worlds where no deal is made in North.


Step 8: Quarter-over-Quarter Growth
-----------------------------------

A region's growth compares its two rows in ``quarter_revenue``, one per
quarter:

.. code-block:: postgresql

    SELECT q1.region,
           round(expected(q2.revenue - q1.revenue)::numeric, 2) AS growth
    FROM quarter_revenue q1
         JOIN quarter_revenue q2 ON q2.region = q1.region
    WHERE q1.quarter = 'Q1' AND q2.quarter = 'Q2'
    ORDER BY q1.region;

The difference is computed in each world, so its expected value is the
difference of the two forecasts: North shrinks by 34.50 on average
(``(120 × 0.3 + 30 × 0.8) - (80 × 0.9 + 45 × 0.5)``), and West, 0.00, is
flat.

The regions whose Q2 revenue beats their Q1 revenue:

.. code-block:: postgresql

    SELECT q1.region,
           round(probability_evaluate(provenance())::numeric, 4) AS p_growth
    FROM quarter_revenue q1
         JOIN quarter_revenue q2 ON q2.region = q1.region
    WHERE q1.quarter = 'Q1' AND q2.quarter = 'Q2'
      AND q2.revenue > q1.revenue
    ORDER BY q1.region;

Each region comes out with the probability that its Q2 revenue beats its Q1
revenue: West 0.4400, although its expected growth is zero, and South, the
likeliest to grow, 0.5080.


Step 9: The Share of Big Deals
------------------------------

.. code-block:: postgresql

    SELECT region,
           round(expected(100.0 * count(*) FILTER (WHERE amount >= 60)
                          / NULLIF(count(*), 0))::numeric, 2) AS pct_big
    FROM deal
    GROUP BY region
    ORDER BY region;

The percentage of closed deals worth at least 60 is computed in each world,
``FILTER`` and ``NULLIF`` included: 49.35% on average for North.


Step 10: Any Big Deal Closed?
-----------------------------

A Boolean aggregate is true in some worlds and false in others. Cast to an
integer, it is 1 or 0, and its expected value is the probability that it is
true. An ``EXISTS`` in the ``SELECT`` list is a Boolean value too:

.. code-block:: postgresql

    SELECT region, bool_or(amount >= 100) AS big_deal,
           round(expected(coalesce(bool_or(amount >= 100), false)::int)::numeric, 4)
             AS p_big
    FROM deal
    GROUP BY region
    ORDER BY region;

    SELECT r.name,
           EXISTS (SELECT * FROM deal d
                   WHERE d.region = r.name AND d.amount >= 100) AS big_deal,
           round(probability_evaluate(provenance())::numeric, 4) AS p
    FROM region r
    ORDER BY r.name, big_deal;

Both queries give North a big deal with probability 0.3000, the probability
that Fjordline closes. In the first, ``coalesce`` makes ``bool_or`` false in
the worlds where no deal is made in the region (Step 2). The second reads
``region``, which is not tracked: each region exists in every world and
comes out once per truth value. South has no big deal in any world.


Step 11: The Best Region and the Average Region
-----------------------------------------------

An aggregate over the regions' revenues is computed in each world too:

.. code-block:: postgresql

    SELECT round(expected(max(revenue))::numeric, 2) AS best_region,
           round(expected(avg(revenue))::numeric, 2) AS average_region
    FROM region_revenue;

The best region's revenue is 194.73 on average, well above any single
region's forecast: in each world, it is the revenue of the luckiest region.
The average over the regions, 135.00, is the mean of the three forecasts of
Step 2.


Step 12: How Many Deals Does a Region Close?
--------------------------------------------

Grouping by an aggregate's value groups the regions by the number of deals
they close, in each world:

.. code-block:: postgresql

    SELECT n AS deals_closed,
           round(probability_evaluate(provenance())::numeric, 4) AS p
    FROM (SELECT region, count(*) AS n FROM deal GROUP BY region) t
    GROUP BY n
    ORDER BY n;

The row for ``n`` exists in the worlds where *some* region closes exactly
``n`` deals: most likely 2 (0.7629) or 3 (0.7458). Several values can
hold at once, one per region, so the probabilities add up to more than 1.


Step 13: The Largest Deal of Each Region
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


Step 14: The Spread of Deal Sizes
---------------------------------

.. code-block:: postgresql

    SELECT region,
           round(stddev(amount), 1) AS sd,
           round(expected(stddev(amount))::numeric, 2) AS expected_sd
    FROM deal
    GROUP BY region
    ORDER BY region;

A standard deviation needs at least two deals, so ``expected`` averages it
over the worlds where the region closes at least two: 33.31 for North,
against 40.1 if every deal closes.


Step 15: A Value Meant Without Provenance
-----------------------------------------

A dashboard label turns the revenue into text:

.. code-block:: postgresql

    SELECT region, sum(amount)::text || ' k€' AS label
    FROM deal
    GROUP BY region
    ORDER BY region;

A text cannot change from world to world, so the label is computed on the
data as it is, without provenance, and a ``WARNING`` says so, naming what
read the value (``reader: operator``). When that value is what you want,
wrap it in :sqlfunc:`plain` to say so, and the warning goes away:

.. code-block:: postgresql

    SELECT region, plain(sum(amount))::text || ' k€' AS label
    FROM deal
    GROUP BY region
    ORDER BY region;

Where reading a value without provenance would be a mistake, set
``provsql.implicit_freeze`` to ``'error'``: the first query is then refused
instead of warned about, and the one with :sqlfunc:`plain` is still
accepted:

.. code-block:: postgresql

    SET provsql.implicit_freeze = 'error';
    SELECT region, sum(amount)::text || ' k€' AS label
    FROM deal
    GROUP BY region;

The error carries the same ``DETAIL`` line as the warning. Restore the
default:

.. code-block:: postgresql

    RESET provsql.implicit_freeze;

See :doc:`aggregation` for the aggregates and their values in every world,
and :ref:`plain-sql` for :sqlfunc:`plain` and
:ref:`provsql.implicit_freeze <provsql-implicit-freeze>`.


Step 16: Purposes a Result May Serve
------------------------------------

Marketing plans a campaign in the regions with no big deal (100 k€ or
more). Each customer allows their data to be used for some purposes only,
and an answer may serve a purpose only if all the deals it reads may. Map each deal to its
customer's purposes, and evaluate with :sqlfunc:`sr_consent`:

.. code-block:: postgresql

    DROP TABLE IF EXISTS deal_consent;
    CREATE TABLE deal_consent AS
      SELECT d.provsql AS provenance, c.purposes AS value
      FROM deal d JOIN consent c USING (customer);

    SELECT r.name,
           (sr_consent(provenance(), 'deal_consent',
                       NULL::purpose)).*,
           consented_for(provenance(), 'deal_consent', 'marketing')
             AS for_marketing,
           consent_conflicts(provenance(), 'deal_consent',
                             NULL::purpose) AS conflicts
    FROM region r
    WHERE NOT EXISTS (SELECT * FROM deal d
                      WHERE d.region = r.name AND d.amount >= 100)
    ORDER BY r.name;

Two columns say which purposes may use each row:

* ``purposes``: the purposes for which the region has no big deal, when the
  query reads only the deals allowed for that purpose;
* ``unrestricted``: whether the region has no big deal when the query reads
  all the deals.

Region by region:

* South has no big deal at all, so every purpose may use it.
* West has one, Sierra's, and every purpose may read it: no purpose finds
  West free of big deals, so ``purposes`` is empty. West still has a row
  because it has no big deal in the worlds where Sierra does not close.
* North has one too, Fjordline's, but Fjordline allows forecasting only.
  Analytics and marketing do not see it, so North looks free of big deals
  to them, which is wrong.

:sqlfunc:`consented_for` keeps a region for a purpose only if the purpose
is in ``purposes`` and ``unrestricted`` is true, so the campaign gets South
only. :sqlfunc:`consent_conflicts` lists the purposes that would get a
wrong answer: analytics and marketing, for North.
:sqlfunc:`consent_purposes` lists the purposes a row may be used for.
