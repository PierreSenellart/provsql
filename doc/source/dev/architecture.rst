Architecture Overview
=====================

This page gives a bird's-eye view of ProvSQL's internals: how the
extension is loaded, how its components are organized, and how data
flows from an SQL query to a provenance evaluation result.  For a
detailed walkthrough of the query rewriting pipeline, see
:doc:`query-rewriting`.

Extension Lifecycle
-------------------

ProvSQL is a PostgreSQL *shared-library extension*.  Because it installs
a **planner hook**, the library must be loaded at server start via the
``shared_preload_libraries`` configuration variable; it cannot be loaded
on demand.

When PostgreSQL starts, it calls :cfunc:`_PG_init`, which:

1. Registers the GUC (Grand Unified Configuration) variables; the
   most central are:

   - ``provsql.active`` -- enable/disable provenance tracking (default: on).
   - ``provsql.provenance`` -- the provenance class: 'where' / 'semiring' (default) / 'absorptive' / 'boolean'.
   - ``provsql.update_provenance`` -- track provenance through DML
     statements (default: off).
   - ``provsql.verbose_level`` -- verbosity for debug messages (0--100,
     default: 0).
   - ``provsql.aggtoken_text_as_uuid`` -- when on, ``agg_token_out`` emits
     the underlying provenance UUID instead of the default ``"value (*)"``
     display string (default: off).
   - ``provsql.tool_search_path`` -- colon-separated directories prepended
     to ``PATH`` when invoking external tools (d4, c2d, minic2d, dsharp,
     weightmc, graph-easy); see :cfile:`external_tool.cpp`.

   Further GUCs gate individual routes and optimizations
   (``provsql.joint_width``, ``provsql.mobius``,
   ``provsql.simplify_on_load``, ...); grep :cfile:`provsql.c` for
   ``DefineCustom`` for the full list, and see :doc:`/user/configuration`
   for the user-facing reference.

2. Installs the **planner hook** (:cfunc:`provsql_planner`) by saving
   the previous hook in ``prev_planner`` and replacing ``planner_hook``.

3. Installs the **ProcessUtility hook**
   (:cfunc:`provsql_ProcessUtility`) by saving the previous hook in
   ``prev_ProcessUtility``.  Used by the CTAS / ``SELECT INTO`` /
   matview lineage propagation path (see
   :ref:`tid-bid-propagation`).

4. Installs **shared-memory hooks** for inter-process coordination
   (see :doc:`memory`).

5. Installs **executor hooks** (``ExecutorStart`` / ``ExecutorEnd``)
   that maintain a nesting depth counter so the
   :cfunc:`provsql_classify_query` NOTICE fires only on the user's
   outermost statement (the rewriter's PL/pgSQL helpers replan
   internally, which would otherwise produce spurious extra
   NOTICEs).

6. Launches the **mmap background worker** that manages persistent
   circuit storage.

When the server shuts down, :cfunc:`_PG_fini` restores the previous
planner, ``ProcessUtility``, executor, and shared-memory hooks.


Component Map
-------------

ProvSQL is a mixed C/|cpp| codebase.  The PostgreSQL interface layer is
written in C (required by the extension API); complex data structures
and algorithms are in |cpp|.

**C files** (PostgreSQL interface layer):

*Planner hook and query rewriting*

- :cfile:`provsql.c` -- planner hook, ProcessUtility hook
  (CTAS / ``SELECT INTO`` / matview lineage), executor hooks,
  and the bulk of the query rewriting logic (~15,000 lines).
- :cfile:`safe_query.c` / :cfile:`safe_query.h` -- safe-query
  rewriter for hierarchical CQs (Dalvi & Suciu 2012), gated on
  the ``'boolean'`` provenance class ; includes the FD-aware
  extensions (constant-selection elimination, PK FDs,
  deterministic-relation transparency, PK-unifiable /
  disjoint-constant self-joins) and the propagation
  normalisation pre-passes (``INNER`` / ``CROSS`` JoinExpr
  flattening, simple-subquery inlining).  See
  :ref:`safe-query-rewriter`.
- :cfile:`classify_query.c` / :cfile:`classify_query.h` --
  query-time TID / BID / OPAQUE classifier exposed through the
  ``provsql.classify_top_level`` GUC.
