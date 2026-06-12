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

To read back a stored probability with :sqlfunc:`get_prob`:

.. code-block:: postgresql

    SELECT get_prob(provenance()) FROM mytable;

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

With no further argument it returns the **exact** probability.  An
optional second argument names a computation method and a third passes
method-specific parameters (a comma-separated ``key=value`` list, the
keys depending on the method; each method also keeps a historical
shorthand, a bare sample count or a ``delta;epsilon`` pair).  You rarely
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

Naming a method explicitly is therefore an **escape hatch** -- for forcing a
specific algorithm, for ``EXPLAIN``-style understanding, or for the rare case
where you know your circuits better than the cost model.  The full catalogue,
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
it, the result is normalized by the probability of the condition. This
:sqlfunc:`expected` ``(aggregate, condition)`` form is the aggregate-specific
spelling of the conditioning operator ``|``; see :doc:`conditioning` for the
uniform ``A | B`` ("``A`` given ``B``") operator across discrete events,
random variables, and aggregates.

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
distributions (Normal, Uniform, Exponential, Erlang, Categorical,
Mixture) rather than scalars, and ``WHERE`` predicates on these
columns are rewritten into conditioning events on the row's
provenance. Evaluation routes through Monte Carlo by default, with
a hybrid evaluator falling back to analytical closed forms where
applicable (RangeCheck for support-decidable comparators, exact
CDFs for single-distribution ``gate_cmp``,
family-closure simplification for linear combinations of
normals…). See :doc:`continuous-distributions` for the full
surface.

.. _tractable-cases:

When is exact evaluation tractable?
-----------------------------------

Computing the exact probability is :math:`\#P`-hard in general
:cite:`DBLP:journals/vldb/DalviS07`, but several structural restrictions make
it tractable -- and ProvSQL recognises each and routes to a dedicated
mechanism rather than a general-purpose counter. Each row below is a
*sufficient* condition for tractability, classified by the shape of the
**data**, of its probabilistic **annotation** (TID = tuple-independent,
BID = block-independent-disjoint, *correlated* = arbitrary, e.g.
view-derived), and of the **query**. The planner-time rewrites and the
cost-based chooser apply whichever fits.

The query conditions are stated over classes of the relational calculus --
`conjunctive queries
<https://en.wikipedia.org/wiki/Conjunctive_query>`__ (CQ) and unions of them
(UCQ) -- which ProvSQL recognises from the structure of ordinary SQL queries.
All complexities are **data complexity**: the query is fixed, so its size is
not counted.  :math:`|D|` is the input size (number of tuples), :math:`k` the
treewidth relevant to each row (lineage, data, or joint treewidth), and
:math:`e` the number of essential query variables.

   +-------------+------------+------------------------+-------------------------+-----------------------------------------+----------------------------------------------------------+
   | Data        | Annotation | Query                  | Complexity              | Source                                  | ProvSQL mechanism                                        |
   +=============+============+========================+=========================+=========================================+==========================================================+
   | any         | TID / BID  | hierarchical,          | :math:`\Theta(|D|)`     | :cite:`DBLP:journals/vldb/DalviS07`     | :ref:`safe-query rewrite <safe-query-rewriting>`, then   |
   |             |            | **self-join-free** CQ  |                         | :cite:`DBLP:journals/jacm/DalviS12`     | ``independent``                                          |
   +-------------+------------+------------------------+-------------------------+-----------------------------------------+----------------------------------------------------------+
   | any         | TID        | inversion-free UCQ     | :math:`O(|D|)`          | :cite:`DBLP:conf/icdt/JhaS11`           | inversion-free certificate, then ``inversion-free``      |
   |             |            | (self-joins allowed)   |                         |                                         |                                                          |
   +-------------+------------+------------------------+-------------------------+-----------------------------------------+----------------------------------------------------------+
   | any         | TID        | safe UCQ needing       |                         | :cite:`DBLP:journals/jacm/DalviS12`     | :ref:`Möbius compiler <safe-ucq-mobius>`, then           |
   |             |            | Möbius inversion       | :math:`O(|D|^e)`        |                                         | the signed Möbius sweep over ``independent``             |
   |             |            | (self-join-free)       |                         |                                         | islands                                                  |
   +-------------+------------+------------------------+-------------------------+-----------------------------------------+----------------------------------------------------------+
   | any query whose **lineage** over this data and    | :math:`2^{O(k)}\,|D|`   | :cite:`DBLP:journals/mst/AmarilliCMS20` | ``tree-decomposition``                                   |
   | annotations has treewidth ≤ k                     |                         |                                         |                                                          |
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
``tree-decomposition`` when it estimates the lineage treewidth low enough to
stand a chance.

