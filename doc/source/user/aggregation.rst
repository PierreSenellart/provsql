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
rather than a scalar, and the per-row provenance gates lower to
*semimodule-of-mixtures* shape via ``rv_aggregate_semimod``:
each per-row argument ``X_i`` is wrapped in
``provsql.mixture(prov_i, X_i, as_random(identity))`` so the
aggregate's effective semantics become

.. math::

   \mathrm{SUM}(X) = \sum_i \mathbf{1}\{\varphi_i\} \cdot X_i, \qquad
   \mathrm{AVG}(X) = \frac{\sum_i \mathbf{1}\{\varphi_i\} \cdot X_i}
                          {\sum_i \mathbf{1}\{\varphi_i\}}, \qquad
   \mathrm{PRODUCT}(X) = \prod_{i:\varphi_i} X_i.

Each aggregate has an ``INITCOND = '{}'`` so the FFUNC runs even on
an empty group, with per-aggregate empty-group identities:
:sqlfunc:`as_random` ``(0)`` for ``SUM``, SQL ``NULL`` for
``AVG`` (matching standard SQL), :sqlfunc:`as_random` ``(1)``
for ``PRODUCT``.

.. code-block:: postgresql

    SELECT district,
           sum(pm25)              AS sum_rv,
           avg(pm25)              AS avg_rv,
           expected(avg(pm25))    AS expected_avg
    FROM readings JOIN stations USING (station_id)
    GROUP BY district;

``AVG`` over a group whose every row has false provenance returns
``NaN`` (the natural floating-point ``0/0``); filter by
``probability_evaluate(provenance()) > 0`` before averaging if
``NULL`` is preferred. Other aggregates (``MIN``, ``MAX``,
``stddev``, ``covar_pop``, percentile aggregates) over
``random_variable`` are not yet supported. See
:doc:`continuous-distributions` for details.

HAVING
------

Simple ``HAVING`` clauses are supported:

.. code-block:: postgresql

    SELECT dept, COUNT(*) AS n, provenance()
    FROM employees
    GROUP BY dept
    HAVING COUNT(*) > 2;

Complex ``HAVING`` conditions that involve provenance-tracked aggregates
(e.g., a ``HAVING`` on the result of a computation over an aggregate)
are not fully supported and may produce incorrect results or an error.

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
