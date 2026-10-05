Probabilities
=============

ProvSQL computes the probability that a query answer holds in a
*probabilistic database* :cite:`DBLP:conf/edbtw/GreenT06`: a database in
which every input provenance token carries an independent probability of
being present, from which ProvSQL derives the marginal probability of
each query answer.  Beyond independent inputs it also models
**correlated** inputs -- block-independent databases through
:sqlfunc:`repair_key` -- and a **continuous** tier, where columns of type
``random_variable`` carry distributions rather than scalars.

This chapter starts with the everyday workflow -- assigning input
probabilities, evaluating a query, aggregates -- and then turns to
reference material: when exact evaluation is tractable, the specialised
compilers for hard queries, the explicit method catalogue, and the
performance optimisations.

Setting input probabilities
---------------------------

Assign a probability to each input tuple's provenance token using
:sqlfunc:`set_prob`:

.. code-block:: postgresql

    SELECT set_prob(provenance(), 0.8) FROM mytable WHERE id = 1;

Or in bulk, from a column of the table itself:

.. code-block:: postgresql

    SELECT set_prob(provenance(), reliability) FROM sightings;

Probabilities must be in the range ``[0, 1]``.

A probability is **written once**: :sqlfunc:`set_prob` writes one on a
token that has none, accepts the identical value again, and refuses a
different one.  To give a tuple a different probability, give it a
different input gate:

.. code-block:: postgresql

    UPDATE sightings SET provsql = provsql.replace_input(provsql, 0.3)
      WHERE id = 42;

Results already derived keep the old probability; :doc:`persistence`
covers this and the block form :sqlfunc:`replace_block` for
:sqlfunc:`repair_key` tables.

To read back a stored probability with :sqlfunc:`get_prob`:

.. code-block:: postgresql

    SELECT get_prob(provenance()) FROM mytable;

:sqlfunc:`get_prob` returns ``1`` both for a token set as certain and for
one with no probability; :sqlfunc:`probability_is_set` tells them apart.

Correlated and block-independent inputs
---------------------------------------

By default ProvSQL assigns a fresh, independent provenance token to each
base tuple, so correlations between tuples are not modelled.  To model
correlated probabilities, derive them explicitly with queries: the
resulting tables carry correlated tokens.

A common case is a *block-independent database* (BID), where tuples are
grouped into mutually-exclusive blocks (exactly one tuple per block is
assumed to be true).  :sqlfunc:`repair_key` sets up provenance to enforce
this mutual exclusivity: it takes a table and a key attribute, and makes
each group of tuples sharing the same key value into mutually-exclusive
alternatives, with the blocks independent of one another.  Call
:sqlfunc:`repair_key` directly on a table without provenance, as in the
example below; it adds the ``provsql`` column itself and is used
*instead of* :sqlfunc:`add_provenance`, not after it.

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

Computing query probabilities
-----------------------------

Use :sqlfunc:`probability_evaluate` to evaluate the probability that a
query result holds, given the assigned input probabilities:

.. code-block:: postgresql

    SELECT person,
           probability_evaluate(provenance()) AS prob
    FROM suspects;

:sqlfunc:`probability` is a short alias with the same arguments, to match
the concise :sqlfunc:`expected` / :sqlfunc:`variance` / :sqlfunc:`support`
surface. It also accepts a Boolean event directly --
``probability(x > y)`` over ``random_variable`` columns, or
``probability(1 > 0)`` on a deterministic event (total: ``1`` if it holds,
``0`` otherwise) -- see the comparison-event surface in
:doc:`continuous-distributions`.

With no further argument it returns the **exact** probability.  An
optional second argument names a computation method and a third passes
method-specific parameters (a comma-separated ``key=value`` list whose
keys depend on the method; a bare sample count or a ``delta;epsilon`` pair
is also accepted).  You rarely
need them: see :ref:`Choosing a guarantee <probability-guarantees>` just
below, and the full catalogue under :ref:`forcing-a-method`.

ProvSQL Studio's :ref:`evaluation strip <studio-circuit-eval-strip>`
exposes :sqlfunc:`probability_evaluate` interactively, with method and
argument selectors.

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
request, per query.  Three things make this safe to rely on:

- The tolerances **nest** (exact ⊂ relative ⊂ additive), so a ``relative`` or
  ``additive`` request still returns the **exact** value whenever an exact method
  is cheapest ("exact when cheaper") -- you never pay for approximation you did
  not need.
- The cost of a few methods is hard to predict from the circuit alone, so the
  chooser runs each optimistic pick under a **budget and escalates automatically**
  if it turns out slow -- a pathological circuit never hangs on the wrong method.
- A ``δ = 0`` (no-failure) approximate request is honoured by a *deterministic*
  method, not a sampler.

Naming a method explicitly forces a specific algorithm: useful for
``EXPLAIN``-style understanding, or in the rare case where you know your
circuits better than the cost model.  The full catalogue,
with a summary table of where each method shines, is under
:ref:`forcing-a-method`; most users can skip it.