Outside these sufficient conditions, when the lineage is genuinely
:math:`\#P`-hard with no structure to exploit, no exact polynomial guarantee
remains, and ProvSQL falls back to knowledge compilation (``compilation`` /
``wmc``) for an exact answer or to an FPRAS (``monte-carlo`` / ``karp-luby``)
for an approximate one (see :ref:`forcing-a-method`).

Specialized routes for hard queries
-----------------------------------

For two query families ProvSQL does not evaluate the provenance circuit
built along the relational plan at all: it compiles a certified circuit
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
graph has bounded treewidth – a property of many real networks
(series-parallel and outerplanar networks, transit and utility networks,
workflow graphs…).

The interface is an ordinary recursive reachability query.  Under
``provsql.provenance = 'absorptive'`` or ``'boolean'`` (the compiled
circuit is the exact Boolean function of the lineage but only the
*absorptive quotient* of the infinite recursive semiring provenance,
so it lives in the same regime that already governs recursion on
cyclic data; see :ref:`provsql-provenance-class`), the query rewriter
recognises the shape

.. code-block:: postgresql

    SET provsql.provenance = 'absorptive';

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

The base arm may also be a relation, ``SELECT v FROM sources`` – a
*source set*.  When ``sources`` is itself provenance-tracked, each
source participates with its tuple's probability (a probabilistic
source set: "reachable from some present source"); an untracked
relation gives certain sources.  A :sqlfunc:`repair_key` source
relation is rejected (its tuples are block-correlated, not an
independent source set) and the query falls back.

Edge relations prepared with :sqlfunc:`repair_key` work too: a block
of mutually exclusive alternative edges (say, an uncertain road whose
true endpoint is one of several candidates) compiles as a single
(k+1)-way deterministic branching, preserving the block-independent
semantics exactly.

The recursive arm may even join a *derived* edge relation – a subquery
or view over several tracked tables.  Each derived edge then
participates as a compound event (the conjunction of its base tuples),
accepted when the derived edges' supports are pairwise disjoint – e.g.
a one-to-one join; edges sharing a base tuple are correlated, and the
query falls back to the generic evaluation.

*Bounded-hop reachability* is recognised as well: a hop-counting CTE
whose counter column is seeded by an integer constant, incremented in
the recursive arm, and bounded by a (mandatory) ``WHERE`` qual:

.. code-block:: postgresql

    WITH RECURSIVE reach(node, hops) AS (
        SELECT 1, 0
      UNION
        SELECT e.dst, r.hops + 1
        FROM link e JOIN reach r ON e.src = r.node
        WHERE r.hops < 4
    )
    SELECT node, hops, probability_evaluate(provenance()) FROM reach;

Row ``(v, h)`` carries the provenance of "some *walk* of exactly
``h`` edges connects the source to ``v``" – walks, not simple paths,
matching the recursive fixpoint's semantics: a cycle on the way pumps
the achievable lengths, and the compilation (whose states refine from
reachability relations to sets of achievable walk lengths) accounts
for that exactly, on cyclic data too.  Both ``<`` and ``<=`` bounds,
either column order, any integer seed, and the undirected, filtered,
multi-source and ``repair_key`` variants compose with the counter.
The natural follow-up, "which nodes are *within* k hops", obtained
by deduplicating the hop column away:

.. code-block:: postgresql

    ... SELECT node FROM reach GROUP BY node;

stays on the fast route: the OR of a vertex's per-length tokens is
correlated (lengths share edges), but the compilation pre-creates,
at the very gate address this deduplication computes, a certified
equivalent built from its native within-bound circuit, so
:sqlfunc:`probability_evaluate` still settles on the linear exact
method.

*Cross-vertex aggregations* of a reachability CTE are recognised as
well: grouping the reachable vertices by a column of a joined
(untracked) member relation:

.. code-block:: postgresql

    ... SELECT t.region
        FROM reach r JOIN regions t ON r.node = t.node
        GROUP BY t.region;

