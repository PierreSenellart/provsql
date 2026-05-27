Probability Evaluation
======================

ProvSQL computes probabilities by reducing a provenance circuit to
Boolean form and then dispatching to one of several evaluation
methods.  This page explains the dispatch architecture, gives the
background on the central data structures (d-DNNF, Tseytin
encoding, weighted model counting, tree decomposition), and ends
with a step-by-step guide for adding a new method.  See
:doc:`../user/probabilities` for the user-facing description of the
existing methods.

The continuous-random-variable surface layers an analytical /
hybrid path *on top of* this Boolean machinery; the architecture
of that layer is documented separately in
:doc:`continuous-distributions`. The sections below cross-link to
the relevant arms of the hybrid evaluator and the conditional
inference path.


Architecture
------------

The entry point is the SQL function :sqlfunc:`probability_evaluate`,
which calls :cfunc:`provenance_evaluate_compiled` on the |cpp| side.
That function builds a :cfunc:`BooleanCircuit` object from the
persistent circuit store and then calls
:cfunc:`probability_evaluate_internal` (in
:cfile:`probability_evaluate.cpp`).

:cfunc:`probability_evaluate_internal` receives the method name as
a string and dispatches via a chain of ``if`` / ``else if`` branches.
There is **no registration mechanism** -- methods are hardcoded in
the dispatcher.


Background: d-DNNF, Tseytin, Knowledge Compilation
--------------------------------------------------

Computing the probability that a Boolean formula evaluates to true
when its variables are assigned independently is :math:`\#\mathrm{P}`-hard
in general, but tractable for *structured* representations.  The
two structures ProvSQL exploits are **d-DNNF** and **tree
decomposition**, both of which give linear-time probability
evaluation in the size of the structure.  The methods that ship
with ProvSQL all reduce to one or the other.

d-DNNF
^^^^^^

A *deterministic decomposable negation normal form* (d-DNNF) is a
Boolean circuit built from ``AND``, ``OR``, ``NOT``, and variable
leaves, satisfying two structural properties:

- **Decomposability**: for every ``AND`` gate, the variable sets
  of its children are pairwise disjoint.  This means the children
  represent independent events, and the probability of the AND is
  the product of the children's probabilities.
- **Determinism**: for every ``OR`` gate, the children represent
  mutually exclusive events.  This means the probability of the
  OR is the sum of the children's probabilities -- no inclusion-
  exclusion correction is needed.

Together these two properties make a single bottom-up traversal
sufficient to compute the probability:
:cfunc:`dDNNF::probabilityEvaluation` does exactly that.  The
implementation in :cfile:`dDNNF.cpp` uses an explicit stack
instead of recursion to avoid blowing the call stack on very deep
circuits, and memoises intermediate results so that shared
sub-circuits are evaluated only once.

A general Boolean formula is *not* a d-DNNF.  Producing a d-DNNF
from an arbitrary formula -- *knowledge compilation* -- is the
expensive part; once you have it, evaluation is cheap.  The
``compilation`` and ``tree-decomposition`` branches of the
dispatcher both end in a :cfunc:`dDNNF` object that
:cfunc:`dDNNF::probabilityEvaluation` then walks.

Tseytin Encoding
^^^^^^^^^^^^^^^^

External knowledge compilers (``d4``, ``c2d``, ``dsharp``,
``minic2d``) and the ``weightmc`` model counter all consume
Boolean formulas in **DIMACS CNF** format.  Producing CNF from a
ProvSQL Boolean circuit is the job of
:cfunc:`BooleanCircuit::TseytinCNF` (in :cfile:`BooleanCircuit.cpp`),
whose string output each caller writes into its own
@c provsql::ScopedTempDir before invoking the tool.

The Tseytin transformation introduces one fresh variable per
internal gate of the circuit, then writes a small set of clauses
that encode the gate's semantics.  For an ``AND`` gate
:math:`g = s_1 \wedge s_2 \wedge \dots \wedge s_n`, that's one
binary clause :math:`(\neg g \vee s_i)` for every child, plus
one big clause
:math:`(g \vee \neg s_1 \vee \neg s_2 \vee \dots \vee \neg s_n)`.
``OR`` is dual.  ``NOT`` becomes two two-literal clauses.  A unit
clause forcing the root variable true is added at the end.

For weighted model counting (and the d4 compiler when built with
weight support), Tseytin also emits one ``w`` line per leaf
variable giving the probability of the corresponding ProvSQL
input gate -- so the SAT-side of the pipeline knows the weights to
multiply through.

