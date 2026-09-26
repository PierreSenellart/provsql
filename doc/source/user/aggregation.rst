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
computes on the same data, without provenance.  Some rows of a query are
kept only for the worlds where they exist, and are absent from the
database as it is: the null-padded row of an outer join for a row that
does have a match, a group that a ``HAVING`` rejects, a row beyond an
``ORDER BY … LIMIT``.  Such rows do not count in the displayed value; they
do in its provenance.  Whether a row holds in the database as it is, every
input tuple present, is :sqlfunc:`sr_boolean` without a mapping:

.. code-block:: postgresql

    SELECT e.name, p.project, sr_boolean(provenance())
    FROM employees e LEFT JOIN projects p ON p.lead = e.id;

``ORDER BY`` on an aggregate result sorts on that displayed value, so
the rows come in the order plain SQL gives them, each with its
provenance.  This is the order of the database as it is, not of each
world, and a warning says so.  With a ``LIMIT``, the cut is therefore not
read per world either (see :ref:`the section on LIMIT <limit>`).

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

``stddev``, ``variance`` and their ``_samp`` and ``_pop`` forms are supported
over an exact argument (an integer, a ``numeric``), where they are read as the
arithmetic that defines them,

.. code-block:: postgresql

    SELECT stddev(pm25) FROM readings;
    --  =  CASE WHEN count(pm25) <= 1 THEN NULL
    --          WHEN count(pm25) * sum(pm25*pm25) - sum(pm25)^2 = 0 THEN 0
    --          ELSE ((count(pm25) * sum(pm25*pm25) - sum(pm25)^2)
    --                / (count(pm25) * (count(pm25) - 1))) ^ 0.5 END

over the sums and the count, which are tracked. The value is the one
PostgreSQL computes, and it is read in every possible world like any other
arithmetic over aggregates. Over a floating-point argument, these aggregates
are read as a plain value on the data as it is, with a warning.

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
value of the aggregate's return type (e.g., ``bigint`` for ``COUNT``,
``numeric`` for ``AVG``, ``boolean`` for ``bool_or`` read as a condition,
as in ``CASE WHEN bool_or(x) THEN … END``), evaluated as plain SQL on the
data as it is (see :ref:`plain-sql`). ProvSQL then emits one warning for
the statement, naming a relation it tracks, and :ref:`provsql.implicit_freeze
<provsql-implicit-freeze>` set to ``'error'`` refuses the query. Marking the
part with :sqlfunc:`plain` says that the plain value is meant, and is the
only way to silence the warning. The provenance of the aggregate group
itself is still tracked in the ``provsql`` column.

A cast to a **number** is a function of the value, like ``round`` or
``abs``, and is tracked:

.. code-block:: postgresql

    SELECT SUM(salary)::numeric FROM employees;   -- tracked, the same number
    SELECT AVG(salary)::bigint  FROM employees;   -- tracked, rounded as the
                                                  -- cast itself rounds
    SELECT SUM(salary)::text    FROM employees;   -- the plain value, named

A widening (an integer to ``numeric``, to a float) keeps the value as it is,
and a narrowing to an integer rounds half away from zero, as PostgreSQL's
own cast does. A cast to ``text``, to a ``boolean`` or to a date reads the
plain value, and so does any cast of a text-valued aggregate.

A Boolean aggregate cast to an integer (``bool_or(flag)::int``, the only
cast SQL has on a Boolean) is tracked as the indicator it means:

.. code-block:: postgresql

    SELECT member, bool_or(role = 'admin')::int AS is_admin
    FROM membership GROUP BY member;
    --  =  CASE WHEN bool_or(…) = true  THEN 1
    --          WHEN bool_or(…) = false THEN 0 ELSE NULL END

It is 1 in the worlds where some row of the group satisfies the condition,
0 in those where the group exists and none does, and ``NULL`` where the
group is present with no value. Casts that PostgreSQL inserts on its own,
to line up the two arms of a ``GREATEST`` or an argument with a parameter,
are left as they are.

A column of such a tracked cast is still an ``agg_token``, and a query may
sort, group or take the ``DISTINCT`` of it. The order used is that of the
values on the data as it is, numbers compared as numbers and a result with
no value first. Since other possible worlds may order the values
differently, a warning is emitted once for the statement:

.. code-block:: text

    WARNING: ordering or grouping an aggregate result reads its value on the
    data as it is, the one this statement computed: another possible world
    need not order them the same way

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

Comparisons that do not reduce to a single aggregate against a constant
(aggregate against aggregate, products of aggregates, a constant divided
by an aggregate) are resolved by an exact enumeration of the possible
worlds, valid in every (m-)semiring.

