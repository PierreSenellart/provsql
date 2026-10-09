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

The ``provsql`` column that ``*`` expands to over a tracked table also comes
last: in ``SELECT *, a + 1 AS e FROM t``, after ``e``. A whole row of a
tracked table (``SELECT t FROM t``, ``row_to_json(t)``, ``t::text``, ``t = u``,
``GROUP BY t``) has only its columns other than ``provsql``.

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

An exact probability of such a row expands the equations into a Boolean
circuit, of about the size of a cycle's rows times its terms: on a large
cyclic graph (a city's transport network), too large to build, and the
exact evaluation is then refused. The sampling methods (``'monte-carlo'``,
``'stopping-rule'``, and the ``'additive'`` and ``'relative'`` requests of
:ref:`probability-guarantees`) solve the equations in each sampled world
instead, at a cost of the order of the query's provenance.

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
every possible world: a row is kept where it is among the first ``k``
present rows. The result has every row that may be among the first ``k``,
each annotated with that condition:

.. code-block:: postgresql

    -- every employee, with the probability of being among the three
    -- best paid
    SELECT name, probability_evaluate(provenance())
    FROM employees
    ORDER BY salary DESC
    LIMIT 3;

The same holds for ``FETCH FIRST``, ``OFFSET``, ``DISTINCT ON`` (the first
row of each group), groups ranked on an aggregate (see
:ref:`rank-over-aggregate`), and in subqueries. Where the ``ORDER BY``
leaves ties, ``LIMIT`` keeps all the tied rows, as ``FETCH … WITH TIES``
does, and a ``WARNING`` says so. Ties are looked for among the rows of every
world, those absent from the actual data included (a null-padded row, a row
excluded by ``NOT EXISTS``), even two rows that are never present together.

To keep the truncation of the actual result instead, for instance to look
at its first rows, write :sqlfunc:`plain`:

.. code-block:: postgresql

    SELECT name, probability_evaluate(provenance())
    FROM employees
    ORDER BY salary DESC
    LIMIT plain(3);

The rows kept carry their provenance in the full result. A ``LIMIT`` with
no ``ORDER BY``, or one ProvSQL cannot read in every world, is also kept
as the truncation of the actual result, with a ``WARNING`` unless it is
written with ``plain()``.

.. _plain-sql:

Parts Whose Provenance Is Not Tracked
-------------------------------------

A few constructs are not tracked: their value is computed without regard
to provenance, as if that part of the query were a constant, and ProvSQL
says which part in a ``WARNING``:

.. code-block:: text

    WARNING:  ProvSQL: scalar subquery nested in an expression: provenance not tracked
    DETAIL:  provsql-reason: sublink-nested-in-expression-frozen; scope: gap
    HINT:  Mark it plain() to say so.

The parts not tracked are:

* a window function other than those of :ref:`window-aggregates`, and a
  window partitioned by an aggregate result or ordered by one other than
  the tracked ranks (see :ref:`rank-over-aggregate`);
* a scalar subquery nested in an expression, and a subquery ProvSQL cannot
  rewrite in a block with no tracked relation of its own;
* a ``LIMIT`` not read in every world (see :ref:`limit`), and a ``LIMIT``
  in a subquery;
* an aggregate result read as a plain value by a function or operator
  ProvSQL does not track (``round(avg(x))``, see :doc:`aggregation`).

If that part reads a relation the rest of the statement tracks, the
warning names it; setting :ref:`provsql.implicit_freeze
<provsql-implicit-freeze>` to ``'error'`` then refuses the query instead.

Wrapping the part in :sqlfunc:`plain` says that computing it without
provenance is meant, and silences the warning: ``plain((SELECT max(x) FROM
t))``, ``plain(lag(v) OVER (ORDER BY d))``, ``LIMIT plain(k)``. In
``FROM``, ``plain(NULL::t)`` reads table ``t`` without its provenance:

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

If the target table does not have a ``provsql`` column, a warning says
that the provenance is lost; the rows inserted are those of the tracked
result (a ``LEFT JOIN`` also inserts its null-padded rows). To keep the
provenance, track the target first, or store the token explicitly with
:sqlfunc:`provenance` (no warning is then emitted):

.. code-block:: sql

    CREATE TABLE archive_tokens (name VARCHAR, token UUID);
    INSERT INTO archive_tokens SELECT name, provenance() FROM employees;

Selecting the ``provsql`` column of a tracked table for that purpose is
refused: use ``provenance()``.

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
