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
sample count, a ``delta;epsilon`` pair …) as documented below.

ProvSQL Studio's :ref:`evaluation strip <studio-circuit-eval-strip>`
exposes :sqlfunc:`probability_evaluate` interactively, with method
and arguments selectors.

When only a coarse estimate is needed, :sqlfunc:`probability_bounds`
returns cheap lower and upper bounds on the marginal probability of a
monotone-DNF token (as ``OUT`` parameters ``lower`` / ``upper``),
without the cost of exact compilation:

.. code-block:: postgresql

    SELECT person, (probability_bounds(provenance())).*
    FROM suspects;

.. _probability-guarantees:

Choosing a guarantee, not a method
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**In practice you do not pick a method.**  Ask for the *guarantee* you want and
let ProvSQL choose how to compute it:

- **exact** -- the default: ``probability_evaluate(provenance())`` returns the
  true probability.
- **relative** ``(ε, δ)`` --
  ``probability_evaluate(provenance(), 'relative', 'epsilon=0.05,delta=0.01')``:
  the estimate is within a factor ``1 ± ε`` of the true value with probability
  ``1 − δ``.  The right choice for **rare events** (small probabilities), where an
  absolute error bound would be meaningless.
- **additive** ``(ε, δ)`` --
  ``probability_evaluate(provenance(), 'additive', 'epsilon=0.05,delta=0.01')``:
  the estimate is within ``ε`` of the true value (absolute) with probability
  ``1 − δ``.

A cost-based chooser then picks and runs the cheapest method that meets your
request, per query, from the portfolio below.  Three things make this safe to
rely on:

- The tolerances **nest** (exact ⊂ relative ⊂ additive), so a ``relative`` or
  ``additive`` request still returns the **exact** value whenever an exact method
  is cheapest ("exact when cheaper") -- you never pay for approximation you did
  not need.
- The cost of a few methods is hard to predict from the circuit alone, so the
  chooser runs each optimistic pick under a **budget and escalates automatically**
  if it turns out slow -- a pathological circuit never hangs on the wrong method.
- A ``δ = 0`` (no-failure) approximate request is honoured by a *deterministic*
  method (the certified-bounds d-tree), not a sampler.

So the method names in the next section are an **escape hatch** -- for forcing a
specific algorithm, for ``EXPLAIN``-style understanding, or for the rare case
where you know your circuits better than the cost model.  The table summarises
where each shines; most users can skip it.

.. list-table:: Where each method shines (the chooser picks for you)
   :header-rows: 1
   :widths: 22 18 60

   * - Method
     - Guarantee
     - Best when (query / provenance circuit)
   * - ``independent``
     - exact
     - Read-once lineage: self-join-free / hierarchical conjunctive queries, where
       each input tuple is used at most once.  Linear time.
   * - ``inversion-free``
     - exact
     - Safe (inversion-free) UCQs the planner certifies -- linear-time via a
       structured d-DNNF even with self-joins.
   * - ``possible-worlds``
     - exact
     - Very few input tuples (a couple of dozen at most): brute force over all
       ``2^N`` worlds.
   * - ``sieve``
     - exact
     - Few clauses: a small monotone-DNF lineage (inclusion-exclusion).
   * - ``tree-decomposition``
     - exact
     - Low-treewidth lineage -- path-, cycle- or band-shaped join graphs; no
       external tool needed.
   * - ``d-tree``
     - exact / certified bounds
     - High-treewidth circuits where ``tree-decomposition`` bails; and the
       **deterministic** approximate corner -- it returns a certified interval, so
       it serves a ``δ = 0`` request.
   * - ``compilation`` (``d4`` / ``c2d`` / …)
     - exact
     - Hard lineage with hidden structure a knowledge compiler can exploit;
       last-resort, needs an external tool (see :doc:`knowledge-compilation`).
   * - ``wmc`` (``ganak`` / ``sharpsat-td`` / ``dpmc`` / ``weightmc``)
     - depends on tool
     - Hard lineage better suited to a weighted model counter than to a d-DNNF
       compiler; an alternative external-tool route to ``compilation``.  Exact for
       ``ganak`` / ``sharpsat-td`` / ``dpmc``; ``weightmc`` is an approximate
       ``(ε, δ)`` counter.
   * - ``monte-carlo``
     - additive ``(ε, δ)``
     - Any circuit; cheap when the probability is not tiny.
   * - ``karp-luby``
     - relative ``(ε, δ)``
     - Rare events (small ``p``) over a DNF, where additive error is uninformative.
   * - ``stopping-rule``
     - relative ``(ε, δ)``
     - A universal relative estimator for any circuit -- including
       random-variable and HAVING-aggregate lineage.

Computation Methods
^^^^^^^^^^^^^^^^^^^^