collapses each group's per-vertex tokens into an OR of *correlated*
events (the vertices share edges).  The route compiles, per group, the
certified circuit of "some member vertex is reachable" (the
set-reachability bit folded through the same decomposition DP) and
plants it at the gate address the aggregation computes, so the
per-region reliability evaluates through the linear certified route.
All the groups share one compilation: the tree decomposition and
variable analysis are built once, one cheap sweep runs per group, and
the parts of the per-group circuits the group's members do not
influence come out as the *same* gates (content-deduplicated
emission), materialised once.  The ``SELECT DISTINCT`` spelling of the
same aggregation (``SELECT DISTINCT t.region FROM ...`` with no
``GROUP BY``) is provenance-identical and recognised too; a
deterministic filter on the member relation's own columns
(``WHERE t.kind = 'hospital'``) is allowed -- it restricts which
members each group counts, exactly as an edge-column filter restricts
the edges, and is pushed into the member gathering.  A tracked member
relation, a filter that touches the recursive side, or any other
deviation from the join-and-group-by-one-column shape simply skips the
planting (the generic evaluation is always available).

*K-terminal conjunctions* close the family: a self-join of the CTE
with one constant node binding per reference

.. code-block:: postgresql

    ... SELECT 'all supplied'
        FROM reach r1, reach r2, reach r3
        WHERE r1.node = 5 AND r2.node = 6 AND r3.node = 9;

asks "are these vertices *all* reachable", and its row provenance
is the product of the correlated per-vertex tokens.  The route
compiles the certified all-members-reachable circuit (a richer
congruence: each forgotten terminal pends on the boundary vertices
that reach it, the pending sets folding through the same DP) and
plants it at the address the conjunction computes, so the query
evaluates to the **k-terminal reliability** through the linear
certified route -- with joint-worlds semantics: under nonnegative
min-plus (see :doc:`semirings`) the same token prices the cheapest
covering subgraph, the **directed Steiner cost**, shared edges paid
once where the raw product would pay them once per terminal.

The emitted circuits are *deterministic and decomposable by
construction* (**d-Ds** -- deterministic and decomposable, but not in
negation normal form, so not d-DNNFs), and each ``plus`` / ``times``
gate carries a
persisted **certificate** of that property (readable with
:sqlfunc:`get_infos`).  Downstream, the certificate is what makes the
tokens cheap: :sqlfunc:`probability_evaluate`'s cost-based chooser
settles on the linear exact ``independent`` method (which trusts
certified gates the way it trusts read-once structure), and the
d-D artefact surface – ``interpret-as-dd`` compilation,
:sqlfunc:`ddnnf_stats`, :sqlfunc:`shapley` and :sqlfunc:`banzhaf` –
works on them without external compilers.  Shapley values of the edge
tuples give a principled *edge criticality* analysis of the network:

.. code-block:: postgresql

    SELECT src, dst, shapley(reach_token, provenance()) AS criticality
    FROM link;

The same certified circuits evaluate exactly in every **absorptive
semiring**, not just under probability: the deterministic world
enumeration surfaces every minimal derivation support – every path –
and absorption (:math:`1 \oplus a = 1`) erases the rest, so the value
is the image of the absorptive provenance of the recursive query
:cite:`DBLP:conf/icdt/DeutchMRT14`.  In the nonnegative min-plus
semiring this gives **exact min-cost reachability** – single-source
shortest distances, on cyclic data too, in time linear in the
circuit:

.. code-block:: postgresql

    SELECT node, sr_tropical(provenance(), 'cost_mapping',
                             nonnegative => true) AS min_cost
    FROM reach;

The bounded-hop variant prices walks under a hop budget (a
constrained shortest path that plain Dijkstra does not answer
directly), and the cross-vertex aggregation gives per-region minima.
The other absorptive semirings read the same tokens: the
most-reliable path (:sqlfunc:`sr_viterbi`), the widest path
(:sqlfunc:`sr_maxmin` over a capacity enum), fuzzy best paths
(:sqlfunc:`sr_lukasiewicz`), and *temporal reachability* – when each
edge carries a validity multirange, :sqlfunc:`sr_temporal` returns
exactly the instants at which the vertex is reachable (see
:doc:`temporal`).  To keep the unsound evaluations out, the
materialised tokens carry the ``'absorptive'`` assumption marker
(:sqlfunc:`get_gate_type` reports the root as ``assumed``): counting
and why-provenance – genuinely infinite on cyclic recursion – refuse
loudly instead of returning a silently wrong value, while probability
and the absorptive semirings (see :doc:`semirings`) pass through.

