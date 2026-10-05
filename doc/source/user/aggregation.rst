Aggregation and Grouping
=========================

ProvSQL supports provenance tracking for ``GROUP BY`` queries and aggregate
functions :cite:`DBLP:conf/pods/AmsterdamerDT11`. The semantics follow a
*semimodule* model: aggregation is treated as a scalar multiplication of
provenance values.

GROUP BY Queries
-----------------

When a query includes a ``GROUP BY`` clause, each aggregate value is an
``agg_token``, which pairs the value of each contributing row with that
row's provenance token. The group row's own :sqlfunc:`provenance` token
combines the tokens of the contributing rows:

.. code-block:: postgresql

    SELECT dept, COUNT(*), provenance()
    FROM employees
    GROUP BY dept;

The resulting provenance token encodes *which* input tuples were combined
to produce each aggregate value.

The value displayed for an aggregate (``3 (*)``) is the one plain SQL
computes on the same data. Rows kept only for the worlds where they exist
(the null-padded row of an outer join for a row that has a match, a group
that a ``HAVING`` rejects, a row beyond an ``ORDER BY … LIMIT``) do not
count in it; they do in its provenance. :sqlfunc:`present` tells whether
a row holds in the actual database:

.. code-block:: postgresql

    SELECT e.name, p.project, present(provenance())
    FROM employees e LEFT JOIN projects p ON p.lead = e.id;

``ORDER BY`` on an aggregate result sorts on that displayed value, i.e.,
in the order of the actual database, not of each world, with a warning.
A ``LIMIT`` after it is still read in every world (see :ref:`limit`).

NULL inputs are skipped exactly as SQL prescribes: a NULL-valued row
contributes to ``count(*)`` but not to ``sum`` / ``avg`` / ``min`` /
``max`` or ``count(col)``, and an all-NULL group's aggregate is NULL,
including across possible worlds in ``HAVING`` (see :doc:`the chapter on
NULLs <nulls>`).

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

``stddev``, ``variance`` and their ``_samp`` and ``_pop`` forms are tracked
over an exact argument (an integer, a ``numeric``), as the arithmetic over
sums and counts that defines them. Over a floating-point argument, they are
read as a plain value, with a warning.

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

An operation that ProvSQL does not track reads the aggregate result as a
plain value of the aggregate's return type (e.g., ``bigint`` for ``COUNT``),
without provenance, with one warning for the statement (see
:ref:`plain-sql`); :ref:`provsql.implicit_freeze <provsql-implicit-freeze>`
set to ``'error'`` refuses the query instead. Wrap the part in
:sqlfunc:`plain` to say that the plain value is meant and silence the
warning. The provenance of the group itself is still tracked.

A cast to a number is tracked, rounding as PostgreSQL's cast does; a cast
to ``text``, ``boolean`` or a date, and any cast of a text-valued
aggregate, reads the plain value:

.. code-block:: postgresql

    SELECT SUM(salary)::numeric FROM employees;   -- tracked
    SELECT AVG(salary)::bigint  FROM employees;   -- tracked, rounded
    SELECT SUM(salary)::text    FROM employees;   -- the plain value

A Boolean aggregate cast to an integer (``bool_or(flag)::int``) is tracked
as an indicator: 1 in the worlds where some row of the group satisfies the
condition, 0 where none does.

``ORDER BY`` on such a tracked expression sorts on the value shown, with a
warning, since other worlds may order the values differently.

Window functions over aggregate results (e.g., ``SUM(cnt) OVER ()``)
execute but are **not** provenance-aware: the windowed value is an opaque
scalar, and a ``WARNING`` is emitted. See :ref:`window-aggregates` for the
window functions that are tracked.

Random-Variable Aggregates
---------------------------

When the aggregated column has type ``random_variable``
(see :doc:`continuous-distributions`), the standard arithmetic
aggregates lift to the distribution algebra:
:sqlfunc:`sum`, :sqlfunc:`avg`, and
:sqlfunc:`product`, plus the order statistics ``min`` and ``max``.
Each returns a ``random_variable`` instead of a scalar, and each
reports an empty group as SQL ``NULL``, as the standard-SQL
aggregates do.
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

A group is not filtered on the current data: one that fails the predicate
may still appear in the result, with a provenance that evaluates to zero
where the predicate fails.

``HAVING`` clauses whose outcome is a deterministic scalar are also
supported, including conditions that wrap a ``random_variable``
aggregate in a moment function such as
``HAVING expected(avg(measurement)) > 20`` (see
:doc:`continuous-distributions`): PostgreSQL evaluates the predicate
on the surviving groups while ProvSQL still tracks the per-group
provenance.

