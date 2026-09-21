Aggregation and Grouping
=========================

ProvSQL supports provenance tracking for ``GROUP BY`` queries and aggregate
functions :cite:`DBLP:conf/pods/AmsterdamerDT11`. The semantics follow a
*semimodule* model: aggregation is treated as a scalar multiplication of
provenance values.

GROUP BY Queries
-----------------

When a query includes a ``GROUP BY`` clause, each aggregate expression
receives an ``agg`` gate in the provenance circuit (surfaced as the
``agg_token`` value of that cell). The children of this gate are
``semimod`` gates, each pairing a contributing row's value with that
row's provenance token. The group row's own :sqlfunc:`provenance` token
is a ``plus`` gate over the contributing tokens, wrapped in a ``delta``
gate marking the aggregation boundary:

.. code-block:: postgresql

    SELECT dept, COUNT(*), provenance()
    FROM employees
    GROUP BY dept;

The resulting provenance token encodes *which* input tuples were combined
to produce each aggregate value.

The value displayed for an aggregate (``3 (*)``) is the one plain SQL
computes on the same data, without provenance.  Some rows of a rewritten
query are kept only for the worlds where they exist, and are absent from
the database as it is: the null-padded row of an outer join for a row that
does have a match, a group that a ``HAVING`` rejects, a row beyond an
``ORDER BY … LIMIT``.  Such rows do not count in the displayed value; they
do in its provenance.  Whether a row holds in the database as it is, every
input tuple present, is :sqlfunc:`sr_boolean` without a mapping:

.. code-block:: postgresql

    SELECT e.name, p.project, sr_boolean(provenance())
    FROM employees e LEFT JOIN projects p ON p.lead = e.id;

``ORDER BY`` on an aggregate result sorts on that displayed value, so
the rows come in the order plain SQL gives them, each with its
provenance.  The order is that of the database as it is, not of each
world: a warning says so.  With a ``LIMIT``, the cut is therefore not
read per world either (see :ref:`the section on LIMIT <limit>`).

NULL inputs are skipped exactly as SQL prescribes: a NULL-valued row
contributes to ``count(*)`` but not to ``sum`` / ``avg`` / ``min`` /
``max`` or ``count(col)``, and an all-NULL group's aggregate is NULL --
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
return type (e.g., ``bigint`` for ``COUNT``, ``numeric`` for ``AVG``,
``boolean`` for ``bool_or`` read as a condition, as in
``CASE WHEN bool_or(x) THEN … END``).
The value is then evaluated as plain SQL, on the data as it is (see
:ref:`plain-sql`): the planner emits one warning for the statement, naming a
relation it tracks, and :ref:`provsql.implicit_freeze
<provsql-implicit-freeze>` set to ``'error'`` refuses the query. An explicit
cast (``cnt::numeric``) says that the plain value is meant: it still warns
that the provenance information is lost in the conversion, but it is never
refused. The provenance of the aggregate group itself is still tracked in
the ``provsql`` column.

Window functions over aggregate results (e.g. ``SUM(cnt) OVER ()``)
execute but are **not** provenance-aware: the aggregate argument is cast
back to its base type before the window computation, so the windowed
value is an opaque scalar and a ``WARNING`` is emitted. See
:ref:`window-aggregates` for the window functions that are tracked.

Random-Variable Aggregates
---------------------------

When the aggregated column has type ``random_variable``
(see :doc:`continuous-distributions`), the standard arithmetic
aggregates lift to the distribution algebra:
:sqlfunc:`sum`, :sqlfunc:`avg`, and
:sqlfunc:`product`, plus the order statistics ``min`` and ``max``.
Each returns a ``random_variable`` rather than a scalar, and each
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
:doc:`continuous-distributions`): the predicate is evaluated by
PostgreSQL on the surviving groups while ProvSQL still tracks the
per-group provenance.

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
into real (numeric) division.  The displayed value of such an expression in
the ``SELECT`` list follows the same rule: ``count(*) / 2`` shows ``1`` for
a count of 3.

