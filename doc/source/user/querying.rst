Querying with Provenance
=========================

Once provenance is enabled on one or more tables, ProvSQL transparently
rewrites every SQL query to propagate and combine provenance annotations.
No changes to query syntax are required.

How It Works
-------------

ProvSQL installs a PostgreSQL *planner hook* (``shared_preload_libraries``
is required for this reason). When a query involves a provenance-enabled
table, the hook intercepts the query plan before execution and:

1. Identifies all relations carrying a ``provsql`` column.
2. Builds a provenance expression that combines the input tokens using the
   appropriate semiring operations (``plus`` for alternative use of
   tuples such as in duplicate elimination,
   ``times`` for combined use of tuples such as in joins, ``monus`` for difference).
3. Appends the resulting provenance token to the output as an extra column.

The ``provsql`` column of a provenance-tracked table that ``*`` expands to
takes that same last place: in ``SELECT *, a + 1 AS e FROM t`` the columns
are those of ``t``, then ``e``, then ``provsql``.  A position in ``ORDER BY``
counts the columns in that order, and so does the column list of a view or
of a ``CREATE TABLE AS`` over ``SELECT *``.

A whole row of a tracked table read as any row -- ``SELECT t FROM t``,
``row_to_json(t)``, ``json_agg(t)``, ``t::text``, ``ROW(t.*)`` -- has its
columns other than ``provsql``, as on the untracked table.  Where the
table's own row type is needed (``ROW(t.*)::t``, a function declared on
it, a column of a table created from the row), the row keeps it.

The final provenance token in each output row is a UUID that represents a
gate in a *provenance circuit* – a DAG recording how that result was derived.

Supported SQL Features
-----------------------

The following SQL constructs are supported with full provenance tracking:

* ``SELECT … FROM … WHERE`` (conjunctive queries, multiset semantics)
* ``JOIN`` (inner joins, outer joins, natural joins)
* ``LATERAL`` subqueries
* Non-recursive CTEs (``WITH`` clauses).  A data-modifying CTE
  (``INSERT`` / ``UPDATE`` / ``DELETE … RETURNING``) runs once, as
  plain SQL, and the rows it returns carry no provenance; it may not
  read another CTE over provenance-tracked relations
* Recursive CTEs (``WITH RECURSIVE``) using ``UNION`` (set semantics) over
  provenance-tracked relations, on PostgreSQL 15+: the recursive CTE is
  transparently evaluated to a fixpoint and the result carries provenance like
  any other query (e.g. the provenance of s–t reachability is the disjunction
  over the s–t paths).  On **acyclic** data this works for any semiring.  On
  **cyclic** data it requires an absorptive provenance class --
  ``provsql.provenance = 'absorptive'`` or ``'boolean'`` -- (an absorptive
  setting, under which the value converges); the resulting circuit is then
  sound only for absorptive evaluation (probability / Boolean), not for
  multiplicity-counting semirings
* Subqueries in the ``FROM`` clause (including deeply nested)
* Subqueries outside ``FROM`` (``EXISTS``/``NOT EXISTS``,
  ``IN``/``NOT IN``, quantified comparisons such as ``= ANY`` or
  ``<> ALL``, scalar subqueries, ``ARRAY(SELECT …)``), correlated or
  not: they are internally decorrelated and rewritten.  The subquery
  body may involve a single provenance-tracked relation, or an inner
  join of several, written with ``JOIN`` or as a comma-separated
  ``FROM`` list; e.g., ``NOT IN``
  over a joined body carries the same antijoin provenance as the
  equivalent ``EXCEPT``.  An aggregate body can be compared against a
  constant or an outer column, including through ``IN``/``NOT IN``
  (the single-row aggregate body makes these scalar comparisons)
* ``GROUP BY``, with ``GROUPING SETS``, ``ROLLUP`` and ``CUBE``
* ``SELECT DISTINCT`` (set semantics), and ``DISTINCT ON``, read as
  a ``LIMIT 1`` in each group (see :ref:`limit`)
* ``UNION`` and ``UNION ALL``
* ``EXCEPT``
* ``INTERSECT`` (set semantics): each row has the provenance of its
  copies on the left, ⊕-combined, times that of its copies on the right
* ``VALUES`` tables (treated as having no provenance)
* Aggregation (``SUM``, ``COUNT``, ``MIN``, ``MAX``, ``AVG``,
  ``COUNT(DISTINCT …)``, ``string_agg``, ``array_agg``)
* ``HAVING`` (a group is not filtered on the current data: one that
  fails the predicate may still appear in the result, with a provenance
  that evaluates to zero where the predicate fails; a group that can
  pass in no possible world may be left out)
* ``FILTER`` clause on aggregates
* ``INSERT … SELECT`` (provenance propagated when target table is
  provenance-tracked)

All of these follow SQL's semantics for NULL values (three-valued
logic in predicates, syntactic matching in set operations and
grouping, NULL-skipping aggregates); :doc:`the chapter on NULLs
<nulls>` spells out the rules and their effect on provenance.

Unsupported SQL Features
-------------------------

The following constructs are **not** currently supported; queries using them
will either raise an error or may cause incorrect provenance tracking.
A query ProvSQL refuses raises an error with SQLSTATE ``0A000``
(``feature_not_supported``), so that a client can tell it from an internal
error (``XX000``):