Integer division follows SQL's truncation toward zero: ``HAVING sum(x) / 2
= 5`` is true for a group whose integer sum is ``10`` or ``11``, exactly as
a plain PostgreSQL ``sum(x) / 2`` would be; ``sum(x) / 2.0`` gives real
(numeric) division.  The displayed value of such an expression in the
``SELECT`` list follows the same rule: ``count(*) / 2`` shows ``1`` for a
count of 3.

Arithmetic whose result is a floating-point number (``real``, ``double
precision``) is tracked as well, including through a widening cast the
query writes to reach it (``users.downvotes / CAST(count(posts.id) AS
REAL)``). The value prints as SQL prints it in the type of the
expression: ``sum(x) / 3`` over a ``double precision`` column gives ``2``,
``100 / CAST(count(*) AS REAL)`` over fifteen rows gives
``6.666666666666667``, and ``(count(*) * max(x))::numeric`` over such a
column gives ``1.9``.  For the four basic operations, the value is the one
floating-point arithmetic gives; for ``^``, the last digit may differ.

A division by an aggregate that the data as it is makes zero has no value,
``NULL``, where plain PostgreSQL raises a division-by-zero error and returns
nothing at all: the row is kept, with its provenance, and reading its value
says only that this one world has none.

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
ordered type (text, date, timestamp…) with a constant, by any
comparison, using the type's own order (under its default collation, for
text).

.. code-block:: postgresql

    SELECT userid FROM badges GROUP BY userid
    HAVING min(name) = 'Enthusiast';

An ``array_agg`` compares with a constant array, or with another
``array_agg`` (the condition of a join on two aggregated arrays), using
``=`` or ``<>``: the worlds are those where the arrays, read in the
aggregate's input order, are equal.  A comparison of two aggregate results
that none of these covers (``min`` of a text column against another
``min``, say) is refused when its probability is asked for.

It may compare :sqlfunc:`choose` of such a column with a constant using
``=`` or ``<>``:

.. code-block:: postgresql

    SELECT city, provenance()
    FROM employees
    GROUP BY city
    HAVING choose(position ORDER BY name) = 'Analyst';

:sqlfunc:`choose` is *PICKFIRST*: in any possible world its value is the
first surviving occurrence of the group. Make the result deterministic
with an explicit in-aggregate ordering, ``choose(col ORDER BY key)``;
otherwise the physical scan order decides which occurrence wins. ProvSQL
tracks exactly the worlds whose first occurrence (in that order) matches
the constant, even when the group's elements are **not** mutually
exclusive. In an absorptive m-semiring whose :math:`\otimes` distributes
over :math:`\ominus` (Boolean, probabilities, tropical, Viterbi…) this
takes time linear in the size of the group. In other semirings
(why-provenance, ``sr_formula``, counting, the security semiring…),
ProvSQL enumerates the possible worlds of each matching occurrence, which
is exponential in the number of occurrences that follow it.

Comparing any other aggregate with a text constant is **not** implemented
and raises an error.

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