The methods below can be named explicitly as the second argument, but see
:ref:`above <probability-guarantees>` -- normally you request a guarantee and the
chooser selects among them.

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
      rounds; deterministic runtime. The rounds are spread across the clauses
      by *stratified* sampling (each clause gets a share proportional to its
      probability), which tightens the estimate at a given budget compared with
      drawing a clause at random each round.
    - ``epsilon=E`` (alias ``eps=E``) -- relative-error target, served by a
      *self-adjusting stopping rule*: the method samples only until the
      estimate is provably within the target, so on outputs whose clauses
      barely overlap it stops far short of the worst-case
      ``⌈4(e−2)·m·ln(2/δ)/ε²⌉`` rounds over the ``m`` clauses.
    - ``delta=D`` -- failure-probability target (only with ``epsilon``).
    - ``max_samples=N`` -- caps the number of rounds (only with the adaptive
      path), bounding the runtime for very small ``ε`` or large ``m``; if the
      cap is hit before the target, the reported guarantee is downgraded to the
      accuracy actually achieved.

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
    :cite:`DBLP:conf/cav/LaiMY25`,
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
    Weighted model counting (umbrella over several counters); the guarantee
    depends on the chosen tool -- ``'ganak'`` / ``'sharpsat-td'`` / ``'dpmc'``
    are exact, ``'weightmc'`` is an approximate ``(ε, δ)`` counter. The third
    argument selects the counter and its options as
    ``tool=<name>[,epsilon=E][,delta=D]`` (the legacy ``tool[;tool_args]`` form
    is still accepted): ``'ganak'`` :cite:`DBLP:conf/ijcai/SharmaRSM19`,
    ``'sharpsat-td'`` :cite:`DBLP:conf/cp/KorhonenJ21`, ``'dpmc'``
    :cite:`DBLP:conf/cp/DudekPV20`, or ``'weightmc'``. Same PATH /
    ``provsql.tool_search_path`` considerations as ``'compilation'``:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'wmc', 'ganak')
        FROM suspects;

Default strategy (no second argument)
    With no method named, ``probability_evaluate(provenance())`` requests
    the **exact** guarantee, and the cost-based chooser (see
    :ref:`above <probability-guarantees>`) runs the cheapest exact method
    applicable to the circuit: typically ``independent`` or
    ``inversion-free`` for safe queries, ``tree-decomposition`` for
    low-treewidth lineage, falling back to ``compilation`` with the
    compiler named by ``provsql.fallback_compiler`` (default ``'d4'``,
    see :doc:`configuration`) when no in-process method fits. Optimistic
    picks run under a budget and escalate automatically, so a pathological
    circuit never hangs on the wrong method.

To time every method on one circuit and compare results side by side,
use ProvSQL Studio's benchmark panel; see :doc:`studio`.

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

Arithmetic over these aggregates in ``HAVING`` (including comparisons
between two aggregates and integer-division thresholds) is also handled;
see :doc:`the aggregation chapter <aggregation>` for the supported forms
and their semantics.

An aggregate (``SUM``, ``AVG``, ``MIN`` or ``MAX``) whose possible values
span a very large range cannot be evaluated exactly in reasonable time (the
problem is *pseudo*-polynomial: exact cost grows with the magnitude of the
values).  In that case ask for an approximate answer instead -- a
``relative`` or ``additive`` guarantee -- and ProvSQL estimates the
probability by sampling, which is independent of the value magnitude:

.. code-block:: postgresql

    SELECT probability_evaluate(provenance(), 'relative', 'epsilon=0.05,delta=0.01')
    FROM orders GROUP BY region HAVING sum(amount_cents) > 100000000;

This is the *approximable* corner of the Ré–Suciu HAVING trichotomy; the
exact (default) call remains correct but may not terminate quickly on such a
query.  (``COUNT`` rarely needs this -- its values are small, so it is almost
always evaluated exactly.)

Network Reliability on Bounded-Treewidth Graphs
------------------------------------------------

All the methods above evaluate the provenance circuit that ProvSQL
builds along the relational query plan.  For one important query
family, ProvSQL instead builds the circuit **along a tree decomposition
of the data itself**, following the provenance refinement of
Courcelle's theorem :cite:`DBLP:conf/icalp/AmarilliBS15`:
*two-terminal network reliability*, the probability that a vertex is
reachable from a source in a probabilistic graph.  This problem is
#P-hard in general, but becomes solvable in time *linear in the number
of edges* when the graph has bounded treewidth – a property of many
real networks (series-parallel and outerplanar networks, transit and
utility networks, workflow graphs…).

The interface is an ordinary recursive reachability query.  Under
``provsql.boolean_provenance = on`` (the construction computes the
Boolean function of the lineage, so it lives in the same regime that
already governs recursion on cyclic data), the query rewriter
recognises the shape

.. code-block:: postgresql

    SET provsql.boolean_provenance = on;

    WITH RECURSIVE reach(node) AS (
        SELECT 1                                  -- the source vertex
      UNION
        SELECT e.dst FROM link e JOIN reach r ON e.src = r.node
    )
    SELECT node, probability_evaluate(provenance())
    FROM reach WHERE node = 42;