Arithmetic whose result is a floating-point number (``real``, ``double
precision``) is tracked as well, and a widening cast the query writes to
reach it -- ``users.downvotes / CAST(count(posts.id) AS REAL)`` -- does not
stop the tracking: the operation is carried by the token and computed in
``numeric``, which subsumes that widening.  Two differences follow from
computing in ``numeric`` rather than in floating point.  The value differs
in its last digits from what the same expression prints without provenance
(``100 / CAST(count(*) AS REAL)`` over a count of 15 gives
``6.6666666666666667`` here and ``6.6666665`` there, ``real`` carrying
seven digits), so compare such values with a tolerance rather than
digit for digit.  And a division by an aggregate that the data as it is
makes zero has no value, ``NULL``, where plain PostgreSQL raises a
division-by-zero error and returns nothing at all: the row is kept, with
its provenance, and reading its value says only that this one world has
none.

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
ordered type -- text, date, timestamp... -- with a constant, by any
comparison: the value in a world only depends on the order of the values,
the type's own (under its default collation, for text).

.. code-block:: postgresql

    SELECT userid FROM badges GROUP BY userid
    HAVING min(name) = 'Enthusiast';

An ``array_agg`` compares with a constant array, or with another
``array_agg`` (the condition of a join on two aggregated arrays), using
``=`` or ``<>``: the worlds are those where the arrays, read in the
aggregate's input order, are equal.  A comparison of two aggregate results
that none of these covers -- ``min`` of a text column against another
``min``, say -- is refused when its probability is asked for.

It may compare :sqlfunc:`choose` of such a column with a constant using
``=`` or ``<>``:

.. code-block:: postgresql

    SELECT city, provenance()
    FROM employees
    GROUP BY city
    HAVING choose(position ORDER BY name) = 'Analyst';

Among the other aggregates of a text column, this is supported **only
for** :sqlfunc:`choose`, which is *PICKFIRST*: in
any possible world its value is the first surviving occurrence of the
group. Because "first" depends on the order of the group's occurrences,
make the result deterministic with an explicit in-aggregate ordering,
``choose(col ORDER BY key)``; otherwise the physical scan order decides
which occurrence wins. ProvSQL tracks exactly the worlds whose first
occurrence (in that order) matches the constant. In an absorptive
m-semiring whose :math:`\otimes` distributes over :math:`\ominus`
(Boolean, probabilities, tropical, Viterbi…) the provenance is computed
in a single linear scan of the group, as

.. math::

    \bigoplus_{i\,:\,v_i \text{ matches}}
      \bigl(\mathbf{1} \ominus \bigoplus_{j<i} k_j\bigr) \otimes k_i,

i.e. occurrence :math:`i` is present and every earlier occurrence is
absent; the worlds that differ only in the later occurrences collapse
into that one term. This is exact even when the group's elements are
**not** mutually exclusive, and runs in :math:`O(N)` time per group
(:math:`N` the group size). In other semirings (why-provenance,
``sr_formula``, counting, the security semiring…) the later occurrences
do not collapse, and ProvSQL enumerates the possible worlds of each
matching occurrence explicitly, which is exponential in the number of
occurrences that follow it.

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
constant (``ELSE 0``), or a nested ``CASE``; an ``avg`` branch is exact
when it depends on at most 20 input tuples (see
:ref:`provsql.rv_mc_samples <provsql-rv-mc-samples>`), and takes the
Monte-Carlo path otherwise.

A branch that is an **arithmetic combination** of aggregates
(``THEN sum(y) + sum(z)``) has no exact closed form -- the region probabilities
stay exact, but that branch's conditional moment is estimated by Monte Carlo
unless it depends on at most 20 input tuples, so it may need
``provsql.rv_mc_samples > 0``. (This is the same limitation the moment surface
has for a bare ``sum(x) + sum(y)``.)

``COALESCE(aggregate, constant)`` is tracked through the same gate, being the
``CASE`` it means:

.. code-block:: postgresql

    SELECT district, COALESCE(sum(pm25), 0) AS total
    FROM readings GROUP BY district;
    --  =  CASE WHEN sum(pm25) IS NOT NULL THEN sum(pm25) ELSE 0 END

The guard is the one ``HAVING sum(pm25) IS NOT NULL`` lowers to, so the value
is the aggregate in every world where a row it reads a value from is present,
and the constant in the worlds where the group exists without one (all its
rows null-valued, as the padded rows of an outer join are). Two arguments and
a default that holds no aggregate of its own -- a constant, a grouping column, an
expression over them, all of which are the same in every world. A default that
is itself an aggregate, or a third argument, leaves the ``COALESCE`` to be read
as a plain value.

