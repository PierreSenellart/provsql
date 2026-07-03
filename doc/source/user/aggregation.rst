Aggregation and Grouping
=========================

ProvSQL supports provenance tracking for ``GROUP BY`` queries and aggregate
functions :cite:`DBLP:conf/pods/AmsterdamerDT11`. The semantics follow a
*semimodule* model: aggregation is treated as a scalar multiplication of
provenance values.

GROUP BY Queries
-----------------

When a query includes a ``GROUP BY`` clause, each output group receives an
``agg`` gate in the provenance circuit. The children of this gate are the
provenance tokens of all input tuples that contributed to the group:

.. code-block:: postgresql

    SELECT dept, COUNT(*), provenance()
    FROM employees
    GROUP BY dept;

The resulting provenance token encodes *which* input tuples were combined
to produce each aggregate value.

SELECT DISTINCT
----------------

``SELECT DISTINCT`` is modelled as a ``GROUP BY`` on all selected columns.
Each distinct output row gets a provenance token that captures all the
duplicate source rows that were merged:

.. code-block:: postgresql

    SELECT DISTINCT dept, provenance()
    FROM employees;

Aggregate Functions
--------------------

The aggregate functions ``COUNT``, ``SUM``, ``MIN``, ``MAX``, and ``AVG``
are all supported over provenance-tracked tables.

Arithmetic on Aggregate Results
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Arithmetic, explicit casts, and other expressions
(``COALESCE``, ``GREATEST``, etc.) on aggregate results are supported,
both in the same query and over subquery results:

.. code-block:: postgresql

    SELECT dept, COUNT(*) * 10 FROM employees GROUP BY dept;
    SELECT dept, SUM(salary) + 1000 FROM employees GROUP BY dept;
    SELECT dept, string_agg(name, ', ') || ' (team)' FROM employees GROUP BY dept;
    SELECT cnt::numeric FROM (SELECT COUNT(*) AS cnt FROM employees GROUP BY dept) t;
    SELECT dept, COALESCE(cnt, 0) FROM (SELECT dept, COUNT(*) AS cnt FROM employees GROUP BY dept) t;
    SELECT dept, GREATEST(cnt, 3) FROM (SELECT dept, COUNT(*) AS cnt FROM employees GROUP BY dept) t;

When such an operation is performed, the aggregate result is cast from
its internal ``agg_token`` representation back to the original aggregate
return type (e.g., ``bigint`` for ``COUNT``, ``numeric`` for ``AVG``).
A warning is emitted to indicate that the provenance information is lost
in the conversion. The provenance of the aggregate group itself is still
tracked in the ``provsql`` column.

Window functions over aggregate results (e.g. ``SUM(cnt) OVER ()``)
execute but are **not** provenance-aware: the aggregate argument is cast
back to its base type before the window computation, so the windowed
value is an opaque scalar and a ``WARNING`` is emitted. See
:doc:`querying` for the general limitation on window functions.

Random-Variable Aggregates
---------------------------

When the aggregated column has type ``random_variable``
(see :doc:`continuous-distributions`), the standard arithmetic
aggregates lift to the distribution algebra:
:sqlfunc:`sum`, :sqlfunc:`avg`, and
:sqlfunc:`product`, plus the order statistics ``min`` and ``max``.
Each returns a ``random_variable`` rather than a scalar; ``min`` /
``max`` carry the empty-group identity ``+inf`` / ``-inf`` (the
extremum counterparts to ``sum``'s ``0`` and ``product``'s ``1``).
See :ref:`continuous-aggregation` for the semantics, empty-group
identities, and worked examples.

HAVING
------

``HAVING`` clauses are supported:

.. code-block:: postgresql

    SELECT dept, COUNT(*) AS n, provenance()
    FROM employees
    GROUP BY dept
    HAVING COUNT(*) > 2;