A ``random_variable`` aggregate can also be compared directly, as in
``HAVING sum(measurement) > 40``. The outcome is then uncertain, and
goes into the provenance of the group: its probability is that of the
group existing and the comparison holding. Such a comparison cannot be
combined, in one ``HAVING`` clause, with a comparison on an ordinary
aggregate (``count(*) > 2``).

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

Integer division truncates toward zero, as in SQL: ``HAVING sum(x) / 2 =
5`` holds for an integer sum of ``10`` or ``11``; write ``sum(x) / 2.0``
for numeric division. Arithmetic whose result is a floating-point number
is tracked too. The displayed value of such an expression is the one SQL
prints in its type (``count(*) / 2`` shows ``1`` for a count of 3).

A division by an aggregate that is zero in the actual database shows
``NULL``, where plain PostgreSQL raises a division-by-zero error; the row
is kept, with its provenance.

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

A ``HAVING`` clause may compare ``min`` or ``max`` of a column of any
ordered type (text, date, timestamp…) with a constant, by any comparison,
using the type's own order:

.. code-block:: postgresql

    SELECT userid FROM badges GROUP BY userid
    HAVING min(name) = 'Enthusiast';

It may compare an ``array_agg`` with a constant array or with another
``array_agg``, using ``=`` or ``<>`` (the arrays read in the aggregate's
input order), and :sqlfunc:`choose` with a constant, using ``=`` or
``<>``:

.. code-block:: postgresql

    SELECT city, provenance()
    FROM employees
    GROUP BY city
    HAVING choose(position ORDER BY name) = 'Analyst';

:sqlfunc:`choose` is *PICKFIRST*: in each possible world its value is the
first present occurrence of the group, so write ``choose(col ORDER BY
key)`` to make the result deterministic; otherwise the physical scan
order decides. The group's elements need not be mutually exclusive.

Comparing any other aggregate with a text constant raises an error, and a
comparison of two aggregate results that none of these cases covers is
refused when its probability is asked for.

CASE over aggregates
--------------------

A searched ``CASE`` whose ``WHEN`` guards are aggregate comparisons and whose
branches are aggregates is a **guarded selection over aggregates**: which branch
is taken depends on the (uncertain) input provenance, so the result is itself an
``agg_token``.

.. code-block:: postgresql

    SELECT district,
           CASE WHEN sum(pm25) > 300 THEN max(pm25)
                WHEN avg(pm25) > 50   THEN avg(pm25)
                ELSE 0 END AS headline
    FROM readings GROUP BY district;

The result behaves like a bare aggregate: it displays as ``value (*)``,
the ``CASE`` evaluated on the actual data, and :sqlfunc:`expected`,
:sqlfunc:`variance` and :sqlfunc:`moment` give the distribution of the
selected value over the possible worlds, conditioned on it being defined.
This is exact for branches that are a single ``sum``, ``count``, ``min``
or ``max``, a numeric constant or a nested ``CASE``. An ``avg`` branch, or
an arithmetic combination of aggregates (``THEN sum(y) + sum(z)``), is
exact when it depends on at most 20 input tuples, and estimated by Monte
Carlo otherwise (see :ref:`provsql.rv_mc_samples
<provsql-rv-mc-samples>`).

``COALESCE(aggregate, default)`` is tracked in the same way, as the
``CASE`` it means, when it has two arguments and the default holds no
aggregate (a constant, a grouping column, or an expression over them);
otherwise it is read as a plain value:

.. code-block:: postgresql

    SELECT district, COALESCE(sum(pm25), 0) AS total
    FROM readings GROUP BY district;

So are ``GREATEST`` and ``LEAST`` of two arguments, such as
``GREATEST(sum(pm25), 2)``, where an argument that is not a plain value
must be an aggregate that is ``NULL`` exactly when it reads no value
(``count``, ``sum``, ``avg``, ``min``, ``max``, :sqlfunc:`choose`,
``stddev_pop``, ``var_pop``, the bitwise and Boolean ones), or arithmetic
over such aggregates; and ``NULLIF(aggregate, value)``, when the compared
value holds no aggregate.

.. _case-over-aggregates:

These guarded expressions may be nested, and compared in a ``HAVING`` or
in the guard of a ``CASE``, exactly:

.. code-block:: postgresql

    SELECT district FROM readings GROUP BY district
    HAVING GREATEST(sum(pm25), 2) > 3;

.. _window-aggregates:

Aggregates as Window Functions
-------------------------------

