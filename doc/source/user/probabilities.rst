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
method, and an optional third argument for method-specific parameters. That
third argument is a comma-separated ``key=value`` list (the keys accepted
depend on the method); each method also keeps its historical shorthand (a bare
sample count, a ``delta;epsilon`` pair, …) as documented below.

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
    Approximate computation by random sampling. The third argument is either a
    fixed sample count (a bare integer or ``samples=N``) or an **additive**
    ``(ε, δ)`` target ``epsilon=E[,delta=D][,max_samples=M]`` (default
    ``eps=0.1, delta=0.05`` when omitted):

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'monte-carlo', '10000')
        FROM suspects;
        SELECT probability_evaluate(provenance(), 'monte-carlo', 'eps=0.01')
        FROM suspects;

    The ``(ε, δ)`` form guarantees that the estimate is within ``ε`` of the
    true probability ``p`` (in **absolute** terms) with probability at least
    ``1 − δ``, drawing ``N = ⌈ln(2/δ)/(2ε²)⌉`` samples (Hoeffding's
    inequality); the count is independent of ``p``. Because the error is
    *absolute*, an ``ε`` of, say, ``0.1`` is uninformative on a rare-event
    output with ``p ≪ ε``; for a **relative**-error guarantee in that regime
    use ``'karp-luby'``. Pin ``provsql.monte_carlo_seed`` for a reproducible
    estimate.

``'karp-luby'``
    Approximate computation by the Karp-Luby fully-polynomial randomised
    approximation scheme (FPRAS) for ``#DNF`` :cite:`DBLP:journals/jal/KarpLM89`.
    It delivers a **relative** ``(ε, δ)`` guarantee -- the estimate is within a
    *factor* ``1 ± ε`` of the true probability with probability at least
    ``1 − δ`` -- at a sample count independent of that probability. This is the
    guarantee that stays meaningful on rare-event outputs, where naive Monte
    Carlo's *absolute* ``ε`` (see ``'monte-carlo'`` above) says nothing. It
    applies to **DNF-shaped** circuits:
    a monotone disjunction (top-level ``OR``) of conjunctions (``AND``) of
    input leaves -- the lineage shape of a union of conjunctive queries over a
    tuple-independent database. Leaves may be shared across clauses. The
    method errors (it does not silently fall back) on any other shape:
    negation (``EXCEPT``/``monus``), comparison (``HAVING``), aggregation,
    random-variable, or multivalued (BID) gates.

    The third argument selects a fixed sample count or an ``(ε, δ)`` accuracy
    target (default ``epsilon=0.1, delta=0.05`` when omitted):

    - ``samples=N`` (or a bare integer ``N``) -- a fixed number of sampling
      rounds; deterministic runtime.
    - ``epsilon=E`` (alias ``eps=E``) -- relative-error target. The sample
      count is derived from the Chernoff bound
      ``N = ⌈4(e−2)·m·ln(2/δ)/ε²⌉`` over the ``m`` clauses.
    - ``delta=D`` -- failure-probability target (only with ``epsilon``).
    - ``max_samples=N`` -- caps the derived count (only with the adaptive
      path), bounding the runtime for very small ``ε`` or large ``m``.

    .. code-block:: postgresql

        -- fixed budget
        SELECT probability_evaluate(provenance(), 'karp-luby', '100000')
        FROM suspects;
        -- (ε, δ) guarantee
        SELECT probability_evaluate(provenance(), 'karp-luby', 'eps=0.05,delta=0.01')
        FROM suspects;

    ``samples`` is mutually exclusive with ``epsilon``/``delta``. Pin
    ``provsql.monte_carlo_seed`` for a reproducible estimate.

``'tree-decomposition'``
    Exact computation via a tree decomposition of the Boolean circuit
    :cite:`DBLP:journals/mst/AmarilliCMS20`. Built-in; no external tool
    required. Fails if the treewidth exceeds the maximum supported value:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'tree-decomposition')
        FROM suspects;

``'inversion-free'``
    Exact computation for the *inversion-free* ``UCQ(OBDD)`` class
    :cite:`DBLP:conf/icdt/JhaS11`: hierarchical, tuple-independent queries
    (possibly with self-joins) whose lineage admits a polynomial-size OBDD.
    The path builds a structured d-DNNF over a query-derived variable order,
    staying linear in the lineage where ``'tree-decomposition'`` would blow
    up. It requires that the planner certified the query as inversion-free
    (a certificate is attached to the provenance root); it errors otherwise.

    The certifier lets a **non-tracked relation** be used as a transparent
    filter, and **flattens SPJ subqueries and views** -- including a join
    inside the view, a view referenced several times (a structured
    self-join), and views-over-views -- before checking the class. An
    aggregating or ``UNION`` view, a correlated subquery, or a query that
    is genuinely non-hierarchical after flattening is not certified and
    falls back to another method.

    The default strategy already takes this path automatically when a
    certificate is present (see below), so calling the method explicitly is
    mainly useful for testing:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'inversion-free')
        FROM suspects;

``'compilation'``
    Exact computation by first compiling the circuit to a d-DNNF using an
    external tool, then evaluating the d-DNNF. The third argument names the
    tool: ``'d4'`` (default), ``'d4v2'``, ``'c2d'``, ``'dsharp'``,
    ``'minic2d'``, or one of the Panini target languages from KCBox
    :cite:`KCBoxPanini`,
    ``'panini-obdd'``, ``'panini-obdd-and'``
    :cite:`DBLP:journals/jair/LaiLY17`, ``'panini-decdnnf'``:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'compilation', 'd4')
        FROM suspects;

    The tool must be installed and accessible in the PostgreSQL server's
    PATH, or in a directory listed in the ``provsql.tool_search_path`` GUC
    (see :doc:`configuration`); :sqlfunc:`tool_available` reports the
    backend's view of a given tool. The CNF handed to the compiler and the
    resulting d-DNNF can both be inspected; see
    :doc:`knowledge-compilation`.

