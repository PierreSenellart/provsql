Adding a New Probability Evaluation Method
==========================================

ProvSQL computes probabilities by reducing a provenance circuit to
Boolean form and then dispatching to one of several evaluation
methods.  This page explains the dispatch architecture and gives a
step-by-step guide for adding a new method.  See
:doc:`../user/probabilities` for the user-facing description of the
existing methods.


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


Step-by-Step: Adding a New Method
---------------------------------

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
     inputs), add the branch **after** the call to
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
