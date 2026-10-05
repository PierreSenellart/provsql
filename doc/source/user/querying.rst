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

* ``SELECT … FROM … WHERE`` (conjunctive queries, multiset semantics)
* ``JOIN`` (inner joins, outer joins, natural joins)
* ``LATERAL`` subqueries
* Non-recursive CTEs (``WITH`` clauses).  A data-modifying CTE
  (``INSERT`` / ``UPDATE`` / ``DELETE … RETURNING``) runs once, not
  tracked, and the rows it returns carry no provenance; it may not
  read another CTE over provenance-tracked relations
* Recursive CTEs (``WITH RECURSIVE``) using ``UNION`` (set semantics) or
  ``UNION ALL`` (bag semantics) over
  provenance-tracked relations, on PostgreSQL 15+: the result carries
  provenance like any other query (e.g., the provenance of s--t
  reachability is the disjunction over the s--t paths).  A row derived
  through no cycle of the data has the ordinary provenance of its
  derivations, in every semiring.  A row derived through a cycle has
  infinitely many derivations: its provenance is recorded as equations
  between the rows of the cycle, and the semiring evaluating it decides its
  value.  Absorptive semirings (probability, Boolean, nonnegative tropical,
  Viterbi, temporal, …) give it, as do min-plus with arbitrary costs
  (``-Infinity`` downstream of a cycle of negative cost) and why- and
  which-provenance; counting refuses it with an integer mapping (the count
  is infinite), and so does how-provenance.  :sqlfunc:`sr_formula` prints the
  equations::

    x₂ where x₁ = a ⊗ x₂, x₂ = 𝟙 ⊕ (c ⊗ x₃), x₃ = b ⊗ x₁

  Where-provenance of a row derived through a cycle is not supported.
  ``SELECT *`` over a tracked relation in the terms gives the answer the
  columns written out give.  A term that reads the ``provsql`` column of the
  CTE itself (the token of a row derived so far, as data) is refused.  Since
  ``*`` includes the ``provsql`` column, ``SELECT 'x', *`` in a ``UNION`` arm
  fails in PostgreSQL itself.

  ``UNION ALL`` is the *bag* recursion, read as SQL reads it: each round
  applies the recursive term to the previous round only, the answer is the
  rows of every round together, and the recursion ends on a round that
  derives nothing. Each row is then one derivation, annotated by the
  conjunction along it, and two derivations of the same tuple are two rows,
  where ``UNION`` returns one row annotated with their disjunction. Over the
  two paths ``1→2→4`` and ``1→3→4``, each edge present with probability one
  half, ``UNION ALL`` gives the row ``4`` twice, at 0.25 each, and ``UNION``
  gives it once, at 0.4375. A ``UNION ALL`` recursion that does not end (over
  cyclic data, as in plain SQL) stops at an iteration bound with an error
* Subqueries in the ``FROM`` clause (including deeply nested)
* Subqueries outside ``FROM`` (``EXISTS``/``NOT EXISTS``,
  ``IN``/``NOT IN``, quantified comparisons such as ``= ANY`` or
  ``<> ALL``, scalar subqueries, ``ARRAY(SELECT …)``), correlated or
  not.  The subquery body may involve a single provenance-tracked relation, or an inner
  join of several, written with ``JOIN`` or as a comma-separated
  ``FROM`` list; e.g., ``NOT IN``
  over a joined body carries the same antijoin provenance as the
  equivalent ``EXCEPT``. A row comparison against a subquery is supported in
  both spellings, ``(a, b) NOT IN (…)`` and ``(a, b) <> ALL (…)``, which are
  the same condition and get the same provenance; an *ordering* row comparison
  (``(a, b) < ANY (…)``) is not.  An aggregate body can be compared against a
  constant or an outer column, including through ``IN``/``NOT IN``
  (the single-row aggregate body makes these scalar comparisons).
  The body of a membership test (``IN``, ``= ANY``) may be a set operation.
  A correlated one is read through its arms where the condition splits into
  conditions on them: ``NOT EXISTS`` over a ``UNION``, ``IN`` over an
  ``INTERSECT`` or an ``EXCEPT``, and ``NOT EXISTS`` over an ``EXCEPT``
  (every row of the first arm is one of the second).
  A block that reads tracked relations only through its subqueries (a
  ``FROM``-less ``SELECT`` whose condition is an ``EXISTS``, a constant or
  untracked left side filtered by a ``NOT EXISTS``, untracked tables whose
  ``WHERE`` tests tracked ones) is tracked as well: the rows of the untracked
  side count as present in every world, and the answer carries the
  provenance of the semijoin or the antijoin. Where this
  is not possible, ProvSQL does not track the block and emits a warning.
  A subquery condition need not be a conjunct of the ``WHERE`` clause: in
  ``WHERE name = 'NY' OR EXISTS (…)``, a row licensed by the other disjunct
  keeps its own provenance and one licensed only by the subquery gets the
  semijoin's. Such a combination may hold only one subquery condition