``GREATEST`` and ``LEAST`` of an aggregate and such an expression are tracked
the same way, being the ``CASE`` they mean:

.. code-block:: postgresql

    SELECT district, GREATEST(sum(pm25), 2) AS floored
    FROM readings GROUP BY district;
    --  =  CASE WHEN sum(pm25) IS NULL THEN 2 WHEN 2 IS NULL THEN sum(pm25)
    --          WHEN sum(pm25) > 2 THEN sum(pm25) ELSE 2 END

The two ``NULL`` guards are SQL's reading of a ``NULL`` argument as *no value*
rather than as an unknown -- ``GREATEST(NULL, 2)`` is 2 -- which a bare
``CASE WHEN a > b`` would get wrong, its unknown comparison falling to the
``ELSE``. Two arguments, and an aggregate one has to be a kind whose
``NULL``-ness has a reading (``count``, ``sum``, ``avg``, ``min``, ``max``,
:sqlfunc:`choose`). ``NULLIF`` is the same kind of reading, ``NULLIF(a, b)`` being
``CASE WHEN a = b THEN NULL ELSE a END``, and is tracked where the compared
value holds no aggregate of its own:

.. code-block:: postgresql

    SELECT district, NULLIF(sum(pm25), 0) AS nonzero
    FROM readings GROUP BY district;
    --  =  CASE WHEN sum(pm25) = 0 THEN NULL ELSE sum(pm25) END

where a ``NULL`` sum makes the guard unknown, so the ``ELSE`` gives it back and
``NULLIF`` answers ``NULL`` there, as SQL does.

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

The other window functions still run, with a ``WARNING``: each row
keeps the provenance of its input row, and the value is an opaque
scalar. These are ``ntile``, ``percent_rank``, ``cume_dist``, the offset
functions (``lag``, ``lead``, ``first_value``, ``last_value``,
``nth_value``), ``ROWS`` and ``GROUPS`` frames with an offset, and the
windows over aggregate results other than the ranks below (an aggregate
over them).

.. _rank-over-aggregate:

Ranking the groups of an aggregation
-------------------------------------

``rank()``, ``dense_rank()`` and ``row_number()`` over an ``ORDER BY``
that reads an aggregate result -- the aggregates of the query's own
``GROUP BY``, or the aggregate columns of a subquery -- are tracked:

.. code-block:: postgresql

    SELECT tag, count(*) AS n, rank() OVER (ORDER BY count(*) DESC)
    FROM posttags GROUP BY tag;

The value ranked on varies between worlds, so the groups before a group
do too, and the frame machinery above, which reads the ordering values
of the database as it is, does not apply. The rank is read instead as
the number of groups that come before the group, itself included, which
compares the two aggregate results for each pair of groups: a group's
rank is then an aggregate result of its own, whose distribution
:sqlfunc:`expected` and the others report. ``ORDER BY`` an aggregate
with a ``LIMIT`` is the filter of that rank (see :ref:`limit`), so the
top ``k`` groups are those that are among the first ``k`` in a world.

``dense_rank()`` counts the distinct values instead of the groups, so it
reads the values of the aggregate as data: the aggregate is exploded into
one row per value it takes (see :ref:`explode-agg-value`), those values
are deduplicated over the whole relation, and the rank of a group counts
the ones up to its own value. The deduplication is done once, for every
partition at once, and only the counting is per row: this keeps it out of
a correlated subquery, which ProvSQL does not track when it groups rows
of its own. A ``dense_rank`` over an aggregate whose values cannot be
exploded, a ``sum()`` for instance, keeps the untracked window above,
with its ``WARNING``.

``ORDER BY`` on a window value sorts on its displayed value.

All the rows of a partition share the gate of a whole-partition window,
which is also the gate of the corresponding ``GROUP BY``. A frame that
moves with the current row has a gate per row, with as many children as
the frame has rows: the circuit of a running aggregate is quadratic in
the size of the partition.

.. _reaggregation:

Aggregates of aggregate results
-------------------------------