Quick bounds without exact evaluation
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When only a coarse estimate is needed, :sqlfunc:`probability_bounds`
returns cheap lower and upper bounds on the marginal probability of a
monotone-DNF token (as ``OUT`` parameters ``lower`` / ``upper``),
without the cost of exact compilation:

.. code-block:: postgresql

    SELECT person, (probability_bounds(provenance())).*
    FROM suspects;

Aggregates: expected values and HAVING
--------------------------------------

Expected values of aggregates
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

For aggregate queries over a probabilistic table, :sqlfunc:`expected`
computes the expected value of a ``COUNT``, ``SUM``, ``MIN``, ``MAX`` or
``AVG`` result:

.. code-block:: postgresql

    SELECT dept,
           expected(COUNT(*)) AS expected_count,
           expected(SUM(salary)) AS expected_salary
    FROM employees
    GROUP BY dept;

The expectation is over the possible worlds in which the value exists,
i.e., where its group has a row; it is ``NULL`` only if the value never
exists.  A ``COUNT`` without ``GROUP BY`` exists in every world, and is
0 where no row is present.  The result is exact for ``AVG`` over
tuple-independent rows and for any aggregate depending on at most 20
input tuples; otherwise it is estimated by Monte Carlo at the
``provsql.rv_mc_samples`` budget.

An optional second argument gives the *conditional* expectation
E[aggregate | condition], for a provenance condition; it is the
aggregate-specific spelling of the conditioning operator ``|`` (see
:doc:`conditioning`).

HAVING with probabilities
^^^^^^^^^^^^^^^^^^^^^^^^^^

``HAVING`` clauses are supported in the probabilistic setting.
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
and their semantics.  For the common ``COUNT`` / ``MIN`` / ``MAX`` /
``SUM`` thresholds a closed-form shortcut keeps the exact call fast; see
:ref:`having-shortcuts`.

A ``SUM`` (or the ``AVG`` that reduces to one) whose possible values
span a very large range cannot be evaluated exactly in reasonable time (the
problem is *pseudo*-polynomial: exact cost grows with the magnitude of the
values; ``MIN`` and ``MAX`` have a magnitude-independent closed form and are
not affected).  In that case ask for an approximate answer instead -- a
``relative`` or ``additive`` guarantee:

.. code-block:: postgresql

    SELECT probability_evaluate(provenance(), 'relative', 'epsilon=0.05,delta=0.01')
    FROM orders GROUP BY region HAVING sum(amount_cents) > 100000000;

Continuous random variables
---------------------------

The discrete-Bernoulli setting above can be combined with a
continuous tier: columns of type ``random_variable`` carry
distributions (Normal, Uniform, Exponential, Erlang, Gamma, Log-normal, Weibull, Pareto, Beta, Categorical,
Mixture) rather than scalars, and ``WHERE`` predicates on these
columns become conditioning events on the row's provenance.
Evaluation uses Monte Carlo by default and analytical closed forms
where they apply (e.g., comparisons decidable from the supports, exact
CDFs for a comparison over a single distribution, linear combinations
of normals…). See :doc:`continuous-distributions` for the full
surface.

.. _tractable-cases:

When is exact evaluation tractable?
-----------------------------------

Computing the exact probability is :math:`\#P`-hard in general
:cite:`DBLP:journals/vldb/DalviS07`, but several structural restrictions make
it tractable, and ProvSQL recognises each and evaluates it with a
dedicated method. Each row below is a
*sufficient* condition for tractability, classified by the shape of the
**data**, of its probabilistic **annotation** (TID = tuple-independent,
BID = block-independent-disjoint, *correlated* = arbitrary, e.g.,
view-derived), and of the **query**. ProvSQL applies whichever fits.

