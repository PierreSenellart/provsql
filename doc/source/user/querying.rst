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

The final provenance token in each output row is a UUID that represents a
gate in a *provenance circuit* – a DAG recording how that result was derived.

Supported SQL Features
-----------------------

The following SQL constructs are supported with full provenance tracking:

* ``SELECT … FROM … WHERE`` (conjunctive queries, multiset semantics)
* ``JOIN`` (inner joins, outer joins, natural joins)
* ``LATERAL`` subqueries
* Subqueries in the ``FROM`` clause (including deeply nested)
* ``GROUP BY``
* ``SELECT DISTINCT`` (set semantics)
* ``UNION`` and ``UNION ALL``
* ``EXCEPT`` and ``EXCEPT ALL``
* ``VALUES`` tables (treated as having no provenance)
* Aggregation (``SUM``, ``COUNT``, ``MIN``, ``MAX``, ``AVG``,
  ``COUNT(DISTINCT …)``, ``string_agg``, ``array_agg``)
* ``HAVING`` (non-matching groups receive zero provenance ``𝟘``
  rather than being filtered out)
* `Window functions <https://www.postgresql.org/docs/current/tutorial-window.html>`_
  (``ROW_NUMBER``, ``RANK``, ``SUM OVER``, ``LAG``, ``LEAD``, etc.
  — provenance stays per-row)
* ``FILTER`` clause on aggregates

Unsupported SQL Features
-------------------------

The following constructs are **not** currently supported; queries using them
will either raise an error or may cause incorrect provenance tracking:

* **Semi-joins** and **anti-joins** (``EXISTS``, ``NOT EXISTS``,
  ``IN`` subqueries, ``NOT IN``)
* **CTEs** (``WITH`` clauses) — queries run but provenance is silently
  lost
* ``INTERSECT``
* ``DISTINCT ON``
* ``GROUPING SETS``, ``CUBE``, ``ROLLUP``

For negation or exclusion, use ``EXCEPT`` rather than ``NOT IN``.
For correlated subqueries, ``LATERAL`` can be used as a workaround.

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

The ``provenance()`` Function
------------------------------

In a ``SELECT`` list, ``provenance()`` returns the provenance UUID of the
current output tuple:

.. code-block:: sql

    SELECT name, provenance() FROM mytable;

The token can be passed to semiring evaluation functions
(see :doc:`semirings`) or to probability/Shapley functions.