An aggregate of the aggregate results of a subquery is supported when it is
of the same kind: ``sum`` over a ``sum`` or a ``count``, ``max`` over a
``max``, ``min`` over a ``min``.  It is then the aggregate of the rows of
the groups, each contributing with the provenance of its group row times its
own, as semimodule scalar multiplication gives, in every semiring; and
``count`` over a ``count`` counts the groups:

.. code-block:: postgresql

    SELECT dept, sum(n) AS employees          -- = count(*) per dept
    FROM (SELECT dept, city, count(*) AS n FROM employees
          GROUP BY dept, city) t
    GROUP BY dept;

Any other aggregate of an aggregate result (``avg`` of a ``count``, ``max``
of a ``sum``, an aggregate of an arithmetic expression over aggregates) is
tracked in a second way: the contribution of each row carries the *gate of
the inner aggregate* rather than a value, so the value of the outer
aggregate is read in every possible world:

.. code-block:: postgresql

    SELECT avg(n) AS employees_per_city
    FROM (SELECT city, count(*) AS n FROM employees GROUP BY city) t;

The value displayed is the one plain SQL computes, on the data as it is, as
for any aggregate; what is read per world is what the probabilities and the
moments are computed over.  Such a value is no constant of the database, so
the closed forms do not apply to it: a probability or a moment over it is
computed by enumerating the possible worlds of the input tuples, which is
exact while they are few (``possible-worlds-aggregates``, see
:ref:`route-methods`), and estimated by sampling beyond that.  So
``expected(avg(n))`` is the average of the counts *of the cities present in
each world*, not the average of the counts the database happens to hold.

An aggregate that reads such a result in a ``FILTER``, an ``ORDER BY`` or a
``DISTINCT`` of its own, or whose inner value is not numeric, reads it on
the data as it is instead, and reports that reading once for the statement
(see :ref:`plain-sql`).

Functions of an aggregate result
--------------------------------

:sqlfunc:`round`, :sqlfunc:`floor`, :sqlfunc:`ceil` (and its synonym
:sqlfunc:`ceiling`), :sqlfunc:`abs`, ``ln``, ``exp`` and ``sqrt`` of an
aggregate result are carried as operations of its gate, so the value stays
tracked and is read in every possible world:

.. code-block:: postgresql

    SELECT city, round(avg(salary), 2) AS average
    FROM employees GROUP BY city;

The value displayed is the one plain SQL computes, as for any aggregate, and
a probability or a moment over it is computed per world: ``expected(floor(
avg(x)))`` is the average of the floors, which is not the floor of the
average. A comparison on such a value is read per world too, and the groups
where it can hold in no world are dropped, so the rows are SQL's.

Any other function reads the value of the aggregate on the data as it is and
reports that reading once for the statement (see :ref:`plain-sql`); an
explicit cast asks for it. A stored column of such an expression has type
``agg_token``, which has no order of its own -- its value is one per world --
so an ``ORDER BY`` on it outside the query that built it needs an explicit
cast, or a second column carrying the cast value.

.. _explode-agg-value:

Grouping by the value of an aggregate
--------------------------------------

The value of an aggregate is not one value of the database but one per
possible world, so grouping rows by it, or deduplicating on it, is no
operation on the data as it is.  ProvSQL reads it as the *explosion* of
the aggregate into one row per value it takes over the worlds, each
annotated by the condition that it takes that value:

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

The value is read the same way wherever it is read as data: from a
subquery, as above, or at the level of the aggregation itself.  A
``SELECT DISTINCT count(*) ... GROUP BY city`` computes its aggregation in
a subquery and deduplicates the values above it, and a ``UNION`` (non-ALL)
explodes the aggregate of each of its arms and deduplicates over the
values of all of them:

.. code-block:: postgresql

    SELECT count(*) FROM employees GROUP BY city
    UNION
    SELECT count(*) FROM employees WHERE remote GROUP BY city;

``EXCEPT`` and ``INTERSECT`` work the same way, and show why the explosion
is done in each arm rather than on the result of the set operation: a
difference matches the rows it removes on their values, so both arms have
to hold values of the database before they are compared.  A city counted
alike in both arms cancels, one counted differently does not:

.. code-block:: postgresql

    SELECT count(*) FROM employees GROUP BY city
    EXCEPT
    SELECT count(*) FROM employees WHERE remote GROUP BY city;