* ``EXISTS (…)`` in the ``SELECT`` list, as a Boolean *value*: the row
  gives two rows, one per truth value (see :ref:`explode-agg-value`), the
  true one annotated with the semijoin's provenance and the false one with
  the antijoin's. ``NOT EXISTS`` exchanges them, and an expression over the
  value (``CASE WHEN EXISTS (…) THEN …``) follows. ``x IN (…)`` as a value
  is not supported: SQL makes it unknown where no row matches and some
  comparison is unknown, which ProvSQL cannot tell from false
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
* ``FILTER`` clause on aggregates; on an ``AGG(DISTINCT …)`` it asks for an
  aggregate that skips NULL inputs, so ``array_agg(DISTINCT …) FILTER (…)``
  and the ``json_agg`` family are refused there
* ``INSERT … SELECT`` (provenance propagated when target table is
  provenance-tracked)

All of these follow SQL's semantics for NULL values (three-valued
logic in predicates, syntactic matching in set operations and
grouping, NULL-skipping aggregates); :doc:`the chapter on NULLs
<nulls>` spells out the rules and their effect on provenance.

Unsupported SQL Features
-------------------------

The following constructs are **not** currently supported; queries using them
either raise an error or have a part whose provenance is not tracked, with a
warning.
A query ProvSQL refuses raises an error with SQLSTATE ``0A000``
(``feature_not_supported``), which a client can tell from an internal
error (``XX000``). Its message names the cause for a reader, and its
``DETAIL`` line for a program:

.. code-block:: text

    ERROR:  ProvSQL: subquery over a provenance-tracked relation not supported
            here: its body is a set operation (UNION, INTERSECT, EXCEPT), whose
            rows the decorrelation cannot group
    DETAIL:  provsql-reason: body-set-operation; scope: gap

The ``provsql-reason`` tag stays stable while the message may be
reworded, so a tool surveying what ProvSQL covers can group refusals by it.
For an aggregate result read as a plain value
(``aggregate-read-as-plain-value``), the ``DETAIL`` line also names what
read it: ``reader:`` followed by one or more of ``function``, ``cast``,
``operator``, ``comparison``, ``comparison-of-aggregates``, ``in-list``,
``conditional``, ``boolean``, ``constructor`` (an array, a row, an XML
element), ``window`` and ``aggregate``. The ``scope`` says what kind of
limit it is:

``deliberate``
    the shape has no provenance to give, so the refusal, or the warning
    naming the part not tracked, is the answer, and will remain so:

    * ``EXCEPT ALL`` and ``INTERSECT ALL``, whose kept copies have no
      provenance of their own;
    * ``IN`` read as a value, whose unknown truth no count of matches tells
      from false;
    * a ``LIMIT`` with no ``ORDER BY``, since SQL leaves open which rows are
      kept, and a ``LIMIT plain(k)``, which asks for the cut of the actual
      result; an ``ORDER BY … LIMIT`` that ProvSQL does not read in
      every world is reported as a ``gap``;
    * a window function whose value is read at an offset other than one
      (``lag(x, 3)``, ``nth_value(x, 2)``), or an aggregate over a ``ROWS`` or
      ``GROUPS`` frame with an offset, both decided by which rows are present;
      the offset functions at offset one (``lag(x)``, ``first_value``, …),
      which are not tracked, are reported as a ``gap``;
    * a recursion outside the shape the fixpoint is defined for.

``gap``
    the query has a provenance, which ProvSQL does not compute yet.

``out-of-scope``
    the feature is outside the provenance of the supported query fragment:
    random variables and continuous distributions, where-provenance,
    conditioning, and ProvSQL's own functions, such as a ``provenance()``
    call in an expression.