The query conditions are stated over classes of the relational calculus --
`conjunctive queries
<https://en.wikipedia.org/wiki/Conjunctive_query>`__ (CQ) and unions of them
(UCQ) -- which ProvSQL recognises from the structure of ordinary SQL queries.
All complexities are **data complexity**: the query is fixed, so its size is
not counted.  :math:`|D|` is the input size (number of tuples), :math:`k` the
treewidth relevant to each row (provenance, data, or joint treewidth), and
:math:`e` the number of essential query variables.

   +-------------+------------+------------------------+-------------------------+-----------------------------------------+----------------------------------------------------------+
   | Data        | Annotation | Query                  | Complexity              | Source                                  | ProvSQL mechanism                                        |
   +=============+============+========================+=========================+=========================================+==========================================================+
   | any         | TID / BID  | hierarchical,          | :math:`\Theta(|D|)`     | :cite:`DBLP:journals/vldb/DalviS07`     | :ref:`safe-query rewrite <safe-query-rewriting>`, then   |
   |             |            | **self-join-free** CQ  |                         | :cite:`DBLP:journals/jacm/DalviS12`     | ``independent``                                          |
   +-------------+------------+------------------------+-------------------------+-----------------------------------------+----------------------------------------------------------+
   | any         | TID        | inversion-free UCQ     | :math:`O(|D|)`          | :cite:`DBLP:conf/icdt/JhaS11`           | :ref:`inversion-free certification                       |
   |             |            | (self-joins allowed)   |                         |                                         | <inversion-free-route>`, then ``inversion-free``         |
   +-------------+------------+------------------------+-------------------------+-----------------------------------------+----------------------------------------------------------+
   | any         | TID        | safe UCQ needing       |                         | :cite:`DBLP:journals/jacm/DalviS12`     | :ref:`Möbius compiler <safe-ucq-mobius>`, then           |
   |             |            | Möbius inversion       | :math:`O(|D|^e)`        |                                         | the signed Möbius sweep over ``independent``             |
   |             |            | (self-join-free)       |                         |                                         | islands                                                  |
   +-------------+------------+------------------------+-------------------------+-----------------------------------------+----------------------------------------------------------+
   | any query whose **provenance** over this data and | :math:`2^{O(k)}\,|D|`   | :cite:`DBLP:journals/mst/AmarilliCMS20` | in-process :ref:`tree-decomposition                      |
   | annotations has treewidth ≤ k                     |                         |                                         | <in-process-compilers>` method                           |
   +-------------+------------+------------------------+-------------------------+-----------------------------------------+----------------------------------------------------------+
   | treewidth ≤ | TID / BID  | recursive reachability | :math:`2^{O(k^2)}\,|D|` | :cite:`DBLP:conf/icalp/AmarilliBS15`    | :ref:`reachability compiler <network-reliability-btw>`,  |
   | k (treelike)|            |                        |                         |                                         | then ``independent``                                     |
   +-------------+------------+------------------------+-------------------------+-----------------------------------------+----------------------------------------------------------+
   | joint treewidth ≤ k of   | any UCQ                | :math:`2^{O(k^e)}\,|D|` | :cite:`Amarilli2016thesis` (§4.2)       | :ref:`joint-width compiler <bounded-joint-width>`, then  |
   | the data and its         |                        |                         |                                         | ``independent``                                          |
   | annotation               |                        |                         |                                         |                                                          |
   +-------------+------------+------------------------+-------------------------+-----------------------------------------+----------------------------------------------------------+

For the exact guarantee, the :ref:`cost-based chooser <probability-guarantees>`
always tries ``independent`` and ``inversion-free`` when their certificate
applies (they are cheap and read-once-friendly), and tries
``tree-decomposition`` when it estimates the provenance treewidth low enough to
stand a chance.

Outside these sufficient conditions, when the provenance is genuinely
:math:`\#P`-hard with no structure to exploit, no exact polynomial guarantee
remains, and ProvSQL falls back to knowledge compilation (``compilation`` /
``wmc``) for an exact answer or to an FPRAS (``monte-carlo`` / ``karp-luby``)
for an approximate one (see :ref:`forcing-a-method`).

Specialised routes for hard queries
-----------------------------------

For two query families ProvSQL compiles a certified circuit
**along a tree decomposition of the data itself**, turning a
:math:`\#P`-hard problem into one linear in the data.  Both are exact and
need no external tool.

.. _network-reliability-btw:

Network reliability on bounded-treewidth graphs
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The first is *two-terminal network reliability*: the probability that a
vertex is reachable from a source in a probabilistic graph, following the
provenance refinement of Courcelle's theorem
:cite:`DBLP:conf/icalp/AmarilliBS15`.  This problem is :math:`\#P`-hard in general,
but becomes solvable in time *linear in the number of edges* when the
graph has bounded treewidth -- a property of many real networks
(series-parallel and outerplanar networks, transit and utility networks,
workflow graphs…).

The interface is an ordinary recursive reachability query, under
``provsql.provenance = 'absorptive'`` or ``'boolean'`` (see
:ref:`provsql-provenance-class`), over a provenance-tracked edge relation
``link`` whose tuples carry probabilities:

.. code-block:: postgresql

    SET provsql.provenance = 'absorptive';

    WITH RECURSIVE reach(node) AS (
        SELECT 1                                  -- the source vertex
      UNION
        SELECT e.dst FROM link e JOIN reach r ON e.src = r.node
    )
    SELECT node, probability_evaluate(provenance())
    FROM reach WHERE node = 42;

Cyclic graphs are handled, and vertex columns of any type work (values
are compared as text).  The following variations are recognised too:

* *undirected connectivity*:
  ``SELECT CASE WHEN e.src = r.node THEN e.dst ELSE e.src END FROM link e
  JOIN reach r ON r.node IN (e.src, e.dst)``;
* a *deterministic edge filter*, a ``WHERE`` clause over the edge
  relation's columns alone (``WHERE e.capacity >= 10``);