When the route cannot apply – the data treewidth exceeds the supported
limit (the same cap as the ``tree-decomposition`` method, here applied
to the *data* treewidth, which is exactly the tractability
assumption), the edge tuples are not independent base tuples, or the
CTE deviates from the recognised shape – the query silently falls back
to the generic recursive-fixpoint evaluation, preserving its behaviour
exactly; set ``provsql.verbose_level`` to at least 10 to get a notice
when the fallback fires, or 20 to confirm the compiled route.

On a 2×n ladder network (treewidth 2), the integrated route answers
exactly over 1,500 probabilistic edges in under 200 ms end to end,
and the columnar form compiles 300,000 edges in seconds – where
evaluating the equivalent recursive query's lineage crosses the
circuit-treewidth cap at a few dozen edges, and the cyclic/undirected
case exceeds minutes already at thirty edges.

.. _bounded-joint-width:

Bounded joint width: hard UCQs over correlated data
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The query-side dichotomies – safe-query rewriting and the
``inversion-free`` class – make a *self-join-free hierarchical* or
*inversion-free* query tractable, but only over **tuple-independent**
inputs, and they give up on the genuinely :math:`\#P`-hard queries: the
textbook one is :math:`H_0 = R(x), S(x, y), T(y)`, and behind it the
whole hard family :math:`H_k`.  ProvSQL evaluates these **exactly**
when a different parameter is small – the **joint width**: the
treewidth of the data graph *together with* its correlation structure,
not of either alone :cite:`Amarilli2016thesis` (§4.2).  There are
instances whose data graph and whose lineage circuit are *both* of
small treewidth yet whose joint width – and hardness – is large, so
the bound has to be taken on the joint object (thesis Prop. 4.2.11);
when it *is* bounded, the probability is linear in the data, even
though the query is :math:`\#P`-hard and the inputs are arbitrarily correlated.

Like the reachability route above, the compilation is **data-side**: it
runs along a tree decomposition of the data – for correlated inputs, of
the data together with the slice of the provenance circuit that carries
the correlations – emitting a certified **d-D** by construction, with no
external compiler and no knowledge-compilation step.

The route is part of the Boolean machinery, so it takes the same opt-in
as :ref:`safe-query rewriting <safe-query-rewriting>`: the ``'boolean'``
provenance class, off by default.  Within that class it fires **automatically** – when a
conjunctive query the safe-query rewriter declined (an unsafe / :math:`\#P`-hard
UCQ) has its *existence* formed (a ``SELECT DISTINCT`` or a
``GROUP BY``), the planner recognises the shape and replaces its
provenance with the joint-width compiler's certified d-D, so
``probability_evaluate(provenance())`` returns the exact marginal with no
method named:

.. code-block:: postgresql

    SET provsql.provenance = 'boolean';

    -- H0 = R(x), S(x, y), T(y): #P-hard, evaluated here per group
    SELECT t.id, probability_evaluate(provenance())
    FROM r, s, t
    WHERE r.x = s.x AND s.y = t.y
    GROUP BY t.id;

(The ``provsql.joint_width`` GUC, on by default, is only a debug switch
to turn the recognition off and compare against the literal circuit.)

A ``GROUP BY`` is compiled in a **single pass**: the facts are gathered
once, the joint graph is decomposed once, and one bottom-up sweep emits
one d-D per group, so a query with many answer groups still pays a
single gather + decomposition + sweep.

Because the bound is on the joint object, the route stays exact where
every query-side method is inapplicable: over **correlated** inputs
(:sqlfunc:`repair_key` blocks, view-derived lineage), the one cell of
the :ref:`tractability table <tractable-cases>` that nothing else fills.
When the joint width exceeds the supported cap, or the query is not a
recognised UCQ-existence shape, the substitution simply does not fire
and the query falls back to the literal circuit and the general
chooser; set ``provsql.verbose_level`` to confirm which route ran.

:doc:`Case Study 7 <casestudy7>`, Step 9, walks a worked example over
both independent and :sqlfunc:`repair_key`-correlated reviewing data.

.. _safe-ucq-mobius:

Safe UCQs that need Möbius inversion
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The query-side routes above stop short of one corner of the
Dalvi-Suciu dichotomy: unions of conjunctive queries that are
**safe** -- PTIME data complexity -- yet are neither hierarchical nor
inversion-free, so they are tractable *only because* the :math:`\#P`-hard
terms of their inclusion-exclusion expansion cancel.  The canonical
witness, due to Dalvi & Suciu and named :math:`q_9` / :math:`Q_W`, is
built from the four hard-boundary patterns
:math:`h_0=R(x),S_1(x,y)`, :math:`h_1=S_1(x,y),S_2(x,y)`,
:math:`h_2=S_2(x,y),S_3(x,y)`, :math:`h_3=S_3(x,y),T(y)` as

.. math::

   q_9 = (h_2\vee h_3)\wedge(h_0\vee h_3)\wedge(h_1\vee h_3)
         \wedge(h_0\vee h_1\vee h_2).

Writing :math:`P(q_9)` by inclusion-exclusion on the CNF lattice and
grouping the terms up to logical equivalence, the conjunction of all
four patterns -- the one :math:`\#P`-hard element, with no separator --
acquires a **zero Möbius coefficient** and never has to be evaluated;
every surviving term is a safe disjunctive sentence.  ProvSQL computes
this exactly: it builds the CNF lattice over the disjuncts' connected
components, collapses elements up to logical equivalence (homomorphism),
reads off the integer coefficients, and recurses by the standard
lifted-inference steps (independent union of disjoint vocabularies, an
independent project at a *separator* variable, an inner Möbius step) down
to read-once islands over the data :cite:`DBLP:journals/jacm/DalviS12`.
(The lattice / Möbius computation follows Dalvi, Schnaitter & Suciu,
*Computing query probability with incidence algebras*, PODS 2010; Monet &
Olteanu, AMW 2018, compute these lattices at scale.)  The route is the
first integration of this complete safe-UCQ step in a query planner;
:math:`q_9` provably has no polynomial OBDD / FBDD / **decision**-DNNF
:cite:`DBLP:journals/mst/AmarilliCMS20` -- the monotone, decision-based
representations ProvSQL's compilers target -- so any polynomial method over
them *must* use subtraction somewhere, here at the root.  (Whether a
polynomial *general* d-DNNF exists for :math:`q_9` is open: the lower
bounds are for the decision classes, and surprising polynomial
constructions are known for some other inversion queries, so the absence of
*any* tractable circuit is conjectured, not proved.)

Like the joint-width route, it is part of the Boolean machinery, fires
**automatically** when a UCQ-existence shape the safe-query rewriter and
the inversion-free certifier both declined is formed (a ``SELECT
DISTINCT`` / ``GROUP BY`` over a ``UNION``), and answers
``probability_evaluate(provenance())`` exactly with no method named:

.. code-block:: postgresql

    SET provsql.provenance = 'boolean';

    -- q9 / QW: #P-hard, exact only because its hard part cancels.
    -- Each UNION arm is one disjunct (a conjunction of the h_i); the
    -- existence is formed by the UNION dedup.
    SELECT probability_evaluate(provenance())
    FROM (
      SELECT 1 FROM r, s1, s3 a, t        -- h0, h3
        WHERE r.x = s1.x AND a.y = t.y
      UNION
      SELECT 1 FROM s1, s2, s3 a, t       -- h1, h3
        WHERE s1.x = s2.x AND s1.y = s2.y AND a.y = t.y
      UNION
      SELECT 1 FROM s2, s3, s3 b, t       -- h2, h3
        WHERE s2.x = s3.x AND s2.y = s3.y AND b.y = t.y
      UNION
      SELECT 1 FROM r, s1, s1 b, s2, s2 c, s3   -- h0, h1, h2
        WHERE r.x = s1.x AND b.x = s2.x AND b.y = s2.y
          AND c.x = s3.x AND c.y = s3.y
    ) q;

The per-row token is rooted at a single new gate -- a *signed Möbius
combination* over its certified-independent children -- which the default
probability route evaluates in one linear pass (the islands read-once, the
root a signed sum of their probabilities).  Crucially, this is a
**probability shortcut layered over the normal provenance**, not a
replacement for it (the model is the ``inversion-free`` certificate, not a
measure-only gate): the gate carries the query's **literal lineage** as a
transparent child, so every
*other* evaluation -- ``shapley``, ``banzhaf``, a named probability method
such as ``possible-worlds``, PROV export -- passes straight through to that
lineage and answers exactly as it would for the ordinary provenance of the
query (necessarily slower -- the literal lineage is the very
:math:`\#P`-hard circuit the cancellation sidesteps -- but available).  Only
the default / ``mobius`` probability takes the fast cancelling route.