An aggregate used as a window function, ``f(x) OVER (…)``, is tracked
as the aggregate of a group is. Each output row keeps the provenance of
its input row, and the value becomes an ``agg_token`` over the rows of
the frame: in each possible world, it is ``f`` applied to the rows of
the frame that are present in that world. ``FILTER`` clauses and the
aggregates of `Aggregate Functions`_ are supported.

.. code-block:: postgresql

    SELECT name, dept, salary,
           sum(salary) OVER (PARTITION BY dept) AS dept_total,
           count(*) OVER (PARTITION BY dept ORDER BY salary DESC) AS at_least_as_paid
    FROM employees;

A comparison on the value in an enclosing query goes into the provenance
of the row, as for the aggregates of a grouped subquery.

The frame must be determined by the values of the rows, not by their
positions: a window without ``ORDER BY``, a ``RANGE`` frame (including the
default frame of a window with ``ORDER BY``), a ``GROUPS`` frame bounded by
``UNBOUNDED`` or ``CURRENT ROW``, or a ``ROWS`` frame spanning the whole
partition, with any ``EXCLUDE`` clause.

The ranking functions ``rank``, ``dense_rank`` and ``row_number`` are
tracked too (on PostgreSQL 11 and later). Selecting the first rows of each
partition gives each row the probability of being among them:

.. code-block:: postgresql

    SELECT name, dept, probability_evaluate(provenance())
    FROM (SELECT name, dept,
                 rank() OVER (PARTITION BY dept ORDER BY salary DESC) AS rk
          FROM employees) t
    WHERE rk <= 3;

``row_number`` is tracked as ``rank``; where the ``ORDER BY`` of the window
leaves ties, the value is the rank, and a ``WARNING`` says so.
``cume_dist()``, ``percent_rank()`` and ``ntile()`` are tracked as well.
Rows that tie on the ``ORDER BY`` of an ``ntile`` share the bucket of their
rank, where SQL may split them between two buckets; a ``WARNING`` says so
when it does.

The other window functions run with a ``WARNING``, their value untracked:
``lag``, ``lead``, ``first_value``, ``last_value``, ``nth_value``,
``ROWS`` and ``GROUPS`` frames with an offset, and windows over aggregate
results other than the ranks below.

The circuit of a running aggregate, whose frame moves with the current
row, is quadratic in the size of the partition.

.. _rank-over-aggregate:

Ranking the groups of an aggregation
-------------------------------------

``rank()``, ``dense_rank()`` and ``row_number()`` over an ``ORDER BY``
that reads an aggregate result (the aggregates of the query's own
``GROUP BY``, or the aggregate columns of a subquery) are tracked:

.. code-block:: postgresql

    SELECT tag, count(*) AS n, rank() OVER (ORDER BY count(*) DESC)
    FROM posttags GROUP BY tag;

In each world, the rank of a group is the number of groups that come
before it, itself included, and :sqlfunc:`expected` and the other moments
report its distribution. ``ORDER BY`` an aggregate with a ``LIMIT`` keeps
the groups that are among the first ``k`` in a world (see :ref:`limit`).

``dense_rank()`` requires an aggregate whose values can be enumerated (see
:ref:`explode-agg-value`); over another one, a ``sum()`` for instance, it
is an untracked window, with a ``WARNING``.

``ORDER BY`` on a window value sorts on its displayed value.

.. _reaggregation:

Aggregates of aggregate results
-------------------------------

An aggregate of the aggregate results of a subquery is supported when it is
of the same kind: ``sum`` over a ``sum`` or a ``count``, ``max`` over a
``max``, ``min`` over a ``min``.  It is then the aggregate of the rows of
the groups, in every semiring; and ``count`` over a ``count`` counts the
groups:

.. code-block:: postgresql

    SELECT dept, sum(n) AS employees          -- = count(*) per dept
    FROM (SELECT dept, city, count(*) AS n FROM employees
          GROUP BY dept, city) t
    GROUP BY dept;

Any other aggregate of an aggregate result (``avg`` of a ``count``, ``max``
of a ``sum``, an aggregate of an arithmetic expression over aggregates) is
also tracked, its value read in every possible world:

.. code-block:: postgresql

    SELECT avg(n) AS employees_per_city
    FROM (SELECT city, count(*) AS n FROM employees GROUP BY city) t;

A probability or a moment over it is read in the possible worlds: so
``expected(avg(n))`` is the average of the counts *of the cities present
in each world*. It is exact over few input tuples, and estimated by
sampling beyond that (see :ref:`route-methods`).

An aggregate that reads such a result in a ``FILTER``, an ``ORDER BY`` or a
``DISTINCT`` of its own, or whose inner value is not numeric, reads it as a
plain value, with a warning (see :ref:`plain-sql`).

Functions of an aggregate result
--------------------------------