The output is dumped to a temporary file under ``/tmp``;
:cfunc:`BooleanCircuit::compilation` then invokes the chosen
compiler with that file and reads the result back. The invocation
goes through :cfunc:`run_external_tool` (:cfile:`external_tool.cpp`),
which honours the ``provsql.tool_search_path`` GUC by prepending
its value to ``PATH`` for the duration of the call.  The tool runs
via ``/bin/sh -c`` in its **own process group**: while it runs the
backend polls for a pending cancel, and on ``statement_timeout`` /
``pg_cancel_backend`` it ``SIGKILL``\ s the whole group (so a tool
that ignores ``SIGINT`` or forks a worker into another process group,
as KCBox/Panini does, is still stopped) and then raises the interrupt
via ``CHECK_FOR_INTERRUPTS``.  Before composing the command line, the same call site pre-flights
the binary with :cfunc:`find_external_tool`, so a missing tool
fails with an actionable error rather than letting the shell return
exit 127. After the call, the wait status is decoded by
:cfunc:`format_external_tool_status` to distinguish "not found",
"killed by signal", and "ran and exited nonzero". The same trio
is used by :cfunc:`BooleanCircuit::WeightMC` for ``weightmc`` and
by :cfunc:`DotCircuit::render` for ``graph-easy``.

Knowledge Compilers and the NNF Format
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

All four supported external compilers (``d4``, ``c2d``,
``dsharp``, ``minic2d``) produce a d-DNNF in the *NNF* text
format -- a line-oriented representation where each line is one
node:

- ``L lit`` -- a leaf literal (positive or negative).
- ``A k c1 c2 ...`` -- an AND of :math:`k` children, given by
  their node indices.
- ``O k c1 c2 ...`` -- an OR of :math:`k` children.

Modern d4 also emits a few extra node kinds (``a`` / ``o`` / ``f``
/ ``t`` for constants, and a decision-tree variant); the parser
in :cfunc:`BooleanCircuit::compilation` handles both the legacy
and the d4-extended dialects.  The result is a
:cfunc:`BooleanCircuit` (with the d-DNNF invariants) that gets
returned to the caller and walked by
:cfunc:`dDNNF::probabilityEvaluation`.

The Panini compiler from KCBox ships with five target-language
modes (``OBDD``, ``OBDD[AND]``, ``Decision-DNNF``, ``R2-D2``,
``CCDD``) selected by the ``--lang`` flag. ProvSQL exposes the
first three; the ``R2-D2`` and ``CCDD`` languages are rejected
upstream because both emit ``K`` (kernelize) nodes encoding
literal-equivalence constraints over a shared kernel variable,
breaking the decomposability invariant of the resulting d-DNNF.
Panini's output is not the NNF text format but a CDD-style
DOT-like syntax; the dedicated parser in
:cfunc:`BooleanCircuit::paniniCompile` translates each ``C`` /
``D`` line into a decomposable AND and each ``v ? t : f`` decision
into an explicit OR-of-AND-NOT structure over the corresponding
input gate. A ``K`` node, if seen, raises an explicit error.

After any external-compiler call, :cfunc:`dDNNF::simplify` runs a
single canonical pass over the result: empty-constant folding,
short-circuiting on opposite-type empty children, and single-child
AND / OR collapse.  The same pass is run on the in-process
tree-decomposition route and on :cfunc:`BooleanCircuit::interpretAsDD`,
so callers see a structurally canonical d-DNNF regardless of which
backend produced it.

Helper Surfaces (Studio and SQL Introspection)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Four small SQL helpers expose intermediate pipeline artifacts to
the user and to Studio:

- :cfile:`tseytin_cnf.cpp` (:sqlfunc:`tseytin_cnf`) returns the
  DIMACS CNF that the external compilers would otherwise receive.
- :cfile:`tree_decomposition_dot.cpp`
  (:sqlfunc:`tree_decomposition_dot`) renders the min-fill tree
  decomposition as GraphViz DOT, with the treewidth and a per-bag
  input map embedded as comment lines so consumers can resolve a
  bag variable back to its provenance UUID.
- :cfile:`compile_to_ddnnf_dot.cpp`
  (:sqlfunc:`compile_to_ddnnf_dot`) goes one step further: it runs
  the requested compiler (or the meta-routes ``tree-decomposition``,
  ``interpret-as-dd``, ``default`` -- all dispatched through
  :cfunc:`BooleanCircuit::makeDD`) and returns the resulting d-DNNF
  itself as DOT.
- :cfile:`tool_available.cpp` (:sqlfunc:`tool_available`) is a thin
  wrapper around :cfunc:`find_external_tool` so a SQL client can
  check whether a given tool resolves on the backend's PATH plus
  ``provsql.tool_search_path`` -- used by Studio to grey out
  unselectable compilers in the picker.

None of these helpers participate in the probability dispatcher;
they are purely introspection surfaces sharing the same Tseytin /
NNF / tree-decomposition primitives as the production methods.

WeightMC: Approximate Weighted Model Counting
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

:cfunc:`BooleanCircuit::WeightMC` uses a different external tool:
the *WeightMC* approximate weighted model counter.  Unlike a
knowledge compiler it does not produce a d-DNNF; instead it
returns a single approximate probability with statistical
guarantees.  The user passes the desired precision as a
``"delta;epsilon"`` string, which is parsed and turned into a
``--pivotAC`` argument that controls how many random hash
constraints WeightMC will sample.