* a *source set* in the base arm, ``SELECT v FROM sources``: certain
  sources if ``sources`` is untracked, probabilistic ones if it is tracked
  (but not prepared with :sqlfunc:`repair_key`);
* edge relations prepared with :sqlfunc:`repair_key` (mutually exclusive
  alternative edges);
* a *derived* edge relation (a subquery or view over several tracked
  tables), provided no two derived edges share a base tuple;
* *bounded hops*: a counter column seeded by an integer constant,
  incremented in the recursive arm and bounded by a mandatory ``WHERE``
  condition (``<`` or ``<=``).  Row ``(v, h)`` holds when some *walk* (not
  necessarily a simple path) of exactly ``h`` edges reaches ``v``, and
  ``SELECT node FROM reach GROUP BY node`` gives the vertices within the
  bound:

  .. code-block:: postgresql

      WITH RECURSIVE reach(node, hops) AS (
          SELECT 1, 0
        UNION
          SELECT e.dst, r.hops + 1
          FROM link e JOIN reach r ON e.src = r.node
          WHERE r.hops < 4
      )
      SELECT node, hops, probability_evaluate(provenance()) FROM reach;

* *reachability per group*: joining the reached vertices with an untracked
  relation and grouping by one of its columns (``... FROM reach r JOIN
  regions t ON r.node = t.node GROUP BY t.region``, or the ``SELECT
  DISTINCT`` equivalent), optionally filtered on that relation's own
  columns, gives the probability that some vertex of each group is
  reachable;
* *k-terminal reliability*: a self-join of the CTE fixing one vertex per
  reference (``FROM reach r1, reach r2, reach r3 WHERE r1.node = 5 AND
  r2.node = 6 AND r3.node = 9``) gives the probability that all are
  reachable.

Any other shape, or data whose treewidth exceeds the cap of the
``tree-decomposition`` method, falls back to the generic recursive
evaluation, with the same result.  Set ``provsql.verbose_level`` to at
least 10 for a notice when this happens, or 20 to confirm the compiled
route.

The circuits produced are certified *deterministic and decomposable*
(**d-Ds**), so :sqlfunc:`probability_evaluate` is linear on them, and
:sqlfunc:`shapley`, :sqlfunc:`banzhaf` and :sqlfunc:`ddnnf_stats` need no
external compiler.  Shapley values of the edge tuples measure the
criticality of each edge:

.. code-block:: postgresql

    SELECT src, dst, shapley(reach_token, provenance()) AS criticality
    FROM link;

The same circuits evaluate exactly in every **absorptive semiring**
:cite:`DBLP:conf/icdt/DeutchMRT14`.  The nonnegative min-plus semiring
gives **min-cost reachability** (shortest distances, constrained by the
hop budget in the bounded-hop variant, per-group minima, and the
**directed Steiner cost** in the k-terminal form, shared edges paid
once):

.. code-block:: postgresql

    SELECT node, sr_tropical(provenance(), 'cost_mapping',
                             nonnegative => true) AS min_cost
    FROM reach;

Likewise :sqlfunc:`sr_viterbi` gives the most reliable path,
:sqlfunc:`sr_maxmin` the widest path, :sqlfunc:`sr_lukasiewicz` the best
fuzzy path, and :sqlfunc:`sr_temporal` the instants at which a vertex is
reachable (see :doc:`temporal`).  The result carries the
``'absorptive'`` assumption (:sqlfunc:`get_gate_type` reports its root as
``assumed``): semirings that are not absorptive, such as counting and
why-provenance, raise an error on it instead of returning a wrong value.

.. _bounded-joint-width:

Bounded joint width: hard UCQs over correlated data
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Safe-query rewriting and the ``inversion-free`` class only apply to
tractable queries, and not over **correlated** inputs.  ProvSQL also
evaluates :math:`\#P`-hard UCQs, such as :math:`H_0 = R(x), S(x, y),
T(y)`, **exactly**, over inputs that may be correlated, when their
**joint width** is small: the treewidth of the data together with its
correlation structure :cite:`Amarilli2016thesis` (§4.2).  The cost is
then linear in the data.

The route takes the same opt-in as :ref:`safe-query rewriting
<safe-query-rewriting>`, the ``'boolean'`` provenance class, and then
applies automatically to a UCQ the safe-query rewriter declined, whose
*existence* is formed by a ``SELECT DISTINCT`` or a ``GROUP BY``:

.. code-block:: postgresql

    SET provsql.provenance = 'boolean';

    -- H0 = R(x), S(x, y), T(y): #P-hard, evaluated here per group
    SELECT t.id, probability_evaluate(provenance())
    FROM r, s, t
    WHERE r.x = s.x AND s.y = t.y
    GROUP BY t.id;