``round``, ``floor``, ``ceil`` (and its synonym ``ceiling``), ``abs``,
``ln``, ``exp``, ``sqrt`` and ``power`` (and its synonym ``pow``, with an
exponent that is not itself an aggregate result) of an aggregate result
are tracked, their value read in every possible world:

.. code-block:: postgresql

    SELECT city, round(avg(salary), 2) AS average
    FROM employees GROUP BY city;

A probability or a moment over the result is computed per world:
``expected(floor(avg(x)))`` is the average of the floors, not the floor
of the average. A comparison on it is read per world too: in a ``HAVING``,
the groups where it holds in no world are dropped; outside the aggregation
(a ``WHERE`` on a derived table's aggregate), such a row is kept with a
provenance of zero.

Any other function reads the aggregate result as a plain value, with a
warning (see :ref:`plain-sql`).

.. _explode-agg-value:

Grouping by the value of an aggregate
--------------------------------------

The value of an aggregate is one per possible world, so grouping rows by
it, or deduplicating on it, reads it as its *explosion*: one row per value
it takes over the worlds, each annotated by the condition that it takes
that value:

.. code-block:: postgresql

    SELECT n, count(*) AS cities          -- how many cities have n employees
    FROM (SELECT city, count(*) AS n FROM employees GROUP BY city) t
    GROUP BY n;

A city whose count is 1 or 2 over the worlds gives two rows, one per
value, mutually exclusive, each with the provenance of counting that many;
the row of a value has the probability that some group takes it. The same
holds for a ``SELECT DISTINCT`` on the value at the level of the
aggregation itself, and for ``UNION``, ``EXCEPT`` and ``INTERSECT``, which
match the values of their arms:

.. code-block:: postgresql

    SELECT count(*) FROM employees GROUP BY city
    UNION
    SELECT count(*) FROM employees WHERE remote GROUP BY city;

A set operation is refused where one arm aggregates at that column and
another does not.

This applies to ``count()``, ``min()``, ``max()``, :sqlfunc:`choose`, and
``sum()`` over an integer column. Other aggregates (``sum()`` over a
non-integer column, ``avg()``, ``string_agg()``…) are refused with SQLSTATE
``0A000``; group by their value shown with :sqlfunc:`plain` instead:

.. code-block:: postgresql

    SELECT plain(total), count(*)
    FROM (SELECT city, sum(salary) AS total FROM employees GROUP BY city) t
    GROUP BY plain(total);

For an aggregation without ``GROUP BY``, ``NULL`` is one of the values,
taken in the worlds where no row it reads is present (except for
``count``, which is then ``0``). An aggregate with a ``NULL`` contribution,
or over more than a thousand rows, is refused.

This explosion into the *values* an aggregate takes differs from the one
of `Joining and exploding aggregated provenance`_, into the rows it
aggregates.

Reading the truth of a comparison
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A comparison of an aggregate against a constant, or against an expression
over the grouping columns, read in the select list or as the condition of
a ``CASE`` whose branches are not aggregates, explodes each row into the
truth values it takes:

.. code-block:: postgresql

    SELECT city, count(*) > 5 AS crowded FROM employees GROUP BY city;

Each city gives a row where the comparison holds and a row where it does
not, each annotated with the provenance of that truth value, and, where the
aggregate can have no value, a third row for SQL's *unknown*. ``IS NULL``
and ``IS NOT NULL`` of an expression over aggregates give two rows the same
way (a division is refused there).

This works for ``count``, ``sum``, ``avg``, ``min``, ``max`` and
:sqlfunc:`choose`, with or without ``GROUP BY``. A comparison between two
aggregates, one over an aggregate whose ``NULL`` does not mean "no value"
(``stddev``), and one in a query that also computes a window function are
read as plain values, with a warning. A ``HAVING`` condition is not
exploded: it is already the provenance of the group.


Joining and exploding aggregated provenance
--------------------------------------------

A column produced by an aggregate has the ``agg_token`` type. Two
facilities let such a column take part in further provenance-aware
processing.

A ``JOIN`` whose condition equates an ``agg_token`` column with an
ordinary (non-aggregate) column is supported: the aggregate is exploded
into one row per contributing row, with its value and provenance, and the
join compares these values.  This reads the value of the aggregate as one
of the values it aggregates, which only holds for :sqlfunc:`choose`:
exploding the result of any other aggregate (a ``count``, whose rows each
contribute 1) is refused.

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
are read as the ``UNION ALL`` of one ``GROUP BY`` per grouping set: each
group has the provenance it has in that ``GROUP BY``, and the empty set
``()`` is an aggregation without ``GROUP BY``, whose single row is always
there.