``HAVING`` clauses whose outcome is a deterministic scalar are also
supported, including conditions that wrap a ``random_variable``
aggregate in a moment function such as
``HAVING expected(avg(measurement)) > 20`` (see
:doc:`continuous-distributions`): the predicate is evaluated by
PostgreSQL on the surviving groups while ProvSQL still tracks the
per-group provenance.

Arithmetic in HAVING
~~~~~~~~~~~~~~~~~~~~~~

``HAVING`` conditions that apply arithmetic to aggregate results are
supported, with provenance and probabilities tracked correctly:

.. code-block:: postgresql

    -- constant arithmetic over a single aggregate
    SELECT dept, provenance() FROM employees GROUP BY dept
    HAVING sum(salary) + bonus > 100000;       -- folded to sum(salary) > 100000 - bonus

    -- arithmetic across several aggregates, and constant/aggregate ratios
    SELECT dept, provenance() FROM sales GROUP BY dept
    HAVING sum(revenue) > sum(cost);           -- agg vs agg
    SELECT dept, provenance() FROM sales GROUP BY dept
    HAVING sum(revenue) * sum(margin) > 1000;  -- product of aggregates

Constant arithmetic over a single aggregate is folded into the
comparison threshold (``sum(x) + 1 > 16`` becomes ``sum(x) > 15``,
flipping the operator for a negative multiplier); a distributive factor
is pushed into the aggregate where possible (``sum(x) * 2`` becomes a
clean aggregate over ``2*x``).  Comparisons that do not reduce to a
single aggregate versus a constant -- aggregate versus aggregate,
products of aggregates, a constant divided by an aggregate -- are
resolved by an exact possible-worlds enumeration that is generic over
every (m-)semiring, so ``sr_formula``, ``sr_why``, probabilities, and the
rest all see the same valid-world annotation.

Integer division follows SQL's truncation-toward-zero semantics rather
than real division: ``HAVING sum(x) / 2 = 5`` is true for a group whose
integer sum is ``10`` or ``11`` (both floor to ``5``), exactly as a plain
PostgreSQL ``sum(x) / 2`` would.  Writing ``sum(x) / 2.0`` instead opts
into real (numeric) division.

The ``choose`` Aggregate
-------------------------

The :sqlfunc:`choose` aggregate picks an arbitrary non-NULL value from a group.
It is particularly useful for modelling mutually exclusive choices
in a probabilistic setting: the provenance of the chosen value records
which input tuple was selected, enabling correct probability computation
over the choice.

.. code-block:: postgresql

    SELECT city, choose(position) AS sample_position
    FROM employees
    GROUP BY city;

Comparing an aggregate with a text constant
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A ``HAVING`` clause may compare a text-valued aggregate with a text
constant using ``=`` or ``<>``:

.. code-block:: postgresql

    SELECT city, provenance()
    FROM employees
    GROUP BY city
    HAVING choose(position ORDER BY name) = 'Analyst';

This is supported **only for** :sqlfunc:`choose`, which is *PICKFIRST*: in
any possible world its value is the first surviving occurrence of the
group. Because "first" depends on the order of the group's occurrences,
make the result deterministic with an explicit in-aggregate ordering,
``choose(col ORDER BY key)``; otherwise the physical scan order decides
which occurrence wins. ProvSQL tracks exactly the worlds whose first
occurrence (in that order) matches the constant. The provenance is
computed in a single linear scan of the group, as

.. math::

    \bigoplus_{i\,:\,v_i \text{ matches}} k_i \otimes
      \bigotimes_{j<i} (\mathbf{1} \ominus k_j),

i.e. occurrence :math:`i` is present and every earlier occurrence is
absent. This is exact even when the group's elements are **not** mutually
exclusive, and runs in :math:`O(N)` time per group (:math:`N` the group
size) for any m-semiring.

Comparing any other aggregate (``min``, ``max``, ``sum``…) with a text
constant is **not** implemented and raises an error, since its
possible-world value is not decided occurrence by occurrence.

CASE over aggregates
--------------------