- :cfile:`qual_classify.c` / :cfile:`qual_classify.h` -- shared
  predicate-tree helpers (AND flattening, equijoin / constant-selection
  recognition, per-relation qual splitting) consumed by both the
  safe-query rewriter and the joint-width recogniser.
- :cfile:`joint_width_query.c` -- planner-time recogniser for the
  joint-width and Möbius UCQ routes (descriptor extraction, transparent
  ``ucq_joint_provenance`` / ``ucq_mobius_provenance`` substitution).

*Utilities and shared state*

- :cfile:`provsql_utils.c` / :cfile:`provsql_utils.h` -- OID cache
  (:cfunc:`get_constants`), type helpers, gate-type enum.
- :cfile:`provsql_error.h` -- ``provsql_error`` / ``_warning`` /
  ``_notice`` / ``_log`` macros.
- :cfile:`c_cpp_compatibility.h` -- small shims for mixing C and C++
  sources.

*Memory-mapped circuit store*

- :cfile:`provsql_mmap.c` / :cfile:`provsql_mmap.h` -- background
  worker and IPC primitives.
- :cfile:`provsql_shmem.c` / :cfile:`provsql_shmem.h` -- shared-memory
  segment setup.

*SQL-callable functions*

- :cfile:`provenance.c` -- error stub for the :sqlfunc:`provenance`
  SQL function (reached only when a query bypasses the planner hook).
- :cfile:`provenance_evaluate.c` -- SQL-level semiring evaluation
  (user-defined ``plus``/``times``/... functions).
- :cfile:`agg_token.c` / :cfile:`agg_token.h` -- the ``agg_token``
  composite type (UUID + running value).

*PostgreSQL version compatibility*

- :cfile:`compatibility.c` / :cfile:`compatibility.h` -- shims for
  cross-version PostgreSQL API differences.

**C++ files** (data structures and algorithms):

*Circuit representation*

- :cfile:`Circuit.h` / :cfile:`Circuit.hpp` -- template base class
  parameterised by gate type; inherited by all circuit variants.
- :cfile:`GenericCircuit.h` / :cfile:`GenericCircuit.hpp` /
  :cfile:`GenericCircuit.cpp` -- semiring-agnostic in-memory circuit.
- :cfile:`BooleanCircuit.h` / :cfile:`BooleanCircuit.cpp` -- Boolean
  circuit used for knowledge compilation and probability evaluation.
- :cfile:`WhereCircuit.h` / :cfile:`WhereCircuit.cpp` --
  where-provenance circuit.
- :cfile:`DotCircuit.h` / :cfile:`DotCircuit.cpp` -- GraphViz DOT
  export of circuits.

*Persistent storage and in-memory reconstruction*

- :cfile:`MMappedCircuit.h` / :cfile:`MMappedCircuit.cpp` --
  mmap-backed persistent circuit store.
- :cfile:`CircuitFromMMap.h` / :cfile:`CircuitFromMMap.cpp` -- reads
  the mmap store to build in-memory :cfunc:`GenericCircuit` /
  :cfunc:`BooleanCircuit` instances.
- :cfile:`CircuitCache.h` / :cfile:`CircuitCache.cpp` /
  :cfile:`circuit_cache.h` -- per-session gate cache.
- :cfile:`MMappedUUIDHashTable.h` / :cfile:`MMappedUUIDHashTable.cpp`
  -- open-addressing hash table keyed by UUID, stored in mmap.
- :cfile:`MMappedVector.h` / :cfile:`MMappedVector.hpp` --
  ``std::vector``-like container over an mmap region.
- :cfile:`MMappedTableInfo.h` -- the per-relation
  :cfunc:`ProvenanceTableInfo` record (TID / BID / OPAQUE kind,
  multi-column BID block-key, base-ancestor set) ; see
  :ref:`per-table-metadata`.

*Semiring evaluation*

