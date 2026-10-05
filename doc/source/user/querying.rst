Querying with Provenance
=========================

Once provenance is enabled on one or more tables, ProvSQL transparently
rewrites every SQL query to propagate and combine provenance annotations.
No changes to query syntax are required.

How It Works
-------------

ProvSQL installs a PostgreSQL *planner hook*, which is why it must be listed
in ``shared_preload_libraries``. When a query involves a provenance-enabled
table, ProvSQL rewrites it before execution:

1. Identifies all relations carrying a ``provsql`` column.
2. Builds a provenance expression that combines the input tokens using the
   appropriate semiring operations (``plus`` for alternative use of
   tuples such as in duplicate elimination,
   ``times`` for combined use of tuples such as in joins, ``monus`` for difference).
3. Appends the resulting provenance token to the output as an extra column.

The ``provsql`` column that ``*`` expands to over a tracked table takes
that same last place: in ``SELECT *, a + 1 AS e FROM t`` the columns
are those of ``t``, then ``e``, then ``provsql``.  A position in ``ORDER BY``
counts the columns in that order, and so does the column list of a view or
of a ``CREATE TABLE AS`` over ``SELECT *``.

A whole row of a tracked table read as any row -- ``SELECT t FROM t``,
``row_to_json(t)``, ``json_agg(t)``, ``t::text``, ``ROW(t.*)`` -- has its
columns other than ``provsql``, as on the untracked table.  So do rows
compared (``t = u``, ``t IN (SELECT u FROM u)``, ``t IS DISTINCT FROM u``,
``CASE t WHEN u``, ``ROW(t.*) = ROW(u.*)``) or grouped (``GROUP BY t``): two
rows equal on those columns are equal, whatever their tokens.  Where the
table's own row type is needed (``ROW(t.*)::t``, a function declared on
it, a column of a table created from the row), the row keeps it.

The provenance token in each output row is a UUID that identifies a gate
in a *provenance circuit*, a DAG recording how that result was derived.

Supported SQL Features
-----------------------

The following SQL constructs are supported with full provenance tracking:

* ``SELECT … FROM … WHERE``, with multiset semantics
* Joins: inner, outer and natural joins, ``LATERAL`` subqueries
* Subqueries in ``FROM``, and outside it (``EXISTS``, ``IN``, quantified
  comparisons such as ``= ANY``, scalar subqueries, ``ARRAY(SELECT …)``),
  correlated or not (see :ref:`subqueries`)
* ``WITH`` clauses, and ``WITH RECURSIVE`` on PostgreSQL 15+ (see
  :ref:`recursive-queries`)
* ``GROUP BY``, with ``GROUPING SETS``, ``ROLLUP`` and ``CUBE``
* Aggregation (``SUM``, ``COUNT``, ``MIN``, ``MAX``, ``AVG``,
  ``COUNT(DISTINCT …)``, ``string_agg``, ``array_agg``, …), ``FILTER``
  and ``HAVING`` (see :doc:`aggregation`)
* Window functions: aggregates over a frame determined by values, ``rank``,
  ``dense_rank``, ``row_number``, ``ntile``, ``percent_rank``,
  ``cume_dist`` (see :ref:`window-aggregates`)
* ``SELECT DISTINCT`` and ``DISTINCT ON``
* ``ORDER BY … LIMIT`` / ``FETCH`` / ``OFFSET`` (see :ref:`limit`)
* ``UNION``, ``UNION ALL``, ``EXCEPT``, ``INTERSECT``
* ``VALUES`` tables, whose rows have no provenance
* ``EXISTS (…)`` in the ``SELECT`` list, as a Boolean value (see
  :ref:`explode-agg-value`)
* ``CREATE TABLE … AS``, ``CREATE MATERIALIZED VIEW`` and
  ``INSERT … SELECT`` (see below)

All of these follow SQL's semantics for NULL values (three-valued
logic in predicates, syntactic matching in set operations and
grouping, NULL-skipping aggregates); :doc:`the chapter on NULLs
<nulls>` spells out the rules and their effect on provenance.

Unsupported SQL Features
-------------------------

A query using one of the constructs below is refused with SQLSTATE
``0A000`` (``feature_not_supported``); where only part of a query cannot
be tracked, it runs with a warning instead (see :ref:`plain-sql`). Both
name their cause on a ``DETAIL`` line, which tools can rely on:

