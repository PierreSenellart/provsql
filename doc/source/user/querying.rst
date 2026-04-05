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
* ``JOIN`` (inner joins)
* Subqueries in the ``FROM`` clause
* ``GROUP BY``
* ``SELECT DISTINCT`` (set semantics)
* ``UNION`` and ``UNION ALL``
* ``EXCEPT``
* ``VALUES`` tables (treated as having no provenance)
* Aggregation (``SUM``, ``COUNT``, ``MIN``, ``MAX``, ``AVG``)
* ``HAVING``

Unsupported SQL Features
-------------------------

The following constructs are **not** currently supported; queries using them
will either raise an error or may cause incorrect provenance tracking:

* **Outer joins** (``LEFT JOIN``, ``RIGHT JOIN``, ``FULL OUTER JOIN``)
* **Semi-joins** and **anti-joins** (``EXISTS``, ``NOT EXISTS``,
  ``IN`` subqueries, ``NOT IN``)
* `Window functions <https://www.postgresql.org/docs/current/tutorial-window.html>`_ (``OVER …``)
* `Recursive CTEs <https://www.postgresql.org/docs/current/queries-with.html>`_ (``WITH RECURSIVE``)

For negation or exclusion, use ``EXCEPT`` rather than ``NOT IN``.

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