The result behaves like a bare aggregate: its cell displays as
``value (*)``, the ``CASE`` evaluated on the actual data (every input
tuple present), and :sqlfunc:`expected`, :sqlfunc:`variance`, and
:sqlfunc:`moment` report the distribution of the selected value over the
possible worlds. Evaluation is **exact** (no Monte Carlo, correct even
under ``SET provsql.rv_mc_samples = 0``). The moments are conditioned on
the value of the ``CASE`` being defined, as for a bare ``min`` / ``max``,
and are ``NULL`` only when it never is: a ``sum`` / ``count`` branch or a
constant is always defined (the empty group's sum is 0), a ``min`` /
``max`` / ``avg`` branch only when some contributing row is present.
This covers branches that are a single aggregate (``sum`` / ``count`` /
``min`` / ``max``), a numeric constant (``ELSE 0``), or a nested
``CASE``; an ``avg`` branch is exact when it depends on at most 20 input
tuples (see :ref:`provsql.rv_mc_samples <provsql-rv-mc-samples>`), and
estimated by Monte Carlo otherwise.

A branch that is an **arithmetic combination** of aggregates
(``THEN sum(y) + sum(z)``) has no exact closed form: the probability of
each branch stays exact, but that branch's conditional moment is estimated
by Monte Carlo unless it depends on at most 20 input tuples, so it may
need ``provsql.rv_mc_samples > 0``, as for a bare ``sum(x) + sum(y)``.

``COALESCE(aggregate, constant)`` is tracked in the same way, being the
``CASE`` it means:

.. code-block:: postgresql

    SELECT district, COALESCE(sum(pm25), 0) AS total
    FROM readings GROUP BY district;
    --  =  CASE WHEN sum(pm25) IS NOT NULL THEN sum(pm25) ELSE 0 END

The value is the aggregate in every world where a row it reads a value
from is present, and the constant in the worlds where the group exists
without one (all its rows null-valued, as the padded rows of an outer join
are). This requires two arguments and a default that holds no aggregate of
its own: a constant, a grouping column, or an expression over them. A
default that is itself an aggregate, or a third argument, makes the
``COALESCE`` read as a plain value.

``GREATEST`` and ``LEAST`` of an aggregate and such an expression are tracked
the same way, being the ``CASE`` they mean:

.. code-block:: postgresql

    SELECT district, GREATEST(sum(pm25), 2) AS floored
    FROM readings GROUP BY district;
    --  =  CASE WHEN sum(pm25) IS NULL THEN 2 WHEN 2 IS NULL THEN sum(pm25)
    --          WHEN sum(pm25) > 2 THEN sum(pm25) ELSE 2 END

The two ``NULL`` guards follow SQL, where a ``NULL`` argument means *no
value* (``GREATEST(NULL, 2)`` is 2). This requires two arguments, and an
argument that is not a plain value must be an aggregate that is NULL
exactly when it reads no value (``count``, ``sum``, ``avg``, ``min``,
``max``, :sqlfunc:`choose`, ``stddev_pop``, ``var_pop``, the bitwise and
Boolean ones), or arithmetic over such aggregates,
``GREATEST(sum(pm25) * 2, 9)``, which is NULL where one of its operands is.

.. _case-over-aggregates:

An argument may itself be one of these guarded expressions (a nested
``GREATEST``, a ``COALESCE`` inside a ``LEAST``), and so may the aggregate
side of a comparison, in a ``HAVING`` or in the guard of a ``CASE`` the query
writes:

.. code-block:: postgresql

    SELECT district FROM readings GROUP BY district
    HAVING GREATEST(sum(pm25), 2) > 3;

Such a comparison is evaluated exactly, as the comparisons of the arm
selected in each world.

``NULLIF`` is tracked in the same way, ``NULLIF(a, b)`` being
``CASE WHEN a = b THEN NULL ELSE a END``, where the compared value holds
no aggregate of its own:

.. code-block:: postgresql

    SELECT district, NULLIF(sum(pm25), 0) AS nonzero
    FROM readings GROUP BY district;
    --  =  CASE WHEN sum(pm25) = 0 THEN NULL ELSE sum(pm25) END

Where the sum is ``NULL``, the guard is unknown and ``NULLIF`` answers
``NULL``, as SQL does.

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

As for the aggregates of a grouped subquery, a comparison on the value
in an enclosing query goes into the provenance of the row. Here, the
probability of each row is that the employee is present and that at
most three present employees of the department, the employee included,
earn at least as much:

.. code-block:: postgresql

    SELECT name, probability_evaluate(provenance())
    FROM (SELECT name,
                 count(*) OVER (PARTITION BY dept ORDER BY salary DESC) AS k
          FROM employees) t
    WHERE k <= 3;

This requires the frame to be determined by the values of the rows, not
by their positions: in a world where some rows are absent, "the previous
row" is the previous row that is present, which differs from one world
to the next. The frames tracked are those of a window without
``ORDER BY``, ``RANGE`` frames (including the default frame of a window
with ``ORDER BY``, the rows up to the current one and its peers),
``GROUPS`` frames whose bounds are ``UNBOUNDED`` or ``CURRENT ROW``, and
``ROWS`` frames that span the whole partition, with any ``EXCLUDE``
clause. A frame that excludes the current row may be empty while the
row exists: a ``count`` is then 0, the other aggregates ``NULL``.

The ranking functions ``rank``, ``dense_rank`` and ``row_number`` are
tracked too (on PostgreSQL 11 and later): the rank of a row is one plus
the number of present rows strictly before it, its dense rank one plus
the number of distinct ordering values of these rows. Selecting the
first rows of each partition then gives each row the probability of
being among them:

.. code-block:: postgresql

    SELECT name, dept, probability_evaluate(provenance())
    FROM (SELECT name, dept,
                 rank() OVER (PARTITION BY dept ORDER BY salary DESC) AS rk
          FROM employees) t
    WHERE rk <= 3;

For probabilities, such a comparison of a rank with a constant is
evaluated without enumerating possible worlds, as a
``HAVING count(*) <= k`` is. ``ORDER BY … LIMIT k`` is read in the same way (see
:ref:`limit`). ``row_number`` is tracked as ``rank``,
which it equals when the ``ORDER BY`` of the window leaves no ties;
with ties, which SQL itself does not order, the value shown and tracked
is the rank, and a ``WARNING`` says so.

``cume_dist()`` and ``percent_rank()`` are tracked as well, as ratios of
counts: for ``cume_dist``, the rows up to the current row's peers over the
rows of the partition; for ``percent_rank``, the rows before the current
row's peers over the other rows of the partition (0 in a partition of one
row). Their values are read in every world.

