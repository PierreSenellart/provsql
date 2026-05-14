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

Arithmetic, explicit casts, window functions, and other expressions
(``COALESCE``, ``GREATEST``, etc.) on aggregate results are supported,
both in the same query and over subquery results:

.. code-block:: postgresql

    SELECT dept, COUNT(*) * 10 FROM employees GROUP BY dept;
    SELECT dept, SUM(salary) + 1000 FROM employees GROUP BY dept;
    SELECT dept, string_agg(name, ', ') || ' (team)' FROM employees GROUP BY dept;
    SELECT cnt::numeric FROM (SELECT COUNT(*) AS cnt FROM employees GROUP BY dept) t;
    SELECT dept, cnt, SUM(cnt) OVER () FROM (SELECT dept, COUNT(*) AS cnt FROM employees GROUP BY dept) t;
    SELECT dept, COALESCE(cnt, 0) FROM (SELECT dept, COUNT(*) AS cnt FROM employees GROUP BY dept) t;
    SELECT dept, GREATEST(cnt, 3) FROM (SELECT dept, COUNT(*) AS cnt FROM employees GROUP BY dept) t;

When such an operation is performed, the aggregate result is cast from
its internal ``agg_token`` representation back to the original aggregate
return type (e.g., ``bigint`` for ``COUNT``, ``numeric`` for ``AVG``).
A warning is emitted to indicate that the provenance information is lost
in the conversion. The provenance of the aggregate group itself is still
tracked in the ``provsql`` column.

Random-Variable Aggregates
---------------------------

When the aggregated column has type ``random_variable``
(see :doc:`continuous-distributions`), three aggregates lift
the standard arithmetic aggregates to the distribution algebra:
:sqlfunc:`sum`, :sqlfunc:`avg`, and
:sqlfunc:`product`. Each returns a ``random_variable``
rather than a scalar. See :ref:`continuous-aggregation` for the
semantics, empty-group identities, and worked examples.

HAVING
------

Simple ``HAVING`` clauses are supported:

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

Complex ``HAVING`` conditions that build a non-trivial expression on
top of an ``agg_token`` aggregate result (e.g., arithmetic across
multiple aggregates) are not fully supported and may produce
incorrect results or an error.

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

Grouping Sets
--------------

`GROUPING SETS, CUBE, and ROLLUP
<https://www.postgresql.org/docs/current/queries-table-expressions.html#QUERIES-GROUPING-SETS>`_
are not supported.