over a provenance-tracked base relation ``link`` whose tuples carry
probabilities, and compiles – along a tree decomposition of the edge
graph – one provenance circuit per reachable vertex, in linear total
size.  Cyclic graphs are handled natively and the computation is
exact; vertex columns of any type work (values are compared as text).

Two variations of the shape are recognised as well.  *Undirected
connectivity* is the natural symmetric traversal:

.. code-block:: postgresql

    WITH RECURSIVE reach(node) AS (
        SELECT 1
      UNION
        SELECT CASE WHEN e.src = r.node THEN e.dst ELSE e.src END
        FROM link e JOIN reach r ON r.node IN (e.src, e.dst)
    )
    SELECT node, probability_evaluate(provenance()) FROM reach;

and *deterministic edge filters* – a ``WHERE`` clause over the edge
relation's columns alone – restrict which edges participate:

.. code-block:: postgresql

    ... SELECT e.dst FROM link e JOIN reach r ON e.src = r.node
        WHERE e.capacity >= 10 ...

The emitted circuits are *deterministic and decomposable by
construction* (d-DNNFs), and each ``plus`` / ``times`` gate carries a
persisted **certificate** of that property (readable with
:sqlfunc:`get_infos`).  Downstream, the certificate is what makes the
tokens cheap: :sqlfunc:`probability_evaluate`'s cost-based chooser
settles on the linear exact ``independent`` method (which trusts
certified gates the way it trusts read-once structure), and the
d-DNNF artefact surface – ``interpret-as-dd`` compilation,
:sqlfunc:`ddnnf_stats`, :sqlfunc:`shapley` and :sqlfunc:`banzhaf` –
works on them without external compilers.  Shapley values of the edge
tuples give a principled *edge criticality* analysis of the network:

.. code-block:: postgresql

    SELECT src, dst, shapley(reach_token, provenance()) AS criticality
    FROM link;

When the route cannot apply – the data treewidth exceeds the supported
limit (the same cap as the ``tree-decomposition`` method, here applied
to the *data* treewidth, which is exactly the tractability
assumption), the edge tuples are not independent base tuples, or the
CTE deviates from the recognised shape – the query silently falls back
to the generic recursive-fixpoint evaluation, preserving its behaviour
exactly; set ``provsql.verbose_level`` to at least 10 to get a notice
when the fallback fires, or 20 to confirm the compiled route.

For workloads that already have the graph in columnar form, the
internal entry points :sqlfunc:`reachability_evaluate` (the
probability of one target) and :sqlfunc:`reachability_compile_stats`
(the probability plus the structural statistics substantiating the
linear-size guarantee: data treewidth, decomposition size, maximal
state count, d-DNNF size) take parallel arrays of sources,
destinations, provenance tokens and probabilities directly.

On a 2×n ladder network (treewidth 2), the integrated route answers
exactly over 1,500 probabilistic edges in under 200 ms end to end,
and the columnar form compiles 300,000 edges in seconds – where
evaluating the equivalent recursive query's lineage crosses the
circuit-treewidth cap at a few dozen edges, and the cyclic/undirected
case exceeds minutes already at thirty edges.

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
relations, certain self-joins, UCQs with disjoint branches…); see
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
be set.  Queries outside the supported shape (multi-cmp HAVING such
as ``COUNT(*) >= a AND COUNT(*) <= b``) fall through to the general
HAVING path.

HAVING-MIN / MAX closed-form shortcut
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The same mechanism handles
``GROUP BY g HAVING MIN(a) op c`` and ``MAX(a) op c`` (same six
operators).  Here no Poisson-binomial DP is needed: ProvSQL
partitions the group's rows on whether their value ``a`` satisfies the
comparison against ``c`` and computes the probability as a product of
the rows' presence probabilities, in ``O(N)`` per group.  For example
``MAX(a) >= c`` holds iff at least one row with ``a >= c`` is present,
with probability ``1 - ∏ (1 - p_i)`` over those rows; ``MIN(a) >= c``
holds iff no row with ``a < c`` is present and the group is non-empty;
all twelve ``(MIN|MAX, op)`` cases have analogous closed forms.  Like
the COUNT shortcut it fires automatically and replaces the
exponential possible-worlds enumeration the general path would
otherwise run for these aggregates.

HAVING-SUM closed-form shortcut
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``GROUP BY g HAVING SUM(a) op c`` (same six operators) is handled by a
weighted-sum dynamic program: the distribution of the group's running
sum over the present rows is built by convolution, and the probability
is read off as the mass of the sums satisfying the comparison (with the
empty group excluded).  This costs ``O(N × R)`` per group, where ``R``
is the range of reachable sums.  Because ``R`` grows with the
magnitude of the values, the shortcut is *pseudo*-polynomial and steps
aside for the general path when the range is too wide; for the usual
small-integer weights it replaces the exponential enumeration with a
fast DP.

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
normals…). See :doc:`continuous-distributions` for the full
surface.