It is the only exact route over **correlated** inputs
(:sqlfunc:`repair_key` blocks, view-derived provenance) in the
:ref:`tractability table <tractable-cases>`.  When the joint width exceeds
the supported cap or the query shape is not recognised, the query is
evaluated on its ordinary circuit by the general chooser; set
``provsql.verbose_level`` to confirm which route ran, or
``provsql.joint_width`` off to disable the route.  :doc:`Case Study 7
<casestudy7>`, Step 9, walks a worked example.

.. _forcing-a-method:

Forcing a specific method
-------------------------

Normally you request a guarantee and the cost-based chooser
(:ref:`probability-guarantees`) selects among the methods below.  You can
also name one explicitly as the second argument of
:sqlfunc:`probability_evaluate` -- to force an algorithm, to understand a
plan, or when you know your circuits better than the cost model.  This
table summarises where each shines:

.. list-table:: Where each method shines (the chooser picks for you)
   :header-rows: 1
   :widths: 22 18 60

   * - Method
     - Guarantee
     - Best when (query / provenance circuit)
   * - ``independent``
     - exact
     - Read-once provenance (self-join-free / hierarchical CQs, each input tuple used
       at most once).  Linear time.
   * - ``sq-rewrite``, ``bounded-jw``, ``reachability``
     - exact
     - The ``independent`` computation, on the circuit one of the three :ref:`planner-time
       routes <route-methods>` produced (read-once rewrite; certified d-D from the
       joint-width or reachability compiler).  Reported under the producing
       route's name so ``provsql.last_eval_method`` tells them apart.
   * - ``inversion-free``
     - exact
     - Safe (inversion-free) UCQs the planner certifies -- linear-time via a
       structured d-DNNF even with self-joins.
   * - ``mobius``
     - exact
     - Safe UCQs that are tractable *only* because the :math:`\#P`-hard terms of
       their inclusion-exclusion expansion cancel (:ref:`Möbius inversion
       <safe-ucq-mobius>`, the :math:`q_9` / :math:`Q_W` class).  Applies to a
       ``gate_mobius``-rooted token.
   * - ``possible-worlds``
     - exact
     - Very few input tuples (a couple of dozen at most): brute force over all
       ``2^N`` worlds.
   * - ``possible-worlds-aggregates``
     - exact
     - A comparison of aggregate results (a ``HAVING`` clause, the rank of a
       ``LIMIT``) over few input tuples: the same brute force, computing the
       aggregates' values in each world.
   * - ``sieve``
     - exact
     - Few clauses: a small monotone-DNF provenance (inclusion-exclusion).
   * - ``tree-decomposition``
     - exact
     - Low-treewidth provenance -- path-, cycle- or band-shaped join graphs; no
       external tool needed.
   * - ``d-tree``
     - exact / certified bounds
     - High-treewidth circuits where ``tree-decomposition`` bails; and the
       **deterministic** approximate corner -- it returns a certified interval, so
       it serves a ``δ = 0`` request.
   * - ``compilation`` (``d4`` / ``c2d`` / …)
     - exact
     - Hard provenance with hidden structure a knowledge compiler can exploit;
       last-resort, needs an external tool (see :doc:`knowledge-compilation`).
   * - ``wmc`` (``ganak`` / ``sharpsat-td`` / ``dpmc`` / ``weightmc``)
     - depends on tool
     - Hard provenance better suited to a weighted model counter than to a d-DNNF
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
       random-variable and HAVING-aggregate provenance.

Each method in detail:

``'independent'``
    Exact computation by a single linear pass that treats each gate as
    independent.  It is correct on **read-once** provenance (each input tuple
    used at most once) and on the **certified d-D circuits** that the
    safe-query, reachability and joint-width compilers produce.  It errors on
    a circuit that is neither:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'independent') FROM suspects;

    When the circuit came from one of those three compilers, the default
    strategy reports it under that compiler's own name instead (see
    ``'sq-rewrite'`` / ``'bounded-jw'`` / ``'reachability'`` below); naming
    ``'independent'`` explicitly still works on such a circuit.

.. _route-methods:

``'sq-rewrite'``, ``'bounded-jw'``, ``'reachability'``
    The three **planner-time routes**: the :ref:`safe-query (read-once)
    rewriter <safe-query-rewriting>`, the joint-width UCQ compiler and the
    bounded-treewidth reachability compiler.  Each replaces a query's
    ordinary provenance with a circuit of its own, evaluated by the same
    linear pass as ``'independent'``.  The method names record *which route
    produced the circuit*, so ``provsql.last_eval_method`` distinguishes them
    instead of reporting ``independent`` for all four cases:

    .. code-block:: postgresql

        SET provsql.last_eval_method = '';
        SELECT probability_evaluate(provenance()) FROM reachable;
        SHOW provsql.last_eval_method;   -- reachability

    Naming one on a token the route did not produce is an error.  The default
    strategy already picks the right one, so naming them explicitly is mainly
    useful for testing:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'reachability') FROM reachable;

