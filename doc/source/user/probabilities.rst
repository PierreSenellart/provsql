Probabilities
==============

ProvSQL can compute the probability that a query answer holds in a
*probabilistic database* :cite:`DBLP:conf/edbtw/GreenT06` – a database where each provenance token has an
independent probability, which is in turned used to determine
probabilities of existence of specific tuples. The same machinery
also handles **continuous random variables** at the value level
(see :doc:`continuous-distributions`): a column of type
``random_variable`` carries a distribution per row, and filter
predicates on that column lift into the row's provenance circuit.
RV-bearing queries route through Monte Carlo by default, with the
hybrid evaluator routing closed-form sub-circuits to the analytical
path.

Setting Probabilities
----------------------

Assign a probability to each input tuple's provenance token using
:sqlfunc:`set_prob`:

.. code-block:: postgresql

    SELECT set_prob(provenance(), 0.8) FROM mytable WHERE id = 1;

Or in bulk, from a column of the table itself:

.. code-block:: postgresql

    SELECT set_prob(provenance(), reliability) FROM sightings;

Probabilities must be in the range ``[0, 1]``.

To read back a stored probability with :sqlfunc:`get_prob`:

.. code-block:: postgresql

    SELECT get_prob(provenance()) FROM mytable;

Computing Query Probabilities
------------------------------

Use :sqlfunc:`probability_evaluate` to evaluate the probability that a query
result holds, given the assigned input probabilities:

.. code-block:: postgresql

    SELECT person,
           probability_evaluate(provenance()) AS prob
    FROM suspects;

The function accepts an optional second argument specifying the computation
method, and an optional third argument for method-specific parameters.

ProvSQL Studio's :ref:`evaluation strip <studio-circuit-eval-strip>`
exposes :sqlfunc:`probability_evaluate` interactively, with method
and arguments selectors.

Computation Methods
^^^^^^^^^^^^^^^^^^^^

``'independent'``
    Exact computation assuming all input tokens are mutually independent.
    Fails with an error if the circuit is not independent:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'independent') FROM suspects;

``'possible-worlds'``
    Exact computation by exhaustive enumeration of all possible worlds.
    Exponential in the number of provenance tokens; practical only for small
    circuits:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'possible-worlds') FROM suspects;

``'monte-carlo'``
    Approximate computation by random sampling. The third argument sets the
    number of samples (default: 1000):

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'monte-carlo', '10000')
        FROM suspects;

``'tree-decomposition'``
    Exact computation via a tree decomposition of the Boolean circuit
    :cite:`DBLP:journals/mst/AmarilliCMS20`. Built-in; no external tool
    required. Fails if the treewidth exceeds the maximum supported value:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'tree-decomposition')
        FROM suspects;

``'compilation'``
    Exact computation by first compiling the circuit to a d-DNNF using an
    external tool, then evaluating the d-DNNF. The third argument names the
    tool: ``'d4'`` (by default), ``'c2d'``, ``'dsharp'``, or ``'minic2d'``:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'compilation', 'd4')
        FROM suspects;

    The tool must be installed and accessible in the PostgreSQL server's
    PATH, or in a directory listed in the ``provsql.tool_search_path`` GUC
    (see :doc:`configuration`).

``'weightmc'``
    Approximate weighted model counting using the external ``weightmc``
    tool:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'weightmc')
        FROM suspects;

    Same PATH / ``provsql.tool_search_path`` considerations as
    ``'compilation'``.

Default strategy (no second argument)
    ProvSQL tries each method in order until one succeeds:

    1. **Independent evaluation** – used if the circuit is independent.
    2. **Tree decomposition** – used if the treewidth is within the
       supported limit.
    3. **Compilation with** ``d4`` – used as a final fallback; requires
       ``d4`` to be installed.

Expected Values of Aggregates
-------------------------------

For aggregate queries over a probabilistic table, the :sqlfunc:`expected`
function computes the expected value of the aggregate result.  It
supports ``COUNT``, ``SUM``, ``MIN``, and ``MAX``:

.. code-block:: postgresql

    SELECT dept,
           expected(COUNT(*)) AS expected_count,
           expected(SUM(salary)) AS expected_salary
    FROM employees
    GROUP BY dept;

An optional second argument specifies a provenance condition for
computing a *conditional* expectation E[aggregate | condition].  For
instance, to compute the expected count within each group conditioned
on the group existing (i.e., its provenance being true):

.. code-block:: postgresql

    SELECT dept,
           expected(COUNT(*), provenance()) AS conditional_count
    FROM employees
    GROUP BY dept;

Without the second argument, the expectation is unconditional.  With
it, the result is normalized by the probability of the condition.

HAVING with Probabilities
--------------------------

``HAVING`` clauses are partially supported in the probabilistic setting.
The following aggregate functions in ``HAVING`` are handled:
``COUNT``, ``SUM``, ``AVG``, ``MIN``, ``MAX``:

.. code-block:: postgresql

    SELECT dept, probability_evaluate(provenance())
    FROM employees
    GROUP BY dept
    HAVING COUNT(*) > 2;

Independent Tuples and Block-Independent Databases
----------------------------------------------------

ProvSQL assumes all input provenance tokens are *independent*.
Since by default, provenance tokens are assigned fresh to each tuple on
base tables,
correlation between
tuples is not modelled. If you need correlated probabilities, model them
explicitly by coding the correlations with queries, the resulting tables
will have correlated tuples.

A common case of correlated data is a *block-independent database*, where
tuples are grouped into mutually-exclusive blocks (exactly one tuple per
block is assumed to be true). :sqlfunc:`repair_key` restructures the
provenance circuit to enforce this mutual exclusivity: it takes a table
and a key attribute, and rewrites each group of tuples sharing the same
key value into independent, mutually-exclusive alternatives.

.. code-block:: postgresql

    CREATE TABLE weather(context VARCHAR, weather VARCHAR, ground VARCHAR,
                         p FLOAT);
    INSERT INTO weather VALUES
      ('day1', 'rain',    'wet', 0.35),
      ('day1', 'rain',    'dry', 0.05),
      ('day1', 'no rain', 'wet', 0.10),
      ('day1', 'no rain', 'dry', 0.50);

    -- Make tuples with the same context mutually exclusive
    SELECT repair_key('weather', 'context');

    -- Assign probabilities and evaluate
    SELECT set_prob(provenance(), p) FROM weather;

    SELECT ground,
           ROUND(probability_evaluate(provenance())::numeric, 3) AS prob
    FROM (SELECT ground FROM weather GROUP BY ground) t;

Continuous Random Variables
----------------------------

The discrete-Bernoulli setting above can be combined with a
continuous tier: columns of type ``random_variable`` carry
distributions (Normal, Uniform, Exponential, Erlang, Categorical,
Mixture) rather than scalars, and ``WHERE`` predicates on these
columns are rewritten into conditioning events on the row's
provenance. Evaluation routes through Monte Carlo by default, with
a hybrid evaluator falling back to analytical closed forms where
applicable (RangeCheck for support-decidable comparators, exact
CDFs for single-distribution :math:`\mathrm{gate\_cmp}`,
family-closure simplification for linear combinations of
normals, …). See :doc:`continuous-distributions` for the full
surface.