The input is again the Tseytin CNF (with weights); the output is
a single line that the function parses and returns as a
``double``.

Tree Decomposition
^^^^^^^^^^^^^^^^^^

The tree-decomposition path is ProvSQL's "no external tool" route
to a d-DNNF.  Conceptually, a tree decomposition of a Boolean
circuit is a tree of *bags* (sets of variables) such that every
constraint of the circuit is captured by at least one bag, and
the bags containing each variable form a connected subtree.  The
*treewidth* is one less than the size of the largest bag; the
smaller it is, the more amenable the formula is to dynamic
programming.

:cfile:`TreeDecomposition.cpp` builds a tree decomposition of the
circuit's primal graph using a min-fill elimination heuristic,
then *normalises* it (:cfunc:`TreeDecomposition::makeFriendly`)
so that every bag has at
most two children and every leaf bag introduces exactly one
variable.  :cfile:`dDNNFTreeDecompositionBuilder.cpp` then walks
the bag tree bottom-up, enumerating per-bag truth assignments and
gluing them into a d-DNNF whose decomposability and determinism
follow from the bag-cover structure of the decomposition.  The
worst-case cost is :math:`O(2^{w+1} \cdot |\mathit{circuit}|)`,
which is why ProvSQL caps the treewidth at
:cfunc:`TreeDecomposition::MAX_TREEWIDTH` (currently 10) and
falls back to ``compilation`` with ``d4`` when that bound is
exceeded.

Both the min-fill elimination loop in the
:cfunc:`TreeDecomposition` constructor and the bottom-up d-DNNF
construction in
:cfunc:`dDNNFTreeDecompositionBuilder::builddDNNF` call
``CHECK_FOR_INTERRUPTS`` in their hot loops so that
``statement_timeout`` and ``pg_cancel_backend`` interrupt the
build promptly when the heuristic struggles on circuits close to
``MAX_TREEWIDTH``.  The macro is conditionally compiled to a
no-op in the standalone ``tdkc`` binary via a ``TDKC`` guard.


Currently Supported Methods
^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Method string
     - Implementation
   * - ``"independent"``
     - :cfunc:`BooleanCircuit::independentEvaluation` -- exact,
       linear time when every input gate appears at most once.
   * - ``"possible-worlds"``
     - :cfunc:`BooleanCircuit::possibleWorlds` -- exact enumeration
       of all :math:`2^n` worlds; capped at 64 inputs.
   * - ``"monte-carlo"``
     - :cfunc:`BooleanCircuit::monteCarlo` -- approximate via random
       sampling; takes sample count as argument.
   * - ``"weightmc"``
     - :cfunc:`BooleanCircuit::WeightMC` -- approximate weighted
       model counting via the external ``weightmc`` tool; takes
       ``delta;epsilon`` as argument.
   * - ``"tree-decomposition"``
     - Builds a :cfunc:`TreeDecomposition` (bounded by
       :cfunc:`TreeDecomposition::MAX_TREEWIDTH`) and uses
       :cfunc:`dDNNFTreeDecompositionBuilder` to construct a
       d-DNNF, then calls :cfunc:`dDNNF::probabilityEvaluation`.
   * - ``"compilation"``
     - :cfunc:`BooleanCircuit::compilation` -- invokes an external
       knowledge compiler (``d4``, ``c2d``, ``dsharp``, or
       ``minic2d``) to produce a :cfunc:`dDNNF`, then
       :cfunc:`dDNNF::probabilityEvaluation`.
   * - ``""`` (default)
     - Fallback chain: try ``independent``, then
       :cfunc:`BooleanCircuit::interpretAsDD` (interpret the circuit
       structure directly as a d-D circuit), then
       ``tree-decomposition``, then ``compilation`` with ``d4``.

The branches for ``"compilation"``, ``"tree-decomposition"``, and
the default all funnel through :cfunc:`BooleanCircuit::makeDD`,
which dispatches further on the d-DNNF construction strategy.

The external-compiler choice inside ``compilation`` is itself a
string dispatch inside :cfunc:`BooleanCircuit::compilation`.  Once
a :cfunc:`dDNNF` has been produced, probability evaluation is a
single linear-time pass
(:cfunc:`dDNNF::probabilityEvaluation`), because the d-DNNF
structure guarantees decomposability and determinism.

Cmp-Probability Pre-Passes
^^^^^^^^^^^^^^^^^^^^^^^^^^

Before the methods above run, ``probability_evaluate.cpp`` walks
the circuit through a chain of pre-passes that resolve specific
``gate_cmp`` shapes to a Bernoulli ``gate_input`` carrying a
closed-form probability.  Resolving a cmp here shrinks the
circuit fed to the downstream method ; in the best case the whole
HAVING comparator collapses to a single leaf, bypassing DNF
construction entirely.

The chain (in order) :