``'possible-worlds'``
    Exact computation by exhaustive enumeration of all possible worlds.
    Exponential in the number of provenance tokens; practical only for small
    circuits:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'possible-worlds') FROM suspects;

``'possible-worlds-aggregates'``
    The same enumeration, computing the aggregates' values in each world,
    for comparisons of aggregate results (``HAVING``, the rank of a
    ``LIMIT``).  The chooser takes it when a comparison aggregates more rows
    than the circuit has input tuples, and these are at most 20, and for an
    aggregate over aggregate results (an ``avg`` of a ``count``, see
    :ref:`reaggregation`):

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'possible-worlds-aggregates')
        FROM top_users;

``'sieve'``
    Exact computation by inclusion-exclusion over the clauses of a monotone-DNF
    provenance, in time ``O(S × 2^m)`` for ``m`` clauses.  The chooser prefers it
    over ``'possible-worlds'`` when there are fewer clauses than input tuples,
    and over the compilers when ``m`` is small.  It applies only to a
    DNF-shaped circuit and errors when the clause count exceeds 24:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'sieve') FROM suspects;

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
    input leaves -- the provenance shape of a union of conjunctive queries over a
    tuple-independent database. Leaves may be shared across clauses. The
    method errors (it does not silently fall back) on any other shape:
    negation (``EXCEPT``/``monus``), comparison (``HAVING``), aggregation,
    random-variable, or multivalued (BID) gates.

    The third argument selects a fixed sample count or an ``(ε, δ)`` accuracy
    target (default ``epsilon=0.1, delta=0.05`` when omitted):

    - ``samples=N`` (or a bare integer ``N``) -- a fixed number of sampling
      rounds; deterministic runtime.
    - ``epsilon=E`` (alias ``eps=E``) -- relative-error target: the method
      samples only until the estimate is provably within the target, so on
      outputs whose clauses barely overlap it stops far short of the
      worst-case ``⌈4(e−2)·m·ln(2/δ)/ε²⌉`` rounds over the ``m`` clauses.
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

``'stopping-rule'``
    A universal **relative** ``(ε, δ)`` estimator that runs on the generic
    circuit, so unlike ``'karp-luby'`` it applies to **any** provenance -- plain
    Boolean, random-variable, or HAVING-aggregate alike.  It stops sampling as
    soon as the estimate is provably within the relative target, in ``O(S / (p ε²) · ln(1/δ))`` for an output of
    probability ``p``.  The third argument is the ``(ε, δ)`` target (with an
    optional ``max_samples`` cap; if the cap is reached first the guarantee
    degrades from relative to the additive accuracy actually achieved).  Pin
    ``provsql.monte_carlo_seed`` for a reproducible estimate:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'stopping-rule', 'eps=0.05,delta=0.01')
        FROM suspects;

``'tree-decomposition'``
    Exact computation via a tree decomposition of the Boolean circuit
    :cite:`DBLP:journals/mst/AmarilliCMS20`. Built-in; no external tool
    required. Fails if the treewidth exceeds the maximum supported value:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'tree-decomposition')
        FROM suspects;

``'d-tree'``
    Anytime **certified-interval** computation
    :cite:`DBLP:conf/icde/OlteanuHK10`: starting from cheap bounds, it
    refines a certified interval until it is narrow enough, or exact (width 0).
    It fills two corners the other exact methods do not: it returns an exact
    value where the provenance treewidth **exceeds**
    ``'tree-decomposition'``'s cap, and, being *deterministic* (its cost does
    not depend on ``δ``), it honours a ``δ = 0`` (no-failure) approximate
    request, returning a certified interval rather than a point estimate.  It
    works on any Boolean circuit.  Called by name
    with no third argument it refines to the exact value; given an accuracy
    target it stops at a certified interval of that width:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'd-tree') FROM suspects;

``'inversion-free'``
    Exact, linear-time computation for the *inversion-free* ``UCQ(OBDD)``
    class :cite:`DBLP:conf/icdt/JhaS11` -- hierarchical, tuple-independent
    queries (self-joins allowed) whose provenance admits a polynomial-size
    OBDD, where ``'tree-decomposition'`` would blow up.  It requires the
    planner's inversion-free certificate on the provenance root and errors
    without it.  The default strategy already takes this path automatically
    when the certificate is present, so naming the method is mainly useful
    for testing; see :ref:`inversion-free-route` for what the certifier
    accepts:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'inversion-free')
        FROM suspects;