The other window functions still run, with a ``WARNING``: each row
keeps the provenance of its input row, and the value is an opaque
scalar. These are ``ntile``, the offset
functions (``lag``, ``lead``, ``first_value``, ``last_value``,
``nth_value``), ``ROWS`` and ``GROUPS`` frames with an offset, and the
windows over aggregate results other than the ranks below (an aggregate
over them).

The circuit of a running aggregate, whose frame moves with the current
row, is quadratic in the size of the partition; a whole-partition window
costs the same as the corresponding ``GROUP BY``.

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
before it, itself included. It is an aggregate result of its own, whose
distribution :sqlfunc:`expected` and the others report. ``ORDER BY`` an
aggregate with a ``LIMIT`` is the filter of that rank (see :ref:`limit`),
so the top ``k`` groups are those that are among the first ``k`` in a
world.

``dense_rank()`` counts the distinct values instead of the groups, and
requires an aggregate whose values can be enumerated (see
:ref:`explode-agg-value`). A ``dense_rank`` over an aggregate whose values
cannot be enumerated, a ``sum()`` for instance, is an untracked window,
with its ``WARNING``.

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

The value displayed is the one plain SQL computes, on the data as it is,
as for any aggregate. A probability or a moment over it is computed by
enumerating the possible worlds of the input tuples, which is exact while
they are few (``possible-worlds-aggregates``, see :ref:`route-methods`),
and estimated by sampling beyond that.  So ``expected(avg(n))`` is the
average of the counts *of the cities present in each world*, not the
average of the counts the database happens to hold.

An aggregate that reads such a result in a ``FILTER``, an ``ORDER BY`` or a
``DISTINCT`` of its own, or whose inner value is not numeric, reads it on
the data as it is instead, and reports that reading once for the statement
(see :ref:`plain-sql`).

Functions of an aggregate result
--------------------------------

``round``, ``floor``, ``ceil`` (and its synonym ``ceiling``), ``abs``,
``ln``, ``exp``, ``sqrt`` and ``power`` (and its synonym ``pow``, with an
exponent that is not itself an aggregate result) of an aggregate result
are tracked, their value read in every possible world:

.. code-block:: postgresql

    SELECT city, round(avg(salary), 2) AS average
    FROM employees GROUP BY city;

The value displayed is the one plain SQL computes, as for any aggregate, and
a probability or a moment over it is computed per world: ``expected(floor(
avg(x)))`` is the average of the floors, which is not the floor of the
average. A comparison on such a value is read per world too, and in a
``HAVING`` the groups where it can hold in no world are dropped, so the rows
are SQL's. Outside the aggregation (a ``WHERE`` on a derived table's
aggregate, a scalar subquery), such a row is kept with a provenance of
zero, which says that no world holds it.

Any other function reads the value of the aggregate on the data as it is and
reports that reading once for the statement (see :ref:`plain-sql`); an
explicit cast asks for it. A stored column of such an expression has type
``agg_token``; an ``ORDER BY`` on it sorts on its value on the data as it is,
with a warning, since another world need not order the rows the same way.

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
value, with the provenance of counting that many; the two are mutually
exclusive, and exactly one of them is there in each world where the city
is.  Grouping, deduplicating or uniting on the column is then an
operation on data like any other, and the row of a value has the
probability that some group takes it.

This holds wherever the value is read as data: from a subquery, as above,
or at the level of the aggregation itself.  A ``SELECT DISTINCT count(*)
… GROUP BY city`` deduplicates the values, and a ``UNION`` (non-ALL)
explodes the aggregate of each of its arms and deduplicates over the
values of all of them:

.. code-block:: postgresql

    SELECT count(*) FROM employees GROUP BY city
    UNION
    SELECT count(*) FROM employees WHERE remote GROUP BY city;

``EXCEPT`` and ``INTERSECT`` work the same way, matching the values of
both arms.  A city counted alike in both arms cancels, one counted
differently does not:

.. code-block:: postgresql

    SELECT count(*) FROM employees GROUP BY city
    EXCEPT
    SELECT count(*) FROM employees WHERE remote GROUP BY city;