``'wmc'``
    Exact (umbrella for weighted model counters) computation. The third
    argument selects the counter and its options, either as
    ``tool[;tool_args]`` or as ``tool=<name>[,epsilon=E][,delta=D]``:
    ``'ganak'`` :cite:`DBLP:conf/ijcai/SharmaRSM19`, ``'sharpsat-td'``
    :cite:`DBLP:conf/cp/KorhonenJ21`, ``'dpmc'``
    :cite:`DBLP:conf/cp/DudekPV20`, or ``'weightmc'``. Same PATH /
    ``provsql.tool_search_path`` considerations as ``'compilation'``:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'wmc', 'ganak')
        FROM suspects;

``'weightmc'``
    Approximate weighted model counting using the external ``weightmc``
    tool (alias for ``'wmc'`` with that tool). The third argument carries
    the approximation tolerance, as ``epsilon=E[,delta=D]`` or the
    historical ``delta;epsilon`` pair:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'weightmc', 'epsilon=0.8')
        FROM suspects;

    Same PATH / ``provsql.tool_search_path`` considerations as
    ``'compilation'``.

Default strategy (no second argument)
    ProvSQL tries each method in order until one succeeds:

    1. **Independent evaluation** – used if the circuit is independent.
    2. **Inversion-free** – used if the query carries an inversion-free
       certificate (see the ``'inversion-free'`` method above). Controlled
       by the ``provsql.inversion_free`` GUC (on by default).
    3. **Tree decomposition** – used if the treewidth is within the
       supported limit.
    4. **Compilation** with the compiler named by
       ``provsql.fallback_compiler`` (default ``'d4'``) – used as a
       final fallback; requires that compiler to be installed. See
       :doc:`configuration`.

To time every method on one circuit and compare results side by side,
use :sqlfunc:`probability_benchmark`; see :doc:`knowledge-compilation`.

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
block is assumed to be true). :sqlfunc:`repair_key` sets up provenance
to enforce this mutual exclusivity: it takes a table and a key
attribute, and makes each group of tuples sharing the same key value
into mutually-exclusive alternatives, with the groups independent of
one another.  Call :sqlfunc:`repair_key` directly on a table without
provenance, as in the example below; it adds the ``provsql`` column
itself and is used *instead of* :sqlfunc:`add_provenance`, not after
it.

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

Boolean-Provenance Optimisations
---------------------------------

Probability evaluation routes through the Boolean-circuit pipeline
(``getBooleanCircuit`` then one of the methods above).  Two
optimisations exploit Boolean-specific structure to make this faster,
sometimes by orders of magnitude.

Safe-query rewriting (``provsql.boolean_provenance``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When the GUC ``provsql.boolean_provenance`` is ``on`` (off by
default), the planner recognises the *safe* hierarchical
conjunctive-query subclass of Dalvi-Suciu :cite:`DBLP:journals/jacm/DalviS12`
and rewrites such queries with per-atom ``DISTINCT`` projections so
that the resulting provenance circuit is *read-once*.  A read-once
circuit can be probability-evaluated in linear time by the
``'independent'`` method, instead of falling through to
``'tree-decomposition'`` or external compilation.

The rewriter recognises self-join-free hierarchical conjunctive
queries over TID or BID base tables, plus a number of extensions
that recover safety for query shapes the raw hierarchical criterion
would reject (FD-aware reductions driven by primary keys / NOT-NULL
UNIQUE constraints, constant selections, transparent deterministic
relations, certain self-joins, UCQs with disjoint branches, …); see
:ref:`safe-query-rewriter` in the developer documentation for the
full set.  Queries outside the
recognised class are passed through unchanged: the GUC enables an
opt-in shortcut, never a different result.

.. code-block:: postgresql

    SET provsql.boolean_provenance = on;

    SELECT person, ROUND(probability_evaluate(provenance())::numeric, 4)
    FROM suspects, witnesses
    WHERE suspects.case_id = witnesses.case_id;

**Trade-off.**  The rewriter tags the root gate so that semiring
evaluators incompatible with Boolean rewriting refuse to run on the
result (see :doc:`semirings` for the compatibility list).  In
practice this means: turn the GUC on for probability-heavy
workloads on hierarchical CQs, turn it off (or re-evaluate in a
fresh session) before running ``sr_counting``, ``sr_how``,
``sr_why`` on the same circuit.

HAVING-COUNT closed-form shortcut
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

For queries of the form
``GROUP BY g HAVING COUNT(*) op c`` (where ``op`` is one of
``>=``, ``>``, ``<=``, ``<``, ``=``, ``<>``) ProvSQL recognises the
HAVING comparator as a Poisson-binomial CDF over the per-row
Bernoulli indicators and computes its probability directly, in
``O(N × min(C, N-C))`` per group, where ``N`` is the per-group row
count.  This replaces the binomial-coefficient-sized DNF that the
general HAVING evaluator would otherwise construct, so HAVING-COUNT
probability queries that previously hit ``'tree-decomposition'`` or
``'compilation'`` now resolve in milliseconds.

The shortcut fires automatically when its soundness preconditions
are met: each per-row provenance must be a single ``gate_input``
leaf and the group-level aggregate must not be shared with any
other comparator.  It is transparent to the user; no GUC needs to
be set.  Queries outside the supported shape (HAVING-SUM,
HAVING-MIN/MAX, multi-cmp HAVING such as
``COUNT(*) >= a AND COUNT(*) <= b``, or non-trivial per-row
provenance from joins) fall through to the general HAVING path.

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