.. code-block:: text

    DETAIL:  provsql-reason: body-set-operation; scope: gap

The scope is ``deliberate`` when the construct has no provenance to give
(``EXCEPT ALL``, a ``LIMIT`` without ``ORDER BY``), ``gap`` when ProvSQL
does not compute it yet, and ``out-of-scope`` for what lies outside the
supported query fragment (random variables, where-provenance,
conditioning).

* Subqueries outside ``FROM`` whose body uses an outer join or
  ``LIMIT`` / ``OFFSET``; a scalar subquery nested in a larger expression
  (``1 + (SELECT …)``), which runs untracked with a warning
* Recursive CTEs before PostgreSQL 15, and where-provenance through a cycle
* ``EXCEPT ALL`` and ``INTERSECT ALL`` over tracked relations: use
  ``EXCEPT`` or ``INTERSECT``, or ``NOT IN`` / ``IN``
* An outer join with a tracked relation on its null-padded side, beside a
  ``LATERAL`` item that reads a row of the join, or whose ``USING`` /
  ``NATURAL`` merged column is read (see :doc:`nulls`)
* ``DISTINCT ON`` over an aggregation, a set operation, or keys that vary
  between possible worlds
* Grouping by, deduplicating on or uniting on the value of an aggregate
  other than ``count``, ``min``, ``max`` and :sqlfunc:`choose` (see
  :ref:`explode-agg-value`)
* Window functions other than those listed above (``lag``, ``lead``,
  ``first_value``, ``ROWS`` frames with an offset, …): the query runs with
  a warning, the window value untracked
* A data-modifying ``WITH`` (``INSERT`` / ``UPDATE`` / ``DELETE …
  RETURNING``), which runs once, untracked
* ``x IN (…)`` used as a value rather than as a condition, and an ordering
  row comparison against a subquery (``(a, b) < ANY (…)``)
* ``FILTER`` on ``array_agg(DISTINCT …)`` and on the ``json_agg`` family
  with ``DISTINCT``
* ``*`` over a tracked table where the number of columns must match (an
  arm of a set operation, ``INSERT INTO t SELECT *`` into an untracked
  table): ``*`` includes the ``provsql`` column, so PostgreSQL rejects the
  query, with a hint from ProvSQL; list the columns instead

For unsupported correlated subqueries, ``LATERAL`` can be used as a
workaround.
To read an aggregate result as its plain value on purpose, and silence the
warning that reports it, wrap it in ``plain()`` (see :ref:`plain-sql`).

.. _recursive-queries:

Recursive Queries
-----------------

``WITH RECURSIVE`` over provenance-tracked relations is supported on
PostgreSQL 15+. With ``UNION``, a row's provenance combines all its
derivations: the provenance of s--t reachability is the disjunction over
the s--t paths. With ``UNION ALL``, each derivation is a row of its own,
as in SQL.

Over cyclic data, a row has infinitely many derivations. ProvSQL records
its provenance as equations between the rows of the cycle, and the
semiring evaluating it solves them: probabilities and the Boolean,
tropical, Viterbi, temporal, why- and which-provenance semirings give a
value, while counting and how-provenance raise an error, the result being
infinite. :sqlfunc:`sr_formula` prints the equations::

    x₂ where x₁ = a ⊗ x₂, x₂ = 𝟙 ⊕ (c ⊗ x₃), x₃ = b ⊗ x₁

A ``UNION ALL`` recursion that does not end stops with an error.

.. _subqueries:

Subqueries
----------

A subquery condition gives a row the provenance of the subquery's matching
rows (``EXISTS``, ``IN``, ``= ANY``), or of their absence (``NOT EXISTS``,
``NOT IN``, ``<> ALL``). The body may join several relations, aggregate,
or be a set operation, and the condition may sit in a disjunction
(``WHERE name = 'NY' OR EXISTS (…)``). A block that reads tracked
relations only through its subqueries is tracked too, its own rows counting
as present in every possible world. The limits are listed under
`Unsupported SQL Features`_.

.. _limit:

ORDER BY, LIMIT and OFFSET
--------------------------

Over provenance-tracked relations, ``ORDER BY … LIMIT k`` is read in
every possible world: a row is kept when it is present and fewer than
``k`` present rows come before it in the order. The result therefore
has every row that may be among the first ``k``, each annotated with
that condition, and not just the first ``k`` rows of the actual data:

.. code-block:: postgresql

    -- every employee, with the probability of being among the three
    -- best paid
    SELECT name, probability_evaluate(provenance())
    FROM employees
    ORDER BY salary DESC
    LIMIT 3;

The rows ordered may be the groups of an aggregation, ranked on one of
their aggregates (``GROUP BY tag ORDER BY count(*) DESC LIMIT 10``, the
ten commonest tags): a group is then kept in the worlds where at most
``k`` groups have a greater count (see :ref:`rank-over-aggregate`).

``FETCH FIRST k ROWS WITH TIES`` keeps a row when fewer than ``k``
present rows come strictly before it, so that the rows tied with the
``k``-th are kept as well (``rank()``). ``LIMIT k`` and
``FETCH FIRST k ROWS ONLY`` number the rows (``row_number()``): when the
``ORDER BY`` leaves ties, SQL does not say which of the tied rows are
kept; ProvSQL then reads the clause as ``WITH TIES`` and emits a
``WARNING``. ``OFFSET m`` requires, in addition, that at least ``m``
present rows come before. The same holds in a subquery, in ``FROM``,
``LATERAL``, a ``WITH`` clause, or an arm of a set operation: a
``LATERAL`` subquery with ``ORDER BY … LIMIT k`` gives the first ``k``
rows of each group, as a ``rank()`` compared with ``k`` does (see
:ref:`window-aggregates`). So does ``SELECT DISTINCT ON (g) … ORDER BY
g…``, with ``k`` = 1: in each world, it keeps the rows of each group
that no present row of the group comes before, reading ties as
``WITH TIES``, with a ``WARNING``.

When the order of the rows is not in question, for instance to look at
the first rows of a result, or when the tokens do not stand for the
existence of the rows, the marker :sqlfunc:`plain` keeps the truncation of the
actual result:

.. code-block:: postgresql

    SELECT name, probability_evaluate(provenance())
    FROM employees
    ORDER BY salary DESC
    LIMIT plain(3);

It applies to ``FETCH FIRST plain(k) ROWS`` and ``OFFSET plain(m)``
too. The rows kept carry the provenance they have in the *full* result:
that a row was among those kept is not recorded. At the top level of a
statement, the statement shows some rows of the full result, each
correctly annotated. In a subquery, the truncated result feeds further
computation, whose provenance then misses that dependence, and ProvSQL
emits a ``WARNING``. The same holds of an ``OFFSET`` with ``WITH TIES``
and of an ``ORDER BY … LIMIT`` over an aggregation, a ``DISTINCT`` or a
set operation, or on values that vary between worlds (an aggregate, a
window function), which are not read in every world; for the latter,
ProvSQL emits a ``WARNING`` at the top level of a statement too, unless
the ``LIMIT`` is marked ``plain``.

A ``LIMIT`` or ``OFFSET`` with no ``ORDER BY`` at all is reported at the
top level too: SQL leaves open which rows it keeps, and with one of them
absent another world would keep a row the answer does not have. Add an
``ORDER BY`` to have the truncation read in every world, or use
``plain(k)`` to say that the truncation of the actual result is meant.
Whether a ``LIMIT`` truncates at all is not known before the query runs,
so ``LIMIT 1000`` over ten rows is reported as well.

.. _plain-sql:

Parts Whose Provenance Is Not Tracked
-------------------------------------

A few constructs are not tracked: their value is computed without regard
to provenance, and need not be the one plain SQL gives, since correlations
between that part and the rest of the query are ignored. The result is
then the exact provenance of a slightly different query, in which that
part is a constant; ProvSQL says which part in a ``WARNING``:

.. code-block:: text

    WARNING:  ProvSQL: scalar subquery nested in an expression: provenance not tracked
    DETAIL:  provsql-reason: sublink-nested-in-expression-frozen; scope: gap
    HINT:  Mark it plain() to say so.

The parts not tracked are:

* a window function other than those of :ref:`window-aggregates`;
* a window partitioned by an aggregate result, or ordered by one other
  than the tracked ranks (see :ref:`rank-over-aggregate`);
* a subquery in a position no rewriting handles (a scalar subquery
  nested in an expression; a subquery of a block with no tracked relation
  of its own that ProvSQL cannot track, such as one reading the
  ``provsql`` column);
* an ``ORDER BY … LIMIT`` that is not read in every world (see
  :ref:`limit`), and a ``LIMIT`` in a subquery;