A set operation whose other arm aggregates nothing at that column is
refused: only the values of a whole column, exploded in every arm, are
matched together.  ``UNION ALL`` keeps every row and needs none of this.

An aggregate over exploded rows (the ``count(*)`` above) is an aggregate
over rows that are uncertain like any others: its displayed value is the
one of the database as it is, and :sqlfunc:`expected` and the other
moments are taken over the worlds where the row is.

The explosion applies to the aggregates whose values can be enumerated:
``count()``, which takes every number of the rows it counts (zero included,
where a row it does not count, such as the null-padded row of an outer
join, can be the only one there); ``min()``, ``max()`` and
:sqlfunc:`choose`, which take one of the values they aggregate; and
``sum()`` over an integer column, which takes one of its subset sums
(equal sums collapsing, so three rows of 1 give three values and not
eight).

A ``sum()`` over a column that is not an integer is refused, and with it
``avg()``, since subset sums of such numbers cannot be compared exactly
(``0.1 + 0.2`` compares unequal to ``0.3`` in floating point). So is a
``string_agg()``, which takes one value per ordering.  Such a grouping
raises an error with SQLSTATE ``0A000``, and the plain value, asked for
explicitly with a cast, groups as plain SQL does:

.. code-block:: postgresql

    SELECT total::numeric, count(*)        -- the plain value, not tracked
    FROM (SELECT city, sum(salary) AS total FROM employees GROUP BY city) t
    GROUP BY total::numeric;

``NULL`` is itself one of the values, for an aggregation over the whole
table: its row is there in every world, including the world holding none
of the rows it reads, where its ``sum`` (``min``, ``max``,
:sqlfunc:`choose`) is ``NULL``. The ``NULL`` value is then annotated by no
row contributing, so it is the answer of exactly the worlds where SQL
returns it. A grouped aggregation has no such value, since a group without
a row is absent, not ``NULL``; neither does a ``count``, which is ``0``
over no row.

A ``NULL`` contribution is refused, as whether the result is ``NULL`` is
then a value of its own, which a comparison of the aggregate with a value
cannot express.  An aggregate of more than a thousand rows is refused too,
an explosion multiplying the rows by the number of values.

This explosion into the *values* an aggregate takes differs from the one
of `Joining and exploding aggregated provenance`_, into the rows it
aggregates.

Reading the truth of a comparison
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A comparison of an aggregate against a constant, or against a column of
its group (a target to meet), has one truth value per world in the same
way, so reading one in the select list, or as the
condition of a ``CASE`` whose branches are not aggregates, explodes each
row into the truth values it takes:

.. code-block:: postgresql

    SELECT city, count(*) > 5 AS crowded FROM employees GROUP BY city;
    SELECT city, CASE WHEN count(*) > 5 THEN 'crowded' ELSE 'quiet' END
    FROM employees GROUP BY city;

Each city gives the row where the comparison holds, annotated with the
provenance of it holding, and the row where it does not; where the
aggregate can have no value, a third row for SQL's *unknown* is annotated
with the provenance of the group existing without a value to compare.  A
row whose truth holds in no world has probability zero, which is
ProvSQL's reading of an absent row, and the ones provably so are dropped.

``IS NULL`` and ``IS NOT NULL`` of an expression over aggregates are read the
same way, the value they test being null in some worlds and not in others:

.. code-block:: postgresql

    SELECT city, sum(salary) IS NULL AS no_salary
    FROM employees GROUP BY city;

This gives two rows, not three, since a value either is null in a world
or is not. A division is refused here: it is null where its divisor reads
zero, which no operand's nullness tells.

No value of the aggregate has to be enumerated, so unlike grouping by the
value, this works for a ``sum()`` over any column and for an ``avg()``. It
applies to ``count``, ``sum``, ``avg``, ``min``, ``max`` and
:sqlfunc:`choose` compared against a constant or an expression over the
grouping columns, with or without a ``GROUP BY``; a column can be ``NULL``,
and then the comparison is unknown, in the row of its own. An aggregation without one has its row even in the world where the
table is empty, in the row of the truth value the comparison has there
(``count(*) > 1`` is false over no row, ``sum(x) > 1`` unknown).  The
following are read as plain values instead, with a warning: a comparison
between two aggregates, one over an aggregate whose
``NULL`` means something other than "no value" (``stddev``, ``NULL`` over
a single row), and one in a query that also computes a window function.
None of this applies in a ``HAVING`` condition, which is already the
provenance of the group, nor to a sort key, which orders the rows on the
data as it is, as an ``ORDER BY`` on the value of an aggregate does.


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