**Cost and priority.**  The route is **supra-linear**: its lifted recursion
fans out over a variable's active domain at each independent project, so it
runs in :math:`O(|D|^e)`, the degree the query's essential-variable count --
more expensive, by a polynomial factor, than the *linear* hierarchical and
inversion-free routes.  So among the **query-side** routes those two run
first, and Möbius only when they decline.  Among the routes for a recognised
*hard* UCQ, however, **Möbius takes precedence over the joint-width
compiler**: where it applies it is a *guaranteed*-PTIME exact route, whereas
the joint-width compiler's success on exactly these queries is unproven --
their joint treewidth need not be bounded -- and it can grind to its state
cap before declining, so trying Möbius first short-circuits past that.  (An
engineering preference, not a theorem: no polynomial d-D is *known* for
:math:`q_9`, but none is *proved* impossible.)  A data-cost cap
(``provsql.mobius_max_gates``) makes the route decline gracefully -- to the
joint-width compiler, then the general chooser -- on a high-level query
whose :math:`|D|^e` would out-cost them.

Inputs must be tuple-independent (lifted inference is sound only under
independence); v1 supports *reduced-form* UCQs: no relation repeated within
a single conjunct's homomorphic core, no constants, and -- the soundness
guards -- no bag multiplicity and no overlapping self-join slots (two facts
sharing an element tuple, or one tuple feeding two slots, carry a
correlation the element-keyed islands cannot represent).  Anything outside
that does not fire and the query falls back to the joint-width compiler, or
the literal circuit and the general chooser.  ``provsql.mobius`` (the
precedence route) and ``provsql.joint_width`` are independent debug
switches, both on by default; turning both off compares against the literal
lineage.

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
     - Read-once lineage (self-join-free / hierarchical CQs, each input tuple used
       at most once) and certified d-D circuits (from the safe-query, reachability
       and joint-width compilers).  Linear time.
   * - ``inversion-free``
     - exact
     - Safe (inversion-free) UCQs the planner certifies -- linear-time via a
       structured d-DNNF even with self-joins.
   * - ``mobius``
     - exact
     - Safe UCQs that are tractable *only* because the :math:`\#P`-hard terms of
       their inclusion-exclusion expansion cancel (:ref:`Möbius inversion
       <safe-ucq-mobius>`, the :math:`q_9` / :math:`Q_W` class).  Applies to a
       ``gate_mobius``-rooted token; a linear signed sweep over
       certified-independent islands.
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

Each method in detail:

``'independent'``
    Exact computation by a single linear pass that treats each gate as
    independent.  It is correct on **read-once** lineage (each input tuple
    used at most once) and on **certified d-D circuits** -- the
    deterministic-and-decomposable circuits the safe-query, reachability and
    joint-width compilers emit, whose ``plus`` / ``times`` gates carry a
    certificate of that property which the method trusts the same way it
    trusts read-once structure.  It errors on a circuit that is neither:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'independent') FROM suspects;

``'possible-worlds'``
    Exact computation by exhaustive enumeration of all possible worlds.
    Exponential in the number of provenance tokens; practical only for small
    circuits:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'possible-worlds') FROM suspects;

``'sieve'``
    Exact computation by inclusion-exclusion over the clauses of a monotone-DNF
    lineage, in time ``O(S × 2^m)`` for ``m`` clauses.  The chooser prefers it
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

``'stopping-rule'``
    A universal **relative** ``(ε, δ)`` estimator that runs on the generic
    circuit, so unlike ``'karp-luby'`` it applies to **any** lineage -- plain
    Boolean, random-variable, or HAVING-aggregate alike.  It samples under an
    optimal stopping rule, halting as soon as the estimate is provably within
    the relative target, in ``O(S / (p ε²) · ln(1/δ))`` for an output of
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
    :cite:`DBLP:conf/icde/OlteanuHK10`: starting from cheap leaf bounds, it
    refines the interval by independent-or decomposition (the connected
    components of the clause graph) and Shannon expansion on the most frequent
    variable until the interval is narrow enough, or exact (width 0).  It fills
    two corners the other exact methods do not: it returns an exact value where
    the lineage treewidth **exceeds** ``'tree-decomposition'``'s cap, and --
    being *deterministic*, at a cost independent of ``δ`` -- it is the method
    that honours a ``δ = 0`` (no-failure) approximate request, returning a
    certified interval rather than a point estimate.  It works on any Boolean
    circuit (a monotone-DNF lineage takes an optimised path).  Called by name
    with no third argument it refines to the exact value; given an accuracy
    target it stops at a certified interval of that width:

    .. code-block:: postgresql

        SELECT probability_evaluate(provenance(), 'd-tree') FROM suspects;

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