- :cfunc:`runRangeCheck` (also runs at load time when
  ``provsql.simplify_on_load`` is on) : support-interval propagation
  through ``gate_arith`` and decision of every ``gate_cmp``
  decidable from the support alone.  Universal across semirings,
  so it lives both at load time and inside
  ``probability_evaluate``.
- :cfunc:`runHybridDecomposer` (gated by ``provsql.hybrid_evaluation``) :
  base-RV-footprint partitioning + per-island marginalisation for
  continuous-RV cmps (see the hybrid section below).
- :cfunc:`runAnalyticEvaluator` : closed-form CDF for trivial RV cmp
  shapes (singleton bare ``gate_rv`` vs ``gate_value``, or two
  bare normals).  Probability-specific (the resulting
  ``gate_input`` carries a numeric probability with no semiring
  meaning), so it runs here and not at load time.
- :cfunc:`runCountCmpEvaluator` (gated by
  ``provsql.cmp_probability_evaluation``, hidden diagnostic
  default on) : recognises HAVING
  ``gate_cmp(gate_agg(COUNT, semimod children), gate_value(C))`` and
  replaces the cmp with a Bernoulli carrying the Poisson-binomial CDF
  ``Pr(B op C)`` over the per-row contributor marginals.  Each semimod's
  K child is that row's contributor sub-circuit -- a single
  ``gate_input``, or (for a join) a ``times`` / ``plus`` / ``monus`` of
  several leaves; a small read-once recursion (``contributorProb``)
  computes its probability.  Soundness condition : every structural gate inside a
  contributor (``input`` / ``times`` / ``plus`` / ``monus``) has
  ``ref_count == 1`` -- a single check that makes the contributors' leaf
  sets pairwise disjoint, unshared with the rest of the circuit, and
  read-once, so the Poisson-binomial trials are independent (plus
  ``ref_count(gate_agg) == 1``, catching multi-cmp HAVING over a shared
  COUNT).  The DP
  dispatches on the smaller side of ``C`` (lower tail directly,
  or upper tail via inverted Bernoullis) for ``O(N x min(C, N -
  C))`` total cost per cmp.  See ``src/CountCmpEvaluator.{h,cpp}``.

Adding another closed-form cmp resolver (MIN / MAX / SUM, future
discrete-RV distributions…) follows the same shape : a
``runXxxEvaluator`` function that walks ``gate_cmp`` gates, checks
shape + independence, computes the probability, calls
:cfunc:`GenericCircuit::resolveCmpToBernoulli`.  Gate it on
``provsql.cmp_probability_evaluation`` so all such evaluators
share one diagnostic switch.


.. _bids-and-multivalued-inputs:

Block-Independent Databases and Multivalued Inputs
--------------------------------------------------

By default, :sqlfunc:`add_provenance` associates one ``input`` gate
per tuple (created lazily on first reference), so each row of a
provenance-tracked base table is an independent Bernoulli variable.
That is the **tuple-independent** probabilistic database (TID) model.

ProvSQL additionally supports the strictly more general
**block-independent database** (BID) model, in which input
tuples are partitioned into *blocks*:

- tuples within a block are pairwise *disjoint* -- at most one
  of them is present in any possible world;
- blocks are *independent*;
- each tuple of a block has its own probability, with the
  per-block sum :math:`\le 1`; the residual :math:`1 - \sum_i p_i`
  is the probability that *no* tuple from the block is present
  (the "null outcome").

A TID is the special case where each block has exactly one
tuple.  BIDs are the natural circuit-level model for tables with
key uncertainty: "exactly one of these rows is the real row, we
don't know which, and here are the weights".

The ``gate_mulinput`` Gate
^^^^^^^^^^^^^^^^^^^^^^^^^^

ProvSQL represents each BID block in the persistent circuit by a
group of ``gate_mulinput`` gates that share a common child, an
``input`` gate acting as the *block key*.  Each ``mulinput`` gate
corresponds to one alternative of the block and carries its own
probability (set with :sqlfunc:`set_prob`).  ``mulinput`` gates
are **not** first-class leaves of the provenance DAG: semiring
evaluators do not know how to interpret them and will refuse
any circuit that contains one, and the probability pipeline
handles them only after rewriting the blocks into standard
Boolean gates -- as described below.

The canonical way to create such gates from SQL is
:sqlfunc:`repair_key`, which takes a table with duplicate key
values, allocates one fresh ``input`` gate per key group, and
turns each member of the group into a ``mulinput`` whose child
is that block key.  When no probabilities are attached,
:sqlfunc:`repair_key` defaults them to a uniform distribution over the
block members.

Rewriting Blocks into Independent Booleans
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Most probability-evaluation algorithms require a purely Boolean
circuit: AND, OR, NOT, and independent Bernoulli leaves.  A BID
block is not directly such a structure -- its elements are
mutually exclusive, not independent.
:cfunc:`BooleanCircuit::rewriteMultivaluedGates` (in
:cfile:`BooleanCircuit.cpp`) reduces every block to an
equivalent Boolean subcircuit by introducing :math:`O(\log n)`
fresh independent Bernoulli variables per block of size
:math:`n` whose *joint distribution* reproduces the original
discrete weights.