* **Subqueries outside FROM** whose body uses an outer join
  (``LEFT`` / ``RIGHT`` / ``FULL``; inner joins, in any syntax, are
  fine) or ``LIMIT``/``OFFSET`` (it would pick
  an order-dependent subset); also, when an *uncorrelated* body with
  no ``WHERE`` clause is compared against an outer column, only
  non-star aggregate bodies are supported (``max(x)``, ``count(x)``,
  …, including via ``IN``/``NOT IN``) -- a plain value body or
  ``count(*)`` in that position is not.  A scalar subquery nested in a
  larger expression (``1 + (SELECT …)``, an argument of a function such
  as ``generate_series(1, (SELECT n FROM t))``) is evaluated by
  PostgreSQL on the data as it is, its data treated as certain, and
  ProvSQL emits a ``WARNING``
* **Recursive CTEs** (``WITH RECURSIVE``) using ``UNION ALL`` (bag
  semantics), over cyclic data *without* an absorptive provenance class, or on
  PostgreSQL versions before 15
* ``EXCEPT ALL`` over provenance-tracked relations: SQL removes as many
  copies of a row as the right-hand side has, without saying which, so
  the copies it keeps have no provenance of their own. Use ``EXCEPT``,
  which returns the same rows whenever the left-hand side has no
  duplicates, or ``NOT IN`` / ``NOT EXISTS``; likewise ``INTERSECT
  ALL``, which keeps as many copies as the side with fewer has: use
  ``INTERSECT``, or ``IN`` / ``EXISTS``
* **Outer joins with a provenance-tracked relation on a null-padded
  side** in a query with a ``LATERAL`` item, or whose ``USING`` /
  ``NATURAL`` merged column is read (see
  :doc:`the chapter on NULLs <nulls>`): refused with an explicit error;
  an outer join whose null-padded side is untracked is fine
* ``DISTINCT ON`` over an aggregation, a set operation, or keys or an
  order on values that vary between worlds
* **Operations on aggregate results requiring comparison or duplicate
  elimination:** ``DISTINCT`` on aggregates, ``UNION``/``EXCEPT``
  (non-ALL) with aggregates, ``GROUP BY`` on aggregate results from a
  subquery
* `Window functions <https://www.postgresql.org/docs/current/tutorial-window.html>`_
  other than aggregates over a frame determined by values and the ranks
  ``RANK``, ``DENSE_RANK``, ``ROW_NUMBER`` (see
  :ref:`window-aggregates`): ``LAG``, ``LEAD``, ``NTILE``, ``ROWS``
  frames with an offset, etc. The query still
  executes and each output row carries the tuple provenance of its
  single input row, but the window value is an opaque scalar. A
  ``WARNING`` is emitted

For unsupported correlated subqueries, ``LATERAL`` can be used as a
workaround.
For comparison or duplicate elimination on aggregate results, explicitly
cast the aggregate column to its base type (e.g., ``cnt::bigint``),
which extracts the value but loses the provenance information on that
column.

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
g, …``, with ``k`` = 1: in each world, it keeps the rows of each group
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
emits a ``WARNING``. The same holds of a ``LIMIT`` without ``ORDER BY``,
of an ``OFFSET`` with ``WITH TIES``, and of an ``ORDER BY … LIMIT`` over
an aggregation, a ``DISTINCT`` or a set operation, or on values that
vary between worlds (an aggregate, a window function), which are not
read in every world; for the latter, ProvSQL emits a ``WARNING`` at the
top level of a statement too, unless the ``LIMIT`` is marked ``plain``.

.. _plain-sql:

Parts Evaluated as Plain SQL
----------------------------

A few constructs are not tracked: the rewriting leaves them to
PostgreSQL, which evaluates them on the data as it is. The result is
then the exact provenance of a slightly different query, in which that
part is a constant; ProvSQL says which part in a ``WARNING``:

* a window function other than those of :ref:`window-aggregates`, whose
  value is the one of the data as it is;
* a window partitioned or ordered by an aggregate result;
* a subquery in a position no rewriting handles (a scalar subquery
  nested in an expression, a subquery in a query over no tracked
  relation);
* an ``ORDER BY … LIMIT`` that is not read in every world (see
  :ref:`limit`), and a ``LIMIT`` in a subquery;
* an aggregate result read as a plain value by a function, an operator or
  a comparison that ProvSQL does not track (``round(avg(x))``,
  ``json_build_object('n', count(*))``, see :doc:`aggregation`).

When the part reads only relations that the rest of the statement does
not track, the result is the provenance of the statement with those
relations untracked, a sound possible-world model. When it reads a
relation the rest tracks, the same tuples are uncertain for the rest and
taken as they are for that part: the warning names such a relation, and
setting :ref:`provsql.implicit_freeze <provsql-implicit-freeze>` to
``'error'`` refuses the query instead.

Marking the part with :sqlfunc:`plain` says that plain SQL is meant, and
silences the warning (for an aggregate result, an explicit cast does, as
``count(*)::numeric``, with a warning that its provenance is lost): ``plain((SELECT max(x) FROM t))``,
``plain(lag(v) OVER (ORDER BY d))``, ``LIMIT plain(k)``:

.. code-block:: postgresql

    SELECT id, plain((SELECT count(*) FROM posts c WHERE c.parent = p.id))
    FROM posts p;

A whole table can be read as plain SQL too, in ``FROM``: ``plain`` of a
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
``SELECT``, rather than a fresh independent token.

If the target table does not have a ``provsql`` column, a warning is
emitted indicating that source provenance is lost. To keep it, store the
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