``'mobius'``
    Exact computation for the safe UCQs that need :ref:`Möbius inversion
    <safe-ucq-mobius>` -- those tractable only because the :math:`\#P`-hard
    terms of their inclusion-exclusion expansion cancel (the :math:`q_9` /
    :math:`Q_W` class).  It applies to a token whose root is the signed
    ``gate_mobius`` combination the planner substitutes for such a query; it
    is a linear sweep that sums the certified-independent islands'
    probabilities with the stored integer coefficients (it errors on a token
    that is not ``gate_mobius``-rooted).  This is the *fast* route only: the
    gate carries the query's literal lineage, so naming **another** method on
    the same token (``'possible-worlds'``, ``'monte-carlo'``, ...), or asking
    for ``shapley`` / ``banzhaf``, evaluates that lineage instead and returns
    the same exact answer (slower).  The default strategy already takes the
    fast Möbius path automatically for a ``gate_mobius``-rooted token, so
    naming it explicitly is mainly useful for testing:

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

Performance optimizations under the hood
----------------------------------------

Probability evaluation runs through the Boolean-circuit pipeline
(``getBooleanCircuit``, then one of the evaluation methods above).  Two
families of optimisation exploit Boolean-specific structure to make this
faster, sometimes by orders of magnitude; both are transparent to the
result.

.. _safe-query-rewriting:

Safe-query rewriting (provenance class ``'boolean'``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When the provenance class is ``'boolean'`` (``provsql.provenance``, off by
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

    SET provsql.provenance = 'boolean';

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

.. _having-shortcuts:

HAVING closed-form shortcuts
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

For the common ``GROUP BY g HAVING <agg> op c`` thresholds (``op`` one of
``>=``, ``>``, ``<=``, ``<``, ``=``, ``<>``) ProvSQL computes the group's
probability in closed form, replacing the exponential DNF the general
HAVING path would build.  Each fires automatically when its soundness
preconditions hold -- each per-row provenance a single ``gate_input``
leaf, the group-level aggregate not shared with another comparator -- and
needs no GUC.

**COUNT.**  ``COUNT(*) op c`` is recognised as a Poisson-binomial CDF over
the per-row Bernoulli indicators and computed directly, in
``O(N × min(C, N−C))`` per group (``N`` the per-group row count).
HAVING-COUNT queries that would otherwise hit ``'tree-decomposition'`` or
``'compilation'`` now resolve in milliseconds.  A multi-comparator HAVING
(``COUNT(*) >= a AND COUNT(*) <= b``) falls through to the general path.

**MIN / MAX.**  ``MIN(a) op c`` and ``MAX(a) op c`` need no DP: ProvSQL
partitions the group's rows on whether their value ``a`` satisfies the
comparison against ``c`` and computes the probability as a product of the
rows' presence probabilities, in ``O(N)`` per group.  For example
``MAX(a) >= c`` holds iff at least one row with ``a >= c`` is present,
with probability ``1 − ∏ (1 − p_i)`` over those rows; ``MIN(a) >= c``
holds iff no row with ``a < c`` is present and the group is non-empty.
All twelve ``(MIN|MAX, op)`` cases have analogous closed forms.

**SUM.**  ``SUM(a) op c`` is handled by a weighted-sum dynamic program:
the distribution of the group's running sum over the present rows is built
by convolution, and the probability is read off as the mass of the sums
satisfying the comparison (with the empty group excluded), in
``O(N × R)`` per group with ``R`` the range of reachable sums.  Because
``R`` grows with the magnitude of the values the shortcut is
*pseudo*-polynomial and steps aside for the general path when the range is
too wide; for the usual small-integer weights it replaces the exponential
enumeration with a fast DP.