The construction is divide-and-conquer.  Given a block with
alternatives carrying cumulative probabilities
:math:`P_0 \le P_1 \le \cdots \le P_{n-1}`, the recursive helper
:cfunc:`BooleanCircuit::rewriteMultivaluedGatesRec` splits the
range :math:`[\mathit{start}, \mathit{end}]` at the midpoint
:math:`\mathit{mid}`, creates one fresh ``input`` gate ``g`` with
probability

.. math::

   \frac{P_{\mathit{mid}+1} - P_{\mathit{start}}}
        {P_{\mathit{end}} - P_{\mathit{start}}}

-- the conditional probability of landing in the left half --
and recurses twice: the left half gets ``g`` pushed onto its
prefix, the right half gets ``NOT g``.  At a leaf
(:math:`\mathit{start} = \mathit{end}`), the ``mulinput`` gate is
rewritten into the AND of the accumulated prefix, so its truth
value becomes the conjunction of the fresh-variable decisions
that lead to it.  If the block's probabilities do not sum to 1,
the outer call wraps the whole construction in one more fresh
input of probability :math:`P_{n-1}` to carry the "none of them"
residual.

After rewriting, the block's ``mulinput`` gates have been turned
into ordinary ``AND`` gates over fresh independent Boolean
inputs, and the circuit is ready for any TID-based probability
method.  The dispatcher in
:cfunc:`probability_evaluate_internal` calls
:cfunc:`BooleanCircuit::rewriteMultivaluedGates` lazily: the
``"independent"`` method handles ``mulinput`` gates natively and
runs on the raw circuit; every other method falls through to the
rewrite first.  This is the pivot point referenced in
:ref:`adding-new-probability-method` below.


.. _shapley-banzhaf:

Shapley and Banzhaf Values
--------------------------

ProvSQL also exposes *expected Shapley values* and *expected
Banzhaf values*, which quantify the individual contribution of
each input tuple to the truth of a provenance circuit.  The
user-facing interface is described in :doc:`../user/shapley`;
this section covers the implementation in :cfile:`shapley.cpp`
and :cfile:`dDNNF.cpp`.

Expected Shapley values are #P-hard in general but become
polynomial-time computable when the provenance is represented as
a *decomposable and deterministic* (d-D) Boolean circuit --
essentially a d-DNNF.  The algorithm ProvSQL uses is
Algorithm 1 of Karmakar, Monet, Senellart, and Bressan (PODS
2024, :cite:`DBLP:journals/pacmmod/KarmakarMSB24`), specialised
to the two coefficient functions that define the Shapley and
Banzhaf scores.  Both scores are computed *in expectation*: the
random subset of variables is drawn according to the
per-variable probabilities of the circuit, and when no
probabilities have been set, each defaults to 1 and the
computation collapses to the standard deterministic Shapley /
Banzhaf value.

Entry Point
^^^^^^^^^^^

:sqlfunc:`shapley` / :sqlfunc:`banzhaf` (and their set-returning
variants :sqlfunc:`shapley_all_vars` /
:sqlfunc:`banzhaf_all_vars`) are thin wrappers that unpack their
arguments and call :cfunc:`shapley_internal` in
:cfile:`shapley.cpp`.  That helper performs the following
sequence:

1. Build a :cfunc:`BooleanCircuit` from the persistent store via
   :cfunc:`getBooleanCircuit`.
2. Build a :cfunc:`dDNNF` by calling
   :cfunc:`BooleanCircuit::makeDD`.  This is the same d-DNNF
   construction used for ordinary probability evaluation, and
   obeys the same ``method`` / ``args`` conventions.
3. :cfunc:`dDNNF::makeSmooth` -- ensure that every OR gate's
   children mention the same variable set.  The paper's
   algorithm assumes a *smooth* d-DNNF.
4. For Shapley (but not Banzhaf):
   :cfunc:`dDNNF::makeGatesBinary` on AND -- binarise AND gates
   so each has at most two children.  Together, the previous
   two steps turn the d-DNNF into a *tight* d-D circuit in the
   paper's sense.
5. Call :cfunc:`dDNNF::shapley` or :cfunc:`dDNNF::banzhaf` on
   the target variable's gate.

The Shapley Recurrence
^^^^^^^^^^^^^^^^^^^^^^

The paper's algorithm conditions the circuit on the target
variable being fixed to ``true`` (call the result :math:`C_1`)
and to ``false`` (call it :math:`C_0`), computes a pair of
per-gate arrays on each conditioned circuit, and combines them
into the final score.  ProvSQL's :cfunc:`dDNNF::shapley`
mirrors that structure:

.. code-block:: cpp

   double dDNNF::shapley(gate_t var) const {
     auto cond_pos = condition(var, true);   // C_1
     auto cond_neg = condition(var, false);  // C_0

     auto alpha_pos = cond_pos.shapley_alpha();
     auto alpha_neg = cond_neg.shapley_alpha();

     double result = 0.;
     for (size_t k = ...; k < alpha_pos.size(); ++k)
       for (size_t l = 0; l <= k; ++l) {
         double pos = alpha_pos[k][l];
         double neg = alpha_neg[k][l];
         result += (pos - neg) / comb(k, l) / (k + 1);
       }
     result *= getProb(var);
     return result;
   }

:cfunc:`dDNNF::condition` returns a copy of the circuit in which
the target input gate has been replaced by an ``AND`` /
``OR``-with-no-children acting as the constant ``true`` /
``false`` respectively.  The private helper
:cfunc:`dDNNF::shapley_alpha` then performs a single bottom-up pass
computing a two-dimensional array
:math:`\beta^g_{k,\ell}` (called ``result[g]`` in the code) at
every gate :math:`g`, where :math:`k` is the number of variables
under :math:`g` in the current cofactor and :math:`\ell` is the
number of them that are positively assigned.  The recurrences
follow the ``IN`` / ``NOT`` / ``OR`` / ``AND`` cases of
Algorithm 1 of the paper:

- At a leaf, the array encodes the Bernoulli distribution of
  that single variable.
- At an OR gate, the arrays of the children are summed coordinatewise
  (valid because the d-DNNF is *smooth*, so all children have
  the same variable set).
- At a binary AND gate, the arrays are *convolved* via a double
  sum over :math:`(k_1, \ell_1)` pairs -- the decomposability of
  AND makes this the Cauchy product of two independent
  distributions.  This convolution is the reason AND gates have
  to be binarised before the algorithm runs.
- A standalone bottom-up pass
  (:cfunc:`dDNNF::shapley_delta`) precomputes the :math:`\delta^g_k`
  polynomials, which the algorithm uses at ``NOT`` gates to turn
  negation into a coefficient flip.

The final score is
:math:`p_x \cdot \sum_{k, \ell} c_{\text{Shapley}}(k+1, \ell)
\cdot (\beta^{g_{\text{out}}}_{k,\ell} - \gamma^{g_{\text{out}}}_{k,\ell})`,
where :math:`\beta^{g_{\text{out}}}` comes from :math:`C_1` and
:math:`\gamma^{g_{\text{out}}}` from :math:`C_0`, and
:math:`c_{\text{Shapley}}(k+1, \ell) = \binom{k}{\ell}^{-1} / (k+1)`
is the Shapley coefficient --
i.e.\ exactly the formula implemented above.  The overall
complexity is :math:`O(|C| \cdot |V|^5)` arithmetic operations,
dominated by the double-sum convolution at AND gates over the
:math:`|V|^2`-sized arrays.

The ``if (isProbabilistic())`` guards inside
:cfunc:`dDNNF::shapley_alpha` and :cfunc:`dDNNF::shapley_delta`
short-circuit the polynomials to a single
top-level coefficient when all input probabilities are 1, so that
the same code path computes classical (deterministic) Shapley
values without paying the expected-score overhead.

Banzhaf
^^^^^^^

The expected Banzhaf value admits a much simpler formula
:cite:`DBLP:journals/pacmmod/KarmakarMSB24`:

.. math::

   \operatorname{EScore}_{\text{Banzhaf}}(\varphi, x) =
     p_x \cdot \bigl( \mathrm{ENV}(C_1) - \mathrm{ENV}(C_0) \bigr)

where :math:`\mathrm{ENV}(\varphi) = \sum_{Z \subseteq V} \Pi_V(Z)
\sum_{E \subseteq Z} \varphi(E)` can be computed in a *single*
linear pass over a smooth d-D circuit without binarising AND
gates.  :cfunc:`dDNNF::banzhaf` runs
:cfunc:`dDNNF::banzhaf_internal` on
the two conditioned circuits :math:`C_1` and :math:`C_0` and
returns the difference times :math:`p_x`; the overall
complexity is :math:`O(|C| \cdot |V|)`, one factor of :math:`|V|`
less than Shapley.  This is why :cfunc:`shapley_internal` skips
the :cfunc:`dDNNF::makeGatesBinary` call in the Banzhaf branch.


Hybrid Evaluation for Continuous Distributions
----------------------------------------------

When the circuit being evaluated contains continuous gates
(``gate_rv``, ``gate_arith``, ``gate_mixture``),
a hybrid evaluator runs *before* the Boolean dispatch above. Its
job is to fold every sub-circuit that has a closed-form analytical
answer into a Bernoulli leaf so the resulting circuit is a normal
Boolean circuit ready for any of the Boolean methods.

The hybrid evaluator has three passes:

- **Peephole pruning** (:cfunc:`runRangeCheck`): support intervals
  propagate through ``gate_arith``, every ``gate_cmp``
  is tested against the propagated interval, and every comparator
  decidable from the support alone collapses to a Bernoulli
  ``gate_input`` with probability ``0`` or ``1``.