A warning that names a part whose provenance is not tracked (see :ref:`plain-sql`)
carries the same fields, whatever :ref:`provsql.implicit_freeze
<provsql-implicit-freeze>` does with it.

The constructs themselves:

* **Subqueries outside FROM** whose body uses an outer join
  (``LEFT`` / ``RIGHT`` / ``FULL``; inner joins, in any syntax, are
  fine) or ``LIMIT``/``OFFSET`` (it would pick
  an order-dependent subset); also, when an *uncorrelated* body with
  no ``WHERE`` clause is compared against an outer column, only
  non-star aggregate bodies are supported (``max(x)``, ``count(x)``,
  …, including via ``IN``/``NOT IN``) -- a plain value body or
  ``count(*)`` in that position is not.  A scalar subquery nested in a
  larger expression (``1 + (SELECT …)``, an argument of a function such
  as ``generate_series(1, (SELECT n FROM t))``) is not tracked, and
  ProvSQL emits a ``WARNING``
* **Recursive CTEs** (``WITH RECURSIVE``) on PostgreSQL versions before 15
* ``EXCEPT ALL`` over provenance-tracked relations: SQL removes as many
  copies of a row as the right-hand side has, without saying which, so
  the copies it keeps have no provenance of their own. Use ``EXCEPT``,
  which returns the same rows whenever the left-hand side has no
  duplicates, or ``NOT IN`` / ``NOT EXISTS``; likewise ``INTERSECT
  ALL``, which keeps as many copies as the side with fewer has: use
  ``INTERSECT``, or ``IN`` / ``EXISTS``
* **Outer joins with a provenance-tracked relation on a null-padded
  side** beside a ``LATERAL`` item that reads a row of the join itself
  (a ``LATERAL`` over constants, or over another item of the same
  ``FROM``, is fine), or whose ``USING`` /
  ``NATURAL`` merged column is read (see
  :doc:`the chapter on NULLs <nulls>`): refused with an explicit error;
  an outer join whose null-padded side is untracked is fine
* ``DISTINCT ON`` over an aggregation, a set operation, or keys or an
  order on values that vary between worlds
* **Operations on the value of an aggregate whose values are not read off
  its contributions:** grouping by, deduplicating on or uniting on the
  value of an aggregate other than a ``count``, a ``min``, a ``max`` or a
  :sqlfunc:`choose`, which are exploded into one row per value they take
  (see :ref:`explode-agg-value`), in every arm of a set operation; and an
  aggregate of them that reads one in its ``FILTER``, its ``ORDER BY`` or
  its ``DISTINCT``, or whose inner value is not numeric (an aggregate of
  another kind is tracked per possible world, see :ref:`reaggregation`)
* `Window functions <https://www.postgresql.org/docs/current/tutorial-window.html>`_
  other than aggregates over a frame determined by values, the ranks
  ``RANK``, ``DENSE_RANK``, ``ROW_NUMBER``, and ``CUME_DIST``,
  ``PERCENT_RANK``, ``NTILE`` (see :ref:`window-aggregates`): ``LAG``,
  ``LEAD``, ``FIRST_VALUE``, ``ROWS`` frames with an offset, etc. The query still executes, with a
  ``WARNING``, and each output row carries the provenance of its input
  row, but the window value is an opaque scalar
* ``*`` **over a provenance-tracked table where the number of columns has
  to match:** ``*`` counts the ``provsql`` column, so PostgreSQL rejects an
  arm of ``UNION``, ``INTERSECT`` or ``EXCEPT`` whose other arm does not
  have that column (``each UNION query must have the same number of
  columns``), and an ``INSERT INTO t SELECT * …`` into a table that is not
  tracked (``INSERT has more expressions than target columns``). These are
  errors of PostgreSQL (SQLSTATE ``42601``), raised before ProvSQL sees the
  query; ProvSQL adds a ``HINT`` to them. List the columns instead of
  writing ``*``. Two arms that both read ``*`` from tracked tables are fine

For unsupported correlated subqueries, ``LATERAL`` can be used as a
workaround.
To read an aggregate result as its plain value on purpose, and silence the
warning that reports it, wrap it in ``plain()`` (see :ref:`plain-sql`).

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