- ``semiring/*.h`` -- header-only semiring implementations (Boolean,
  BoolExpr, Counting, Formula, How, IntervalUnion, Lukasiewicz,
  MinMax, Tropical, Viterbi, Which, Why).
- :cfile:`provenance_evaluate_compiled.cpp` /
  :cfile:`provenance_evaluate_compiled.hpp` -- dispatcher for
  compiled semirings.
- :cfile:`having_semantics.cpp` / :cfile:`having_semantics.hpp` --
  pre-evaluation of ``HAVING`` sub-circuits before the main semiring
  traversal.

*Aggregation*

- :cfile:`Aggregation.h` / :cfile:`Aggregation.cpp` -- aggregate
  operator enum, accumulator interface, and built-in accumulators
  (see :doc:`aggregation`).
- :cfile:`aggregation_evaluate.c` -- SQL entry points for
  aggregate-provenance evaluation.

*Continuous random variables* (see :doc:`continuous-distributions`)

- :cfile:`RandomVariable.h` / :cfile:`RandomVariable.cpp` /
  :cfile:`random_variable_type.c` -- the ``random_variable`` type,
  the ``DistributionSpec`` POD, and the ``gate_rv`` blob parser.
- ``distributions/`` -- the registry-driven per-family
  ``Distribution`` class hierarchy (one self-registering file per
  family).
- :cfile:`MonteCarloSampler.h` / :cfile:`MonteCarloSampler.cpp` --
  the Monte Carlo sampler.
- :cfile:`RangeCheck.h` / :cfile:`RangeCheck.cpp` -- support-interval
  propagation and comparator resolution.
- :cfile:`AnalyticEvaluator.h` / :cfile:`AnalyticEvaluator.cpp` --
  closed-form CDF resolution for ``gate_cmp``.
- :cfile:`Expectation.h` / :cfile:`Expectation.cpp` -- analytical
  moment evaluator (with :cfile:`PivotIntegration.h`, the shared
  quadrature core).
- :cfile:`HybridEvaluator.h` / :cfile:`HybridEvaluator.cpp` --
  peephole simplifier and island decomposition.
- :cfile:`InformationTheory.h` / :cfile:`InformationTheory.cpp` --
  entropy / KL / mutual-information readouts.

*External tools and KCMCP* (see :doc:`kc-server-protocol`)

- :cfile:`external_tool.cpp` -- external-tool resolution and
  invocation (``provsql.tool_search_path``, process groups).
- :cfile:`ToolRegistry.cpp` -- the compiled-in seed of the
  ``provsql.tools`` registry.
- :cfile:`kcmcp_protocol.h` / :cfile:`kcmcp_protocol.cpp` -- shared
  KCMCP wire codec.
- :cfile:`kcmcp_client.cpp` -- in-extension KCMCP client.
- :cfile:`kcmcp_supervisor.c` -- background worker supervising the
  managed KCMCP server.
- :cfile:`kcmcp_server.cpp` / :cfile:`dimacs_cnf.cpp` -- the
  ``tdkc --kcmcp`` reference server.

*Build-configuration and storage abstraction*

- :cfile:`provsql_config.h` -- the ``PROVSQL_INPROCESS_STORE`` /
  ``PROVSQL_NO_SUBPROCESS`` switches for the WASM / in-process
  build (see :doc:`playground`).
- :cfile:`MappedRegion.h` -- backing abstraction for the mmap
  regions (kernel-shared mmap vs. heap buffer).

*Data-decomposition compilers (reachability, joint-width, Möbius)*

- :cfile:`ReachabilityCompiler.cpp` / :cfile:`reachability_evaluate.cpp`
  -- bounded-treewidth reachability compiler (single-target,
  all-targets, bounded-hop, any-reach, k-terminal DPs) and its
  columnar SQL entry points.
- :cfile:`UCQJointCompiler.cpp` / :cfile:`JointEncoding.cpp` /
  :cfile:`ucq_joint_evaluate.cpp` -- joint-width UCQ compiler
  (encoding, merged data+circuit DP, per-answer single sweep) and its
  materialisation entry points.
- :cfile:`mobius_evaluate.cpp` -- Möbius-inversion compiler for safe
  UCQs (CNF lattice, lifted-inference recursion, ``gate_mobius``
  construction).
- :cfile:`CertifiedDDMaterialize.cpp` -- shared content-addressed
  materialisation of certified d-D circuits into the mmap store.

*Probability, Shapley, knowledge compilation*

- :cfile:`probability_evaluate.cpp` -- probability-method dispatcher
  (see :doc:`probability-evaluation`).
- :cfile:`dDNNF.h` / :cfile:`dDNNF.cpp` -- d-DNNF data structure and
  linear-time probability evaluation.
- :cfile:`StructuredDNNF.h` / :cfile:`StructuredDNNF.cpp` --
  vtree-structured DNNF used by the inversion-free OBDD route.
- :cfile:`dDNNFTreeDecompositionBuilder.h` /
  :cfile:`dDNNFTreeDecompositionBuilder.cpp` -- constructs a d-DNNF
  from a tree decomposition.
- :cfile:`TreeDecomposition.h` / :cfile:`TreeDecomposition.cpp` --
  tree decomposition via min-fill elimination.
- :cfile:`TreeDecompositionKnowledgeCompiler.cpp` -- the standalone
  ``tdkc`` binary built by ``make tdkc``.
- :cfile:`provsql_migrate_mmap.cpp` -- the standalone
  ``provsql_migrate_mmap`` binary built by ``make provsql_migrate_mmap``
  (migrates pre-1.3.0 flat mmap files to the per-database layout).
- :cfile:`shapley.cpp` -- Shapley and Banzhaf value computation.

*Export and visualization*

- :cfile:`view_circuit.cpp` -- SQL :sqlfunc:`view_circuit` function
  (renders a DOT graph via ``graph-easy``).
- :cfile:`to_prov.cpp` -- PROV-XML export.
- :cfile:`where_provenance.cpp` -- SQL where-provenance output
  function.

*C++ utilities*

- :cfile:`provsql_utils_cpp.h` / :cfile:`provsql_utils_cpp.cpp` --
  C++ counterparts to :cfile:`provsql_utils.h`, including UUID
  string conversion.
- :cfile:`subset.cpp` / :cfile:`subset.hpp` -- subset enumeration
  used by HAVING evaluation.
- :cfile:`Graph.h` -- lightweight graph helpers used by the tree
  decomposition code.
- :cfile:`PermutationStrategy.h` -- pluggable vertex-ordering
  heuristic for tree decomposition.
- :cfile:`flat_map.hpp` / :cfile:`flat_set.hpp` -- contiguous
  associative containers used inside hot loops.


Data Flow
---------

The end-to-end flow of a query through ProvSQL:

.. graphviz::

   digraph dataflow {
     rankdir=LR;
     node [shape=box, fontname="sans-serif", fontsize=11];
     edge [fontsize=9, fontname="sans-serif"];

     sql [label="SQL query", shape=ellipse];
     planner [label="provsql_planner"];
     rewrite [label="process_query\n(rewriting)"];
     exec [label="PostgreSQL\nexecutor"];
     circuit [label="Circuit\n(mmap storage)"];
     eval [label="Semiring\nevaluation"];
     result [label="Query result\n+ provenance", shape=ellipse];

     sql -> planner [label="Query tree"];
     planner -> rewrite [label="has provenance?"];
     rewrite -> exec [label="rewritten query"];
     exec -> circuit [label="UUID tokens\n(gate creation)"];
     exec -> result [label="tuples + UUIDs"];
     circuit -> eval [label="circuit DAG"];
     eval -> result [label="semiring values\nor probabilities"];
   }

1. The user submits an SQL query.  PostgreSQL parses it into a ``Query``
   tree and calls the planner.

2. :cfunc:`provsql_planner` intercepts the call.  If the query touches
   provenance-tracked tables (detected by :cfunc:`has_provenance`), it
   calls :cfunc:`process_query` to rewrite it.

3. The rewritten query carries an extra UUID expression in its target
   list.  When the executor evaluates the query, it calls ProvSQL's
   SQL-level functions (``provenance_times``, ``provenance_plus``, etc.)
   to construct circuit gates.  These calls route through the mmap
   worker to persist the circuit.

4. Each result tuple comes back with a UUID identifying the root gate
   of its provenance sub-circuit.

5. To *evaluate* provenance, the user calls functions like
   :sqlfunc:`provenance_evaluate`, :sqlfunc:`probability_evaluate`, or a
   compiled semiring evaluator (e.g., :sqlfunc:`sr_boolean`).  These
   retrieve the circuit from mmap, build an in-memory
   :cfunc:`GenericCircuit` or :cfunc:`BooleanCircuit`, and traverse
   the DAG applying semiring operations.


The OID Cache: ``constants_t``
------------------------------

PostgreSQL identifies types, functions, and operators by their Object
Identifiers (OIDs).  ProvSQL needs to reference its own types and
functions when constructing rewritten query trees, so it caches their
OIDs in a :cfunc:`constants_t` structure.

Key fields:

- **Type OIDs**: ``OID_TYPE_UUID``, ``OID_TYPE_AGG_TOKEN``,
  ``OID_TYPE_GATE_TYPE``, and standard types (``BOOL``, ``INT``,
  ``FLOAT``, ``VARCHAR``).
- **Function OIDs**: ``OID_FUNCTION_PROVENANCE_PLUS``,
  ``OID_FUNCTION_PROVENANCE_TIMES``, ``OID_FUNCTION_PROVENANCE_MONUS``,
  ``OID_FUNCTION_PROVENANCE_DELTA``, ``OID_FUNCTION_PROVENANCE_AGGREGATE``,
  ``OID_FUNCTION_PROVENANCE_SEMIMOD``, etc.
- **Gate-type mapping**: ``GATE_TYPE_TO_OID[nb_gate_types]`` maps each
  :cfunc:`gate_type` enum value to the OID of the corresponding
  ``provenance_gate`` enum member in PostgreSQL.
- **Status flag**: ``ok`` is ``true`` if the OIDs were loaded
  successfully (``false`` if the extension is not installed in the
  current database).

The cache is populated by :cfunc:`get_constants`, which looks up OIDs in
the system catalogs on first call and stores them per-database.
Subsequent calls return the cached values without catalog access.


Gate Types
----------

The provenance circuit is a directed acyclic graph (DAG) whose nodes are
*gates*.  Each gate has a type from the :cfunc:`gate_type` enum
defined in :cfile:`provsql_utils.h`:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Gate type
     - Meaning
   * - ``gate_input``
     - Leaf gate representing a base-table tuple.
   * - ``gate_plus``
     - Semiring addition (⊕): duplicate elimination, UNION.
   * - ``gate_times``
     - Semiring multiplication (⊗): joins, cross products.
   * - ``gate_monus``
     - M-semiring monus (⊖): EXCEPT.
   * - ``gate_project``
     - Projection gate (where-provenance).
   * - ``gate_eq``
     - Equijoin gate (where-provenance).
   * - ``gate_zero``
     - Semiring additive identity (0).
   * - ``gate_one``
     - Semiring multiplicative identity (1).
   * - ``gate_agg``
     - Aggregation operator.
   * - ``gate_semimod``
     - Semimodule scalar multiplication (for aggregation).
   * - ``gate_delta``
     - Delta operator (δ-semiring).
   * - ``gate_value``
     - Scalar constant value. The ``extra`` blob encodes the literal
       in text form; the mode used in HAVING sub-circuits is parsed
       as a string by ``extract_constant_string``
       (:cfile:`having_semantics.cpp`) and the *float8* mode,
       used to lift numeric constants into the continuous
       random-variable surface, is parsed as a double by the RV
       evaluators.
   * - ``gate_mulinput``
     - Multivalued input (for Boolean probability).
   * - ``gate_cmp``
     - Comparison gate (``<``, ``=``, etc.) used in HAVING
       sub-circuits and by the planner lift of WHERE comparisons on
       ``random_variable`` columns.
   * - ``gate_update``
     - Update-provenance gate.
   * - ``gate_rv``
     - Continuous random-variable leaf. The ``extra`` blob encodes
       the distribution family and parameters
       (e.g. ``normal:μ,σ``, ``uniform:a,b``, ``exponential:λ``);
       the family token resolves through the distribution registry
       (one self-registering file per family under
       ``src/distributions/``, see :doc:`continuous-distributions`).
   * - ``gate_arith``
     - ``N``-ary arithmetic over scalar children. The operator tag
       (``provsql_arith_op``: PLUS / TIMES / MINUS / DIV / NEG /
       MAX / MIN / POW / LN / EXP / PERCENTILE) is stored in
       ``info1``. MAX / MIN are the order
       statistics behind ``greatest`` / ``least`` and the
       RV ``min`` / ``max`` aggregates; PERCENTILE is the
       ``percentile_cont`` order-statistic aggregate.
   * - ``gate_mixture``
     - Probabilistic mixture of scalar random-variable roots gated by
       a Bernoulli weight. The wire vector is ``[p, x, y]`` for a
       Bernoulli mixture or ``[key, mul_1, …, mul_n]`` for a
       categorical block.
   * - ``gate_assumed``
     - Single-child marker wrapping a per-row root computed under a
       provenance-class assumption; the assumption kind is a label in
       ``extra`` (``'boolean'`` from the safe-query rewriter,
       ``'absorptive'`` from cyclic-recursion truncation and the
       reachability route). Identity for compatible evaluators, fatal
       error for the rest; see :doc:`semiring-evaluation`.
   * - ``gate_annotation``
     - Transparent single-child marker carrying an ``extra`` payload
       (the inversion-free certificate on a per-row root, or a per-input
       order key on an input). Unlike every other gate its UUID folds in
       ``extra`` (so two annotations over the same child with different
       ``extra`` are distinct gates); inert (identity) for every
       evaluator. See :ref:`inversion-free-path`.
   * - ``gate_conditioned``
     - Conditioning marker with children ``[target, evidence]``:
       measure-only, ``probability_evaluate`` returns
       P(target ∧ evidence) / P(evidence) and the RV / ``agg_token``
       evaluators the restricted distribution; terminal for the uuid
       carrier and refused by every general ``sr_*`` semiring
       (normalisation is not a semiring operation).
   * - ``gate_mobius``
     - Signed Möbius combination: measure-only, one integer coefficient
       per child stored in ``extra`` (the ``gate_arith`` precedent);
       ``probability_evaluate`` returns Σᵢ coeffᵢ · P(childᵢ). The
       query's literal lineage is a designated transparent child
       (marked ``L:<uuid>`` in ``extra``), so semiring evaluation,
       Shapley and named probability methods pass through to it.
   * - ``gate_case``
     - ``N``-ary guarded selection over scalar (RV) children: wires
       ``[guard_1, value_1, …, guard_k, value_k, default]`` with
       first-match semantics (the value of the first guard event that
       holds, else the default). Backs a ``CASE`` over random variables
       (and ``abs`` / ``clamp`` / ReLU as sugar). Carries data only in
       its wires (the ``gate_conditioned`` precedent). RV/measure-carrier:
       real arms in the Monte-Carlo sampler, ``RangeCheck``, and the
       ``Expectation`` footprint, refused by every general ``sr_*``
       semiring.
   * - ``gate_observe``
     - Latent-variable observation (likelihood-weighting evidence): one
       wire → an observed bare ``gate_rv`` leaf, the observed datum in
       ``extra``. Contributes a continuous density factor (the leaf's
       pdf at the datum) instead of a Boolean truth value, composing
       into an evidence circuit by ``gate_times`` exactly like a
       conditioning event. Evaluated only by the importance-sampling
       weight walk (``Sampler::evalWeight``); refused by every Boolean /
       semiring evaluator (a density factor is not a semiring
       operation). See :doc:`continuous-distributions`.

The random-variable gate types (``gate_rv``, ``gate_arith``,
``gate_mixture``, ``gate_case``), the marker gates (``gate_assumed``,
``gate_annotation``), the measure-only gates (``gate_conditioned``,
``gate_mobius``) and the evidence gate (``gate_observe``) are appended
to the enum before ``gate_invalid``, with no renumbering of older
values. See
:doc:`continuous-distributions` for the full architecture of the
continuous-distribution surface.  ``gate_plus`` / ``gate_times`` gates
may additionally carry a persisted d-DNNF certificate in ``info1``
(deterministic / decomposable by construction), stamped by the
reachability and joint-width compilers and the certified HAVING
enumerations, and consumed by the ``independent`` and
``interpret-as-dd`` evaluators (see :doc:`probability-evaluation`).

Edges (wires) connect parent gates to their children, forming the
provenance formula for each query result tuple.