- **Family-closure simplifier** (:cfunc:`runHybridSimplifier`):
  linear combinations of independent normals fold into a single
  normal; sums of i.i.d. exponentials with the same rate fold
  into an Erlang; identity / single-child arith gates and
  semiring identities collapse.
- **Island decomposition** (:cfunc:`runHybridDecomposer`):
  the remaining cmps are partitioned by base-RV footprints into
  *islands*; single-cmp islands marginalise via
  :cfunc:`runAnalyticEvaluator`'s closed-form CDF; multi-cmp islands
  with shared base RVs go through the joint table.

See :doc:`continuous-distributions` for the full simplifier rule
set and the island-decomposition algorithm.

Conditional Evaluation
----------------------

:sqlfunc:`expected` / :sqlfunc:`variance` / :sqlfunc:`moment` /
:sqlfunc:`central_moment` / :sqlfunc:`support` / :sqlfunc:`rv_sample` /
:sqlfunc:`rv_histogram` all accept an optional ``prov uuid`` argument
that conditions the moment, sample, or histogram on the provenance
event ``prov``. When ``prov`` resolves to anything other than
:sqlfunc:`gate_one`, evaluation routes through the joint-circuit
loader :cfunc:`getJointCircuit` (:cfile:`MMappedCircuit.cpp`),
which performs a multi-rooted BFS over the union of the reachable
gates from both ``input`` and ``prov`` so shared ``gate_rv``
leaves between the two are loaded into a single
:cfunc:`GenericCircuit` and consequently couple correctly in the
Monte Carlo sampler's ``rv_cache_``. The closed-form
truncated-distribution table is exhaustive for Normal (Mills
ratio), Uniform (intersected support), and Exponential
(memorylessness); other shapes fall back to MC rejection sampling
at ``provsql.rv_mc_samples`` budget. See
:doc:`continuous-distributions` for depth.

.. _inversion-free-path:

The Inversion-Free ``UCQ(OBDD)`` Path
-------------------------------------

The ``'inversion-free'`` method (and the default-chain rung that follows
``independent``) evaluates the *inversion-free* class of Jha, Olteanu and
Suciu :cite:`DBLP:conf/edbt/JhaOS10`: hierarchical, tuple-independent
queries -- including self-joins -- whose lineage admits a polynomial-size
OBDD. On these the generic ``'tree-decomposition'`` / compilation
fallbacks can blow up (the lineage is not low-treewidth), yet a
*structured* d-DNNF built over a query-derived variable order stays
linear in the lineage.

This path is a sibling of the :ref:`safe-query-rewriter`, and the two are
complementary:

- The safe-query rewriter (``provsql.boolean_provenance`` on) restructures
  the *query* so the planner emits a read-once *circuit*, which
  ``independent`` then evaluates almost for free. It applies only to the
  read-once (safe) class and changes the produced circuit.
- The inversion-free path leaves the lineage intact and evaluates the
  *naive* circuit -- which, even for a safe query, is generally **not**
  read-once (e.g. ``q(x) :- B(x), A(x,y)`` yields ``⋁_y (B(x) ∧ A(x,y))``,
  repeating ``B(x)``), so ``independent`` rejects it. It also covers the
  strictly larger inversion-free-but-not-read-once self-join class. It is
  decoupled from ``boolean_provenance`` and gated on its own GUC,
  ``provsql.inversion_free`` (on by default).

The pipeline has four stages.

Detection (``src/safe_query.c``)
   :cfunc:`detect_inversion_free` checks the four preconditions
   (hierarchical, strictly tuple-independent atoms, positional
   consistency, acyclic precedence) and, on success, builds a
   :cfunc:`SafeCert` recipe describing the query-derived (Prop. 4.5)
   variable order. It reuses the candidate gate and union-find machinery
   of the safe-query rewriter but is *not* gated on
   ``boolean_provenance``: :cfunc:`process_query` runs it on the lineage
   query whenever ``provsql.inversion_free`` is on, after (and only when)
   the read-once rewrite did not already fire. On PostgreSQL 18 a
   ``GROUP BY`` query carries a synthetic ``RTE_GROUP`` that hides the
   base atoms, so detection runs on a stripped copy while the lineage is
   built on the original query.

Certificate and per-input markers (``src/safe_query_cert.{h,c}``)
   The recipe and the order are carried into the circuit on transparent
   ``gate_annotation`` gates (see :doc:`architecture`):

   - the serialised :cfunc:`SafeCert` is stamped on the per-row root as a
     ``C``-prefixed ``extra`` payload;
   - each certified atom's input is wrapped (via :sqlfunc:`annotate`) in an
     annotation carrying a ``K``-prefixed *order key* ``(root, sec, factor)``
     (:cfunc:`SafeCertKey`), emitted by the planner
     (:cfunc:`build_inversion_free_marker` in ``src/provsql.c``) via the
     :sqlfunc:`inversion_free_key` SQL function. An atom binding only the
     head class is *root-only* (no secondary column); a relation whose
     occurrences span two or more secondary classes is the shared
     self-join *guard* (``factor = SAFE_CERT_GUARD_FACTOR``).

   Both markers are inert at evaluation: the annotation gate is identity
   for every evaluator, so a query carrying them evaluates identically
   whether or not the analysis ran.