* an aggregate result read as a plain value by a function, an operator or
  a comparison that ProvSQL does not track (``round(avg(x))``,
  ``json_build_object('n', count(*))``, see :doc:`aggregation`).

When the part reads only relations that the rest of the statement does
not track, the result is the provenance of the statement with those
relations untracked, a sound possible-world model. When it reads a
relation the rest tracks, the same tuples are uncertain for the rest and
not for that part: the warning names such a relation (``provenance not
tracked, although the statement tracks t``), and
setting :ref:`provsql.implicit_freeze <provsql-implicit-freeze>` to
``'error'`` refuses the query instead.

Marking the part with :sqlfunc:`plain` says that computing it without
provenance is meant, and
is the only way to silence the warning (a cast of an aggregate to a number
is tracked, see :doc:`aggregation`): ``plain((SELECT max(x) FROM t))``,
``plain(lag(v) OVER (ORDER BY d))``, ``LIMIT plain(k)``:

.. code-block:: postgresql

    SELECT id, plain((SELECT count(*) FROM posts c WHERE c.parent = p.id))
    FROM posts p;

A whole table can be read without provenance too, in ``FROM``: ``plain`` of a
value of its row type (the usual ``NULL::t``) stands for the table, its
columns without the ``provsql`` one, and brings no provenance of its own
(the rows of a join with it carry the provenance of the other side only):

.. code-block:: postgresql

    SELECT * FROM plain(NULL::employees);

Provenance in Nested Queries
-----------------------------

Subqueries in the ``FROM`` clause are supported. Each sub-result carries its
own provenance, which is further combined by the outer query:

.. code-block:: sql

    SELECT t.name, provenance()
    FROM (
        SELECT name FROM employees WHERE dept = 'R&D'
    ) t;

``CREATE TABLE … AS SELECT``
-----------------------------

You can materialise a provenance-tracked query result into a new table.
The new table automatically inherits provenance from its source:

.. code-block:: sql

    CREATE TABLE derived AS
    SELECT name, dept FROM employees WHERE active;

A row inserted into it later gets a token of its own, as in a table
passed to :sqlfunc:`add_provenance`.  A ``CREATE MATERIALIZED VIEW`` over
the same query gets the provenance of its rows too, as a ``provsql``
column that ``REFRESH MATERIALIZED VIEW`` recomputes; an aggregate is
stored there as its value.

``INSERT … SELECT``
---------------------

When both the source and target tables are provenance-tracked,
``INSERT … SELECT`` propagates provenance from the source query to the
inserted rows:

.. code-block:: sql

    CREATE TABLE archive (name VARCHAR, city VARCHAR);
    SELECT add_provenance('archive');
    INSERT INTO archive SELECT name, city FROM employees WHERE dept = 'R&D';

Each inserted row receives the provenance token computed by the source
``SELECT``, not a fresh independent token.

If the target table does not have a ``provsql`` column, a warning is
emitted indicating that source provenance is lost. The rows inserted are
those of the tracked result, without their provenance: a ``LEFT JOIN``, for
instance, also inserts the null-padded rows that are there only in other
possible worlds, as :sqlfunc:`remove_provenance` would keep them. To keep
the provenance, track the target first, or store the
token explicitly with :sqlfunc:`provenance` (no warning is then emitted):

.. code-block:: sql

    CREATE TABLE archive_tokens (name VARCHAR, token UUID);
    INSERT INTO archive_tokens SELECT name, provenance() FROM employees;

Selecting the ``provsql`` column of a tracked table for that purpose is
refused: it is the token of that input table, which is the provenance of
the query's rows only in the simplest queries.

The ``provenance()`` Function
------------------------------

In a ``SELECT`` list, ``provenance()`` returns the provenance UUID of the
current output tuple:

.. code-block:: sql

    SELECT name, provenance() FROM mytable;

The token can be passed to semiring evaluation functions
(see :doc:`semirings`) or to probability/Shapley functions.

In a query that aggregates, ``provenance()`` is read where SQL evaluates
it. In the ``SELECT`` list or ``HAVING``, outside an aggregate, it is the
provenance of the group. In ``WHERE``, in a ``GROUP BY`` key and inside an
aggregate (its ``FILTER`` included), it is the provenance of each input row:

.. code-block:: sql

    -- the tokens of the rows of each group, and the group's own
    SELECT dept, array_agg(provenance()) AS rows, provenance() AS grp
    FROM employees GROUP BY dept;