A searched ``CASE`` whose ``WHEN`` guards are aggregate comparisons and whose
branches are aggregates is a **guarded selection over aggregates**: which branch
is taken depends on the (uncertain) input provenance, so the result is itself an
aggregate-carrier ``agg_token``.

.. code-block:: postgresql

    SELECT district,
           CASE WHEN sum(pm25) > 300 THEN max(pm25)
                WHEN avg(pm25) > 50   THEN avg(pm25)
                ELSE 0 END AS headline
    FROM readings GROUP BY district;

The result flows onward exactly like a bare aggregate: its cell displays as
``value (*)`` where the value is the ``CASE`` evaluated on the actual data
(the branch selected when every input tuple is present), and
:sqlfunc:`expected`, :sqlfunc:`variance`, and :sqlfunc:`moment` report the
distribution of the selected value over the possible worlds. Evaluation is
**exact** (no Monte Carlo -- correct even under
``SET provsql.rv_mc_samples = 0``):

.. math::

    E[\text{pick}^k \mid \text{defined}]
      = \frac{\sum_i \Pr(R_i \wedge d_i)\;
               E[\text{value}_i^k \mid R_i \wedge d_i]}
              {\sum_i \Pr(R_i \wedge d_i)},

summed over the first-match regions
:math:`R_i = \lnot g_1 \wedge \dots \wedge \lnot g_{i-1} \wedge g_i` (the
``ELSE`` region is "all guards false"), each conjoined with the branch's
*defined* event :math:`d_i` -- always true for ``sum`` / ``count`` and
constants (the empty group is the real value 0), "some contributing row
present" for ``min`` / ``max`` / ``avg``, which are ``NULL`` on an empty
group. The moment thus conditions on the ``CASE``'s value being defined,
the same convention as a bare ``min`` / ``max``, and is ``NULL`` only
when the value never is. The regions are mutually exclusive, and
the correlation between a guard and its branch (they share input tuples) is
carried by the conditioning, exactly as in ``HAVING``. This covers branches
that are a single aggregate (``sum`` / ``count`` / ``min`` / ``max``), a numeric
constant (``ELSE 0``), or a nested ``CASE``; an ``avg`` branch takes the
Monte-Carlo path (its exact arm, below, is unconditional-only).

A branch that is an **arithmetic combination** of aggregates
(``THEN sum(y) + sum(z)``) has no exact closed form -- the region probabilities
stay exact, but that branch's conditional moment is estimated by Monte Carlo, so
it needs ``provsql.rv_mc_samples > 0``. (This is the same limitation the moment
surface has for a bare ``sum(x) + sum(y)``.)

Joining and exploding aggregated provenance
--------------------------------------------

A column produced by an aggregate has the internal ``agg_token`` type.
Two facilities let such a column take part in further provenance-aware
processing.

A ``JOIN`` whose condition equates an ``agg_token`` column with an
ordinary (non-aggregate) column is rewritten automatically at plan time:
the aggregated relation is replaced by a subquery that *explodes* the
aggregate into one row per contributing child, recombining the child's
value and provenance, so the join then runs as a plain ``text = text``
comparison with provenance correctly propagated.

.. code-block:: postgresql

    -- agg.sample is an aggregate (agg_token) column; lookup.name is text
    SELECT agg.city, lookup.name, provenance()
    FROM (SELECT city, choose(position ORDER BY name) AS sample FROM employees GROUP BY city) agg
    JOIN lookup ON agg.sample = lookup.name;

The same explosion is available explicitly through the
:sqlfunc:`explode_table` function, which rewrites a stored table in place,
turning its ``agg_token`` column into one row per child with the matching
value and provenance:

.. code-block:: postgresql

    CREATE TABLE grouped AS
      SELECT city, choose(position ORDER BY name) AS sample FROM employees GROUP BY city;
    SELECT explode_table('grouped', 'sample');

Grouping Sets
--------------

`GROUPING SETS, CUBE, and ROLLUP
<https://www.postgresql.org/docs/current/queries-table-expressions.html#QUERIES-GROUPING-SETS>`_
are not supported.