Structured d-DNNF builder (``src/StructuredDNNF.{h,cpp}``)
   :cfunc:`StructuredDNNFBuilder` compiles the monotone lineage top-down
   into a ProvSQL :cfunc:`dDNNF`: it expands the circuit to a canonical
   DNF and recurses with **decomposable AND** at independence points
   (variable-disjoint factors) and **deterministic OR** at Shannon
   decisions on the supplied variable order, threading a *false-sink*
   through OR-chains and sharing equal sub-d-DNNFs through a component
   cache. The order affects only the d-DNNF *size*, never correctness, so
   the builder is sound on any monotone lineage; the Prop. 4.5 order is
   what keeps it polynomial on the certified class. Multivalued (BID) and
   ``NOT`` gates are out of scope and rejected with a
   :cfunc:`CircuitException`.

Dispatch (``src/probability_evaluate.cpp``)
   :cfunc:`collect_inversion_free_keys` walks the circuit for the
   ``K``-marker annotations and maps each wrapped input to its
   :cfunc:`InputKey`; :cfunc:`inversion_free_rank` flattens those keys
   into a total rank (root value, then secondary value, then
   guard-before-payload, then factor) for the order-only builder. The
   explicit ``'inversion-free'`` method requires the certificate and
   errors without it; the default chain takes this rung only when a
   certificate is present and ``provsql.inversion_free`` is on, after
   ``independent`` and before tree-decomposition, catching
   :cfunc:`CircuitException` to fall through.

Atoms the analysis does not model (a BID/``gate_mulinput`` atom, a
derived/view atom, which is not an ``RTE_RELATION``) cause detection to
decline (no certificate), and a malformed ``C``-prefixed payload fails to
parse and is treated as an inert annotation; in every case evaluation
falls back to the normal chain and stays correct. These declines are
covered by ``test/sql/safe_query_inversion_free.sql``.

.. _adding-new-probability-method:

Step-by-Step: Adding a New Probability Evaluation Method
--------------------------------------------------------

The work is almost entirely in two files.  Pick a short, descriptive
method string -- it is the value the user passes to
:sqlfunc:`probability_evaluate`.

1. **Declare the method** on :cfile:`BooleanCircuit.h`:

   .. code-block:: cpp

      double myMethod(gate_t g, const std::string &args) const;

2. **Implement it** in :cfile:`BooleanCircuit.cpp`.  The method
   receives the root gate and the user-supplied ``args`` string (may
   be empty) and must return a probability in :math:`[0, 1]`.  Check
   :cfunc:`provsql_interrupted` periodically if the computation is
   long so that the user can cancel with ``Ctrl-C``:

   .. code-block:: cpp

      double BooleanCircuit::myMethod(gate_t g, const std::string &args) const {
        // Parse args if needed.
        // Run the algorithm, respecting provsql_interrupted.
        // Return the probability.
      }

3. **Add a dispatch branch** in :cfunc:`probability_evaluate_internal`
   in :cfile:`probability_evaluate.cpp`.  The exact location depends
   on the method's characteristics:

   - If the algorithm requires a *Boolean* circuit (no multivalued
     inputs -- see :ref:`bids-and-multivalued-inputs`), add the
     branch **after** the call to
     :cfunc:`BooleanCircuit::rewriteMultivaluedGates`.  That is the
     case for most approximate methods.
   - If the algorithm operates directly on the raw circuit (like
     ``independent``), add it **before**
     :cfunc:`BooleanCircuit::rewriteMultivaluedGates`.
   - If the algorithm produces a d-DNNF and you want it to benefit
     from the linear-time d-DNNF evaluator, add it to
     :cfunc:`BooleanCircuit::makeDD` instead and route your dispatch
     branch through :cfunc:`BooleanCircuit::makeDD`.

   Example for an approximate method that takes a numeric argument:

   .. code-block:: cpp

      } else if(method == "mymethod") {
        int param;
        try { param = std::stoi(args); }
        catch(const std::invalid_argument &) {
          provsql_error("mymethod requires a numeric argument");
        }
        result = c.myMethod(gate, param);
      }

4. **(Optional) Extend the default fallback chain.**  If the method
   is a good universal choice, update :cfunc:`BooleanCircuit::makeDD`
   and/or the default branch in :cfunc:`probability_evaluate_internal`
   to try it before falling back to ``compilation`` with ``d4``.

5. **Add a regression test** under ``test/sql/`` and register it in
   ``test/schedule.common``.  Follow the skip-if-missing pattern
   from the other external-tool tests (see :doc:`testing`) if the
   new method depends on an external binary.

6. **Update the user documentation** in
   :doc:`../user/probabilities` and add a row for the new method to
   the "Currently supported methods" table above.