``'mobius'``
    Exact computation for the safe UCQs that need :ref:`Möbius inversion
    <safe-ucq-mobius>` -- those tractable only because the :math:`\#P`-hard
    terms of their inclusion-exclusion expansion cancel (the :math:`q_9` /
    :math:`Q_W` class).  It applies to the ``gate_mobius``-rooted token
    ProvSQL produces for such a query, in linear time, and errors on any other
    token.  Naming **another** method on the same token
    (``'possible-worlds'``, ``'monte-carlo'``…), or asking for ``shapley`` /
    ``banzhaf``, evaluates the query's literal provenance instead and returns
    the same exact answer, more slowly.  The default strategy already takes
    the Möbius route for such a token, so naming it explicitly is mainly
    useful for testing:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'mobius') FROM safe_ucq;

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
    ``tool=<name>[,epsilon=E][,delta=D]`` (a ``tool[;tool_args]`` form is also
    accepted): ``'ganak'`` :cite:`DBLP:conf/ijcai/SharmaRSM19`,
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
    low-treewidth provenance, falling back to ``compilation`` with the
    compiler named by ``provsql.fallback_compiler`` (default ``'d4'``,
    see :doc:`configuration`) when no in-process method fits. Optimistic
    picks run under a budget and escalate automatically, so a pathological
    circuit never hangs on the wrong method.

    The empty default is identical to an explicit ``'exact'`` request on
    an ordinary Boolean circuit, **with one exception**. On a circuit
    carrying continuous random variables (see
    :doc:`continuous-distributions`), some comparison events have no closed
    form and depend on correlations that cannot be marginalised analytically
    (for instance conditioning on a mixture's own Bernoulli selector, or a
    comparison over a combination of mixtures). For those the empty default
    **falls back to the Monte Carlo sampler** (an approximate ``(ε, δ)``
    estimate at the ``provsql.rv_mc_samples`` budget), whereas an explicit
    ``'exact'`` request **raises an error** instead of returning an
    estimate; name ``'monte-carlo'`` explicitly to opt in.
    ``provsql.rv_mc_samples = 0`` disables the fallback, and the default then
    raises as well.

To time every method on one circuit and compare results side by side,
use ProvSQL Studio's benchmark panel; see :doc:`studio`.

Performance optimisations under the hood
----------------------------------------

Four optimisations exploit Boolean-specific structure to make
probability evaluation faster, sometimes by orders of magnitude; none
changes the result.

.. _safe-query-rewriting:

Safe-query rewriting (provenance class ``'boolean'``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When the provenance class is ``'boolean'`` (``provsql.provenance``, off by
default), the planner recognises the *safe* hierarchical
conjunctive-query subclass of Dalvi-Suciu :cite:`DBLP:journals/jacm/DalviS12`
and rewrites such queries so that their provenance circuit is
*read-once*, evaluated in linear time by the ``'independent'`` method
instead of ``'tree-decomposition'`` or external compilation.

The rewriter recognises self-join-free hierarchical conjunctive
queries over TID or BID base tables, plus a number of extensions
that recover safety for query shapes the raw hierarchical criterion
would reject (FD-aware reductions driven by primary keys / NOT-NULL
UNIQUE constraints, constant selections, transparent deterministic
relations, certain self-joins, UCQs with disjoint branches…); see
:ref:`safe-query-rewriter` in the developer documentation for the
full set.  Queries outside the
recognised class are passed through unchanged: the setting enables an
opt-in shortcut, never a different result.

.. code-block:: postgresql

    SET provsql.provenance = 'boolean';

    SELECT person, ROUND(probability_evaluate(provenance())::numeric, 4)
    FROM suspects, witnesses
    WHERE suspects.case_id = witnesses.case_id;

**Trade-off.**  Semiring evaluators incompatible with Boolean rewriting
refuse to run on the result (see :doc:`semirings` for the compatibility
list).  In practice: use the ``'boolean'`` class for probability-heavy
workloads on hierarchical CQs, and switch it off (or re-evaluate in a
fresh session) before running ``sr_counting``, ``sr_how``,
``sr_why`` on the same circuit.

.. _inversion-free-route:

Inversion-free certification
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The *inversion-free* ``UCQ(OBDD)`` class of Jha & Suciu
:cite:`DBLP:conf/icdt/JhaS11` -- hierarchical, tuple-independent queries
whose provenance admits a polynomial-size OBDD -- is a second linear-time
route for safe queries, a sibling of the :ref:`safe-query rewrite
<safe-query-rewriting>`.  ProvSQL certifies the query and the default
chooser takes the route automatically, right after ``'independent'``, so
no method need be named.  It stays linear in the provenance where
``'tree-decomposition'`` would blow up because the provenance is not
low-treewidth.

It differs from the safe-query rewrite on three counts:

- **Self-joins.**  The inversion-free class natively admits queries that
  join a relation with itself; the safe-query rewrite targets
  self-join-free CQs and recovers only limited self-join cases.
- **Provenance scheme.**  The inversion-free route evaluates the literal
  provenance unchanged, so it does **not** require the ``'boolean'``
  provenance class (it applies under the default semiring scheme too),
  and is governed by its own ``provsql.inversion_free`` GUC (on by
  default).  The safe-query rewrite restructures the query and fires only
  under ``provsql.provenance = 'boolean'``.
- **Edge cases.**  In exchange it certifies fewer shapes: the safe-query
  rewrite recovers safety from functional dependencies and BID blocks
  (see :ref:`safe-query-rewriting`), which the inversion-free certifier
  does not: its atoms must be strictly tuple-independent.  (A plain
  constant selection is fine either way: the certifier treats it as a
  transparent atom-local filter.)

The certifier does let a **non-tracked relation** act as a transparent
filter, and **flattens SPJ subqueries and views** before checking the
class: a join inside a view, a view referenced several times (a
structured self-join), and views-over-views all reduce to their base
atoms first.  An aggregating or ``UNION`` view, a correlated subquery,
or a query still non-hierarchical after flattening is not certified and
falls back to another method.  See :ref:`inversion-free-path` in the
developer documentation for the full pipeline.

.. _safe-ucq-mobius:

Möbius inversion for safe UCQs
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Some unions of conjunctive queries are *safe* (PTIME data complexity)
while being neither hierarchical nor inversion-free: they are
tractable only because the :math:`\#P`-hard terms of their
inclusion-exclusion expansion carry a zero Möbius coefficient and
cancel.  The canonical witness is Dalvi & Suciu's :math:`q_9` /
:math:`Q_W` :cite:`DBLP:journals/jacm/DalviS12`; the lattice
computation follows Dalvi, Schnaitter & Suciu (PODS 2010).  Under the
``'boolean'`` provenance class, when the safe-query rewriter and the
inversion-free certifier both decline a UCQ-existence shape (a
``SELECT DISTINCT`` / ``GROUP BY`` over a ``UNION``), ProvSQL
recognises this class and replaces the provenance with a *signed Möbius
combination* of read-once parts, which the default probability
evaluation handles in one linear pass; no method needs to be named.
:doc:`Case Study 7 <casestudy7>` runs the complete :math:`q_9`
example.

Like the safe-query rewrite, this is a shortcut, not a different
result: the query's literal provenance is kept, so :sqlfunc:`shapley`,
:sqlfunc:`banzhaf`, PROV export, and any *named* probability method
(``possible-worlds``…) answer exactly as on the ordinary provenance,
necessarily more slowly, since the literal provenance is the
:math:`\#P`-hard circuit the cancellation avoids.  Only the default / ``mobius`` probability
takes the fast route.

The route runs in :math:`O(|D|^e)` (:math:`e` the essential-variable
count), so the linear hierarchical and inversion-free routes are
tried first.  Where it applies, it takes precedence over the
:ref:`joint-width compiler <bounded-joint-width>`, whose success on
these queries is not guaranteed.  Inputs must be tuple-independent,
with one probabilistic tuple per element tuple and no two query slots
sharing a base tuple; anything else falls back to joint width, then
the general chooser.  ``provsql.mobius`` (on by default), the
``provsql.mobius_max_gates`` data-cost cap and the
``provsql.mobius_max_cnf`` query-cost cap control the route.

**Self-joins** are handled, except a self-join carrying an inversion
(``S(x,y),S(y,x)``), which is outside every tractable class.

.. _having-shortcuts:

HAVING closed-form shortcuts
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

For the common ``GROUP BY g HAVING <agg> op c`` thresholds (``op`` one of
``>=``, ``>``, ``<=``, ``<``, ``=``, ``<>``) ProvSQL computes the group's
probability in closed form, replacing the exponential DNF the general
HAVING path would build.  Each applies automatically, with no setting to
change, when each row's provenance is a single input token and the
group's aggregate is not shared with another comparison.

**COUNT.**  ``COUNT(*) op c`` is computed as a Poisson-binomial
distribution over the rows' presence, in
``O(N × min(C, N−C))`` per group (``N`` the per-group row count).
HAVING-COUNT queries that would otherwise hit ``'tree-decomposition'`` or
``'compilation'`` resolve in milliseconds.  A multi-comparator HAVING
(``COUNT(*) >= a AND COUNT(*) <= b``) falls through to the general path.

**MIN / MAX.**  ``MIN(a) op c`` and ``MAX(a) op c`` are computed as a
product of the rows' presence probabilities, in ``O(N)`` per group.  For
example,
``MAX(a) >= c`` holds iff at least one row with ``a >= c`` is present,
with probability ``1 − ∏ (1 − p_i)`` over those rows; ``MIN(a) >= c``
holds iff no row with ``a < c`` is present and the group is non-empty.
All twelve ``(MIN|MAX, op)`` cases have analogous closed forms.

**SUM.**  ``SUM(a) op c`` is computed from the distribution of the
group's sum (the empty group excluded), in ``O(N × R)`` per group with
``R`` the range of reachable sums.  Because ``R`` grows with the
magnitude of the values, the shortcut is *pseudo*-polynomial and steps
aside for the general path when the range is too wide; for the usual
small-integer values it replaces the exponential enumeration with a fast
computation.