A set operation whose other arm aggregates nothing at that column is
refused: only the values of a whole column, exploded in every arm, are
matched together.  ``UNION ALL`` keeps every row and needs none of this.

An aggregate over exploded rows -- the ``count(*)`` above -- is an
aggregate over rows that are uncertain like any others: its displayed
value is the one of the database as it is, and :sqlfunc:`expected` and
the other moments are taken over the worlds where the row is.

The explosion applies to the aggregates whose values can be enumerated: a
``count()``, which takes every number of the rows it counts -- zero
included where a row it does not count, such as the null-padded row of an
outer join, can be the only one there -- a ``min()``, a ``max()`` or a
:sqlfunc:`choose`, which take one of the values they aggregate, and a
``sum()`` over an integer column, which takes one of its subset sums (equal
sums collapsing, so three rows of 1 give three values and not eight).

A ``sum()`` over a column that is not an integer is refused, and with it
``avg()``: the value of a summation is read back through the evaluator's own
arithmetic, that of a ``double``, and a subset sum of numbers that are not
integers is not the number that arithmetic reaches -- ``0.1 + 0.2``
comparing unequal to ``0.3`` would drop a row silently rather than report
anything.  A ``string_agg()`` takes one value per ordering.  Grouping by one
of those is refused with SQLSTATE ``0A000``, and the plain value, said
explicitly with a cast, groups as plain SQL does:

.. code-block:: postgresql

    SELECT total::numeric, count(*)        -- the plain value, not tracked
    FROM (SELECT city, sum(salary) AS total FROM employees GROUP BY city) t
    GROUP BY total::numeric;

``NULL`` is itself one of the values, for an aggregation over the whole table:
it gives its row in every world, including the world holding none of the rows
it reads, and its ``sum`` (``min``, ``max``, :sqlfunc:`choose`) is ``NULL``
there. That row is annotated by no row contributing -- the comparison
``count(x) = 0`` over the same argument -- so it is the answer of exactly the
worlds where SQL returns it. A grouped aggregation needs no such value: a group
without a row is no group, and its row is absent rather than ``NULL``. A
``count`` needs none either, being ``0`` rather than ``NULL`` over no row.

A ``NULL`` contribution is refused as well: whether the result is ``NULL``
is then a value of its own, which a comparison of the aggregate with a
value cannot express.  An aggregate of more than a thousand rows is
refused too, an explosion multiplying the rows by the number of values.

The explosion of an aggregate into the *values* it takes is not the one
of the next section, which explodes it into the rows it aggregates.

Reading the truth of a comparison
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A comparison of an aggregate against a constant has one truth per world in
the same way, so reading one in the select list, or as the condition of a
``CASE`` whose branches are not aggregates, explodes each row into the truths
it takes:

.. code-block:: postgresql

    SELECT city, count(*) > 5 AS crowded FROM employees GROUP BY city;
    SELECT city, CASE WHEN count(*) > 5 THEN 'crowded' ELSE 'quiet' END
    FROM employees GROUP BY city;

Each city gives the row where the comparison holds, annotated with the
provenance of it holding, and the row where it does not -- and, where the
aggregate can have no value, the third row of SQL's *unknown*, annotated with
the provenance of the group existing without a value to compare.  A row whose
truth holds in no world has probability zero, which is ProvSQL's reading of an
absent row, and the ones provably so are dropped outright.

The comparison is the condition the annotation carries, so no value of the
aggregate has to be enumerated: unlike grouping by the value, this works for
a ``sum()`` over any column and for an ``avg()``.  It applies to ``count``,
``sum``, ``avg``, ``min``, ``max`` and :sqlfunc:`choose` compared against a
constant, in a query that groups rows of its own.  A comparison between two
aggregates, one against a column, one over an aggregate whose ``NULL`` says
something else than "no value" (``stddev``, ``NULL`` over a single row), and a
scalar aggregation -- whose one row is there even in the world where the table
is empty, which no exploded row would be -- are read as plain values instead,
with the warning that says so.  A comparison in ``HAVING`` needs none of this:
it is already the provenance of the group.


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
comparison with provenance correctly propagated.  This reads the value of
the aggregate as one of the values it aggregates, which only holds for
:sqlfunc:`choose`: exploding the result of any other aggregate (a
``count``, whose rows each contribute 1) is refused.

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
