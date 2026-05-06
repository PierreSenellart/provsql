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

1. Registers five GUC (Grand Unified Configuration) variables:

   - ``provsql.active`` -- enable/disable provenance tracking (default: on).
   - ``provsql.where_provenance`` -- enable where-provenance (default: off).
   - ``provsql.update_provenance`` -- track provenance through DML
     statements (default: off).
   - ``provsql.verbose_level`` -- verbosity for debug messages (0--100,
     default: 0).
   - ``provsql.tool_search_path`` -- colon-separated directories prepended
     to ``PATH`` when invoking external tools (d4, c2d, minic2d, dsharp,
     weightmc, graph-easy); see :cfile:`external_tool.cpp`.

2. Installs the **planner hook** (:cfunc:`provsql_planner`) by saving
   the previous hook in ``prev_planner`` and replacing ``planner_hook``.

3. Installs **shared-memory hooks** for inter-process coordination
   (see :doc:`memory`).

4. Launches the **mmap background worker** that manages persistent
   circuit storage.

When the server shuts down, :cfunc:`_PG_fini` restores the previous
planner and shared-memory hooks.


Component Map
-------------

ProvSQL is a mixed C/|cpp| codebase.  The PostgreSQL interface layer is
written in C (required by the extension API); complex data structures
and algorithms are in |cpp|.

**C files** (PostgreSQL interface layer):

*Planner hook and query rewriting*

- :cfile:`provsql.c` -- planner hook and the bulk of the query
  rewriting logic (~3400 lines).

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

- :cfile:`provenance.c` -- error stub for the ``provenance()`` SQL
  function (reached only when a query bypasses the planner hook).
- :cfile:`provenance_evaluate.c` -- SQL-level semiring evaluation
  (user-defined ``plus``/``times``/... functions).
- :cfile:`aggregation_evaluate.c` -- aggregate evaluation entry point.
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

*Semiring evaluation*

- ``semiring/*.h`` -- header-only semiring implementations (Boolean,
  Counting, Formula, Why, BoolExpr).
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

*Probability, Shapley, knowledge compilation*

- :cfile:`probability_evaluate.cpp` -- probability-method dispatcher
  (see :doc:`probability-evaluation`).
- :cfile:`dDNNF.h` / :cfile:`dDNNF.cpp` -- d-DNNF data structure and
  linear-time probability evaluation.
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

- :cfile:`view_circuit.cpp` -- SQL ``view_circuit`` function
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
     - Scalar constant value.
   * - ``gate_mulinput``
     - Multivalued input (for Boolean probability).
   * - ``gate_cmp``
     - Comparison gate used in HAVING sub-circuits (``<``, ``=``, etc.).
   * - ``gate_update``
     - Update-provenance gate.

Edges (wires) connect parent gates to their children, forming the
provenance formula for each query result tuple.
