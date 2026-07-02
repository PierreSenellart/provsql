.. _case-studies-intro:

Case Studies: Overview
======================

The :doc:`tutorial <tutorial>` is the gentle first contact with ProvSQL:
it walks through the core workflow -- add provenance to a table, run a
query, evaluate the result in a semiring -- on a single small example.
The **case studies** that follow are longer, self-contained worked
examples, each built around a realistic dataset and centred on a
different facet of the system. They go well beyond the tutorial in both
the breadth of SQL they exercise and the depth to which they push a
particular capability.

Each case study is independent: most ship a single self-contained
``setup.sql`` to download and load; case study 3 instead gives
instructions for fetching the large Île-de-France GTFS dataset (not
bundled, due to its size), and case study 4 loads bundled data files
from the source tree under ``doc/casestudy4/data/``. Each states its
scenario and tasks up front and can be read on its own. Read the one whose theme
matches what you want to learn; the :ref:`coverage matrix
<case-study-coverage>` at the end of this page is the quickest way to
find which study demonstrates a given feature.

.. tip::

   **No install required.** Every case study here except case study 3 (whose
   GTFS dataset is too large to bundle) runs in the `ProvSQL Playground
   <https://provsql.org/playground/>`_, the browser build of ProvSQL Studio,
   on a ready-made database -- and all but case studies 3 and 4 also ship as
   :ref:`runnable notebooks <studio-example-notebooks>`; each chapter links
   straight to what it has. The Playground bundles no external tools, so
   steps that explicitly call an external knowledge compiler (``d4``,
   ``c2d``…) or ``graph-easy`` do not run there, but the built-in methods
   and everything else do. See the :ref:`Playground note <playground-note>`.

What each case study covers
---------------------------

:doc:`Case study 1 -- Intelligence Agency <casestudy1>`
    The **broadest tour of provenance evaluation**: a security-classification
    scenario over seven agents drives a **custom min-clearance semiring**,
    **where-provenance**, **circuit export**, and the **full probability-method
    line-up** side by side (possible-worlds, Monte-Carlo, tree-decomposition,
    and knowledge compilation through ``d4`` / ``c2d`` / ``dsharp`` /
    ``minic2d``). Start here for a panoramic view.

:doc:`Case study 2 -- Open Science Database <casestudy2>`
    Evidence synthesis over a fictional biomedical corpus -- single-source
    vs. replicated claims, contradictions, strength-of-evidence ranking.
    The home of **Shapley and Banzhaf** values, attributing a result to the
    studies behind it.

:doc:`Case study 3 -- Île-de-France Public Transit <casestudy3>`
    **Boolean provenance at real-world scale.** On the STIF GTFS dataset
    (hundreds of routes, tens of thousands of stops) a result token is true
    iff every record along the path carries the accessibility flag -- which
    stops are reachable from Bagneux by a fully wheelchair-accessible
    journey?

:doc:`Case study 4 -- Government Ministers Over Time <casestudy4>`
    The **temporal** extension and **data-modification tracking**. Over
    French and Singaporean ministers every fact carries a validity interval;
    the study time-travels, takes history and timeslices, and rolls back an
    ``INSERT`` / ``DELETE`` round-trip with ``undo``.

:doc:`Case study 5 -- Wildlife Photo Archive <casestudy5>`
    Uncertainty from a machine-learning detector: **block-correlated
    alternatives** via :sqlfunc:`repair_key` and the ``mulinput`` gate, with
    candidate species per bounding box. It contrasts **probabilistic ranking**
    against naive confidence thresholding and computes **expected species
    counts** with :sqlfunc:`expected`.

:doc:`Case study 6 -- City Air-Quality Sensor Network <casestudy6>`
    The **continuous-distribution** surface end to end: ``random_variable``
    columns (Normal / Uniform / Exponential / Erlang / Gamma / Log-normal / Weibull / Pareto / categorical /
    mixture), arithmetic and comparison on them, analytic moments with
    Monte-Carlo fallback, and conditional inference. The first study driven
    primarily through :doc:`ProvSQL Studio <studio>`.

:doc:`Case study 7 -- Peer-Review Assignment and Knowledge Compilation <casestudy7>`
    How a query's **shape, the schema's keys, and the structure of the
    data** decide which probability method is cheap and which needs a
    compiler. A peer-reviewing scenario organised by where tractability
    comes from: the query is *safe* (the four Dalvi-Suciu routes, including
    the **Möbius-cancellation** witness :math:`q_9`), the query is
    :math:`\#P`-*hard* (the knowledge-compilation pipeline), or the *data*
    is well-structured (the **joint-width** compiler and recursive network
    reliability).

:doc:`Case study 8 -- ProvSQL as a Probability Calculator <casestudy8>`
    ProvSQL as an **exact, correlation-aware probability calculator driven
    in SQL**: five textbook problems -- base-rate fallacy, correlation
    breaking the independence formula, the method portfolio and its cost
    chooser, a continuous posterior by truncation, the conditional
    expectation of an aggregate -- each a one-line query with the ``|``
    ("given") operator throughout. A compact, notebook-first tour of the
    probability surface.

.. _case-study-coverage:

Feature coverage matrix
-----------------------

The tables below cross-reference every user-facing feature documented
under the User Guide against the tutorial and the eight case studies.

Columns:

- **T** -- :doc:`Tutorial <tutorial>` (*Who Killed Daphine?*)
- **1** -- :doc:`Case study 1 <casestudy1>` (*Intelligence Agency*)
- **2** -- :doc:`Case study 2 <casestudy2>` (*Open Science Database*)
- **3** -- :doc:`Case study 3 <casestudy3>` (*Île-de-France Public Transit*)
- **4** -- :doc:`Case study 4 <casestudy4>` (*Government Ministers Over Time*)
- **5** -- :doc:`Case study 5 <casestudy5>` (*Wildlife Photo Archive*)
- **6** -- :doc:`Case study 6 <casestudy6>` (*City Air-Quality Sensor Network*)
- **7** -- :doc:`Case study 7 <casestudy7>` (*Peer-Review Assignment and Knowledge Compilation*)
- **8** -- :doc:`Case study 8 <casestudy8>` (*ProvSQL as a Probability Calculator*)

Cells: ``✓`` the feature is exercised; ``(✓)`` it is mentioned in
passing but not actually executed; empty means it is not covered.

.. raw:: html

   <style>
   table.coverage-matrix td:not(:first-child),
   table.coverage-matrix th:not(:first-child) { white-space: nowrap; }
   </style>

Setup and basics
~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7", "8"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4, 4

   "``add_provenance``", "✓", "✓", "✓", "✓", "✓", "✓", "✓", "✓", "✓"
   "``remove_provenance``", "", "", "", "", "", "✓", "", "", ""
   "``provenance()`` (SELECT-list)", "✓", "✓", "✓", "✓", "✓", "✓", "✓", "✓", "✓"
   "``create_provenance_mapping`` (table)", "✓", "✓", "✓", "✓", "", "", "", "✓", ""
   "``create_provenance_mapping`` (``maintained``)", "", "", "", "", "✓", "", "", "", ""
   "Hand-built mapping table", "", "", "", "", "", "✓", "", "", ""
   "``setup_search_path``", "(✓)", "", "", "", "", "", "", "", ""
   "``provsql.active`` GUC", "", "", "", "", "", "", "", "", ""
   "``gate_one`` / ``gate_zero`` (semiring constants)", "", "", "", "", "", "", "", "", ""

Supported SQL constructs
~~~~~~~~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7", "8"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4, 4

   "SELECT-FROM-WHERE / inner JOIN", "✓", "✓", "✓", "✓", "✓", "✓", "✓", "✓", ""
   "Self-join", "✓", "✓", "", "✓", "", "✓", "", "✓", ""
   "Subqueries in FROM / nested", "", "✓", "✓", "", "", "✓", "", "", ""
   "GROUP BY", "", "✓", "✓", "✓", "✓", "✓", "✓", "✓", "✓"
   "SELECT DISTINCT", "✓", "✓", "", "✓", "", "✓", "", "✓", ""
   "EXCEPT (monus)", "✓", "✓", "", "", "", "✓", "", "", ""
   "UNION / UNION ALL", "", "", "✓", "", "", "", "✓", "✓", ""
   "HAVING", "", "", "✓", "", "", "", "✓", "✓", ""
   "VALUES", "", "✓", "", "", "", "✓", "", "", ""
   "CTE (WITH)", "", "", "", "", "", "✓", "", "✓", "✓"
   "WITH RECURSIVE", "", "", "", "", "", "", "", "✓", ""
   "LATERAL", "", "", "", "✓", "", "", "✓", "", ""
   "FILTER clause on aggregates", "", "", "✓", "", "", "", "", "", ""
   "CREATE TABLE AS SELECT", "✓", "", "", "✓", "✓", "✓", "", "", "✓"
   "Provenance-bearing VIEW", "", "", "✓", "", "✓", "", "", "", ""
   "INSERT … SELECT (provenance propagation)", "", "", "", "", "", "✓", "", "", ""

Aggregation
~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7", "8"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4, 4

   "COUNT / SUM / MIN / MAX / AVG", "", "", "✓", "✓", "", "✓", "", "✓", "✓"
   "``sum`` / ``avg`` / ``product`` over ``random_variable``", "", "", "", "", "", "", "✓", "", ""
   "``string_agg`` / ``array_agg``", "", "", "✓", "", "", "", "", "", ""
   "``COUNT(DISTINCT …)``", "", "", "✓", "", "", "", "", "", ""
   "Arithmetic / cast on aggregate result", "", "", "✓", "", "", "", "", "", ""
   "Provenance-preserving ``agg_token`` arithmetic (``+ - * /``, agg-vs-agg, in HAVING)", "", "", "", "", "", "", "", "", ""
   "``agg_token_value_text`` / ``provsql.aggtoken_text_as_uuid`` GUC", "", "", "", "", "", "", "", "", ""
   "``choose`` aggregate", "", "", "", "", "", "", "", "", ""
   "``explode_table`` (``agg_token`` column to rows)", "", "", "", "", "", "", "", "", ""

Circuit inspection
~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7", "8"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4, 4

   "``get_gate_type``", "", "✓", "", "", "", "", "✓", "", ""
   "``get_children``", "", "✓", "", "", "", "", "", "", ""
   "``identify_token``", "", "✓", "", "", "", "", "", "", ""
   "``get_nb_gates``", "", "✓", "", "", "", "", "", "", ""
   "``get_infos``", "", "", "", "", "", "", "✓", "", ""
   "``get_extra``", "", "", "", "", "", "", "✓", "", ""
   "``circuit_subgraph`` / ``resolve_input`` (Studio circuit mode)", "", "", "", "", "", "", "✓", "✓", ""
   "``simplified_circuit_subgraph``", "", "", "", "", "", "", "(✓)", "✓", ""

Knowledge compilation and safe queries
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7", "8"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4, 4

   "``provsql.provenance = 'boolean'``", "", "", "", "", "", "", "", "✓", ""
   "``provsql.provenance = 'absorptive'`` (cyclic recursion)", "", "", "", "", "", "", "", "✓", ""
   "Bounded-treewidth network reliability (recursive reachability)", "", "", "", "", "", "", "", "✓", ""
   "``provsql.classify_top_level`` GUC (TID/BID pills)", "", "", "", "", "", "", "", "✓", ""
   "Safe-query rewriter (hierarchical / read-once)", "", "", "", "", "", "", "", "✓", ""
   "Joint-width UCQ compiler (bounded joint treewidth)", "", "", "", "", "", "", "", "✓", ""
   "Möbius-inversion route (safe-by-cancellation UCQ)", "", "", "", "", "", "", "", "✓", ""
   "Tseytin CNF export (DIMACS)", "", "", "", "", "", "", "", "✓", ""
   "``tseytin_cnf`` / ``tseytin_cnf_mapping``", "", "", "", "", "", "", "", "✓", ""
   "``ddnnf_stats``", "", "", "", "", "", "", "", "✓", ""
   "``compile_to_ddnnf`` / ``compile_to_ddnnf_dot``", "", "", "", "", "", "", "", "✓", ""
   "``tree_decomposition_dot``", "", "", "", "", "", "", "", "✓", ""
   "``tool_available`` (compiler-picker filter)", "", "", "", "", "", "", "", "✓", ""
   "``HAVING`` Poisson-binomial pre-pass", "", "", "", "", "", "", "", "✓", ""
   "Inversion-free certificate (``annotate`` / ``inversion_free_key`` / Studio IF badge)", "", "", "", "", "", "", "", "✓", ""
   "External-tool registry (``provsql.tools``, ``register_tool``, ``set_tool_preference``)", "", "", "", "", "", "", "", "(✓)", ""
   "``provsql.fallback_compiler`` GUC", "", "", "", "", "", "", "", "", ""
   "``provsql.tool_search_path`` GUC", "", "", "", "", "", "", "", "", ""
   "``provsql.kcmcp_server`` GUC (managed KCMCP server)", "", "", "", "", "", "", "", "", ""

Semiring evaluation
~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7", "8"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4, 4

   "``sr_boolean``", "", "", "", "✓", "", "", "", "", ""
   "``sr_boolexpr``", "", "✓", "", "", "", "✓", "", "", ""
   "``sr_formula``", "✓", "✓", "✓", "✓", "", "✓", "", "✓", ""
   "``sr_counting``", "✓", "", "✓", "", "", "", "", "", ""
   "``sr_why``", "", "", "✓", "", "", "", "", "(✓)", ""
   "``sr_how``", "", "", "", "", "", "", "", "(✓)", ""
   "``sr_which``", "", "", "", "", "", "", "", "", ""
   "``sr_tropical``", "", "", "", "", "", "", "", "", ""
   "``sr_viterbi``", "", "", "", "", "", "", "", "", ""
   "``sr_lukasiewicz``", "", "", "", "", "", "", "", "", ""
   "``sr_minmax`` / ``sr_maxmin``", "", "✓", "", "", "", "", "", "", ""
   "``sr_temporal`` / ``sr_interval_num`` / ``sr_interval_int``", "", "", "", "", "✓", "", "", "", ""
   "Custom semiring via ``provenance_evaluate``", "", "", "✓", "", "", "", "", "", ""

Probabilities
~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7", "8"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4, 4

   "``set_prob``", "✓", "✓", "✓", "", "", "✓", "", "✓", "✓"
   "``get_prob``", "", "", "", "", "", "", "✓", "✓", ""
   "``probability_evaluate`` (default fallback)", "", "✓", "✓", "", "", "✓", "", "✓", "✓"
   "Conditioning operator ``|`` / ``cond`` / ``given``", "", "", "", "", "", "", "", "", "✓"
   "``P(A | B)`` conditional probability", "", "", "", "", "", "", "", "", "✓"
   "``expected(X | C)`` / ``variance(X | C)`` (conditional moments)", "", "", "", "", "", "", "", "", "✓"
   "``'independent'`` method", "", "(✓)", "", "", "", "", "✓", "✓", "✓"
   "``'possible-worlds'`` method", "✓", "✓", "", "", "", "", "", "", "✓"
   "``'monte-carlo'`` method", "(✓)", "✓", "", "", "", "", "✓", "✓", "✓"
   "``'tree-decomposition'`` method", "(✓)", "✓", "", "", "", "✓", "✓", "✓", ""
   "``'compilation'`` (d4 / c2d / dsharp / minic2d)", "(✓)", "✓", "", "", "", "", "", "✓", ""
   "``'inversion-free'`` method", "", "", "", "", "", "", "", "✓", ""
   "``'wmc'`` counters", "", "", "", "", "", "", "", "✓", ""
   "``'d-tree'`` method (certified anytime bounds)", "", "", "", "", "", "", "", "", ""
   "``'sieve'`` method (inclusion-exclusion)", "", "", "", "", "", "", "", "", ""
   "``'karp-luby'`` method (relative FPRAS)", "", "", "", "", "", "", "", "", ""
   "``'stopping-rule'`` method (additive FPRAS)", "", "", "", "", "", "", "", "", ""
   "Guarantee request (``'relative'`` / ``'additive'``, cost-based chooser)", "", "", "", "", "", "", "", "", ""
   "``probability_bounds`` (cheap lower / upper marginals)", "", "", "", "", "", "", "", "", ""
   "Studio benchmark panel", "", "", "", "", "", "", "", "✓", ""
   "``expected(COUNT/SUM/MIN/MAX)``", "", "", "", "", "", "✓", "✓", "", "✓"
   "``repair_key`` (block-independent, ``mulinput``)", "", "", "", "", "", "✓", "", "✓", "✓"
   "``provsql.monte_carlo_seed`` GUC", "", "", "", "", "", "", "✓", "", ""
   "``provsql.rv_mc_samples`` GUC", "", "", "", "", "", "", "✓", "", "✓"
   "``provsql.simplify_on_load`` GUC", "", "", "", "", "", "", "✓", "", ""

Continuous random variables
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7", "8"
   :widths: 44, 4, 4, 4, 4, 4, 4, 4, 4, 4

   "``random_variable`` type / ``provsql.normal``", "", "", "", "", "", "", "✓", "", "✓"
   "``provsql.uniform`` / ``provsql.exponential``", "", "", "", "", "", "", "✓", "", ""
   "``provsql.erlang`` / ``provsql.categorical``", "", "", "", "", "", "", "✓", "", ""
   "``provsql.gamma`` / ``provsql.chi_squared``", "", "", "", "", "", "", "", "", ""
   "``provsql.lognormal`` (exp/ln bridges, product closure)", "", "", "", "", "", "", "", "", ""
   "``provsql.weibull`` / ``provsql.pareto``", "", "", "", "", "", "", "", "", ""
   "``provsql.mixture`` (Bernoulli and ad-hoc overloads)", "", "", "", "", "", "", "✓", "", ""
   "``provsql.as_random`` and implicit numeric→rv casts", "", "", "", "", "", "", "✓", "", ""
   "Arithmetic on ``random_variable`` (``+ - * /``, unary ``-``)", "", "", "", "", "", "", "✓", "", ""
   "Transforms ``^`` / ``pow`` / ``ln`` / ``exp`` / ``sqrt``", "", "", "", "", "", "", "", "", ""
   "Comparison ``< <= = <> >= >`` (planner-hook rewrite)", "", "", "", "", "", "", "✓", "", "✓"
   "``expected(random_variable)`` (unconditional)", "", "", "", "", "", "", "✓", "", "✓"
   "``variance(random_variable)``", "", "", "", "", "", "", "✓", "", "✓"
   "``moment`` / ``central_moment`` / ``support`` over rv", "", "", "", "", "", "", "✓", "", "✓"
   "``quantile`` (inverse CDF / percentiles / VaR)", "", "", "", "", "", "", "", "", ""
   "Conditional inference via ``provenance()`` argument", "", "", "", "", "", "", "✓", "", "(✓)"
   "``rv_sample`` / ``rv_histogram``", "", "", "", "", "", "", "✓", "", ""
   "``rv_analytical_curves`` (PDF/CDF overlay)", "", "", "", "", "", "", "✓", "", ""

Shapley and Banzhaf values
~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7", "8"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4, 4

   "``shapley``", "", "", "✓", "", "", "", "", "", ""
   "``shapley_all_vars``", "", "", "✓", "", "", "", "", "", ""
   "``banzhaf``", "", "", "✓", "", "", "", "", "", ""
   "``banzhaf_all_vars``", "", "", "✓", "", "", "", "", "", ""

Where-provenance
~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7", "8"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4, 4

   "``provsql.provenance = 'where'``", "", "✓", "✓", "", "", "", "", "", ""
   "``where_provenance(col)``", "", "✓", "✓", "", "", "", "", "", ""

Data-modification tracking
~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7", "8"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4, 4

   "``provsql.update_provenance`` GUC", "", "", "", "", "✓", "", "", "", ""
   "INSERT / UPDATE / DELETE tracked", "", "", "", "", "✓", "", "", "", ""
   "``update_provenance`` log table", "", "", "", "", "✓", "", "", "", ""
   "``undo``", "", "", "", "", "✓", "", "", "", ""

Temporal features
~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7", "8"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4, 4

   "``union_tstzintervals``", "", "", "", "", "✓", "", "", "", ""
   "``timeslice``", "", "", "", "", "✓", "", "", "", ""
   "``timetravel``", "", "", "", "", "✓", "", "", "", ""
   "``history``", "", "", "", "", "✓", "", "", "", ""
   "``time_validity_view`` extension", "", "", "", "", "✓", "", "", "", ""
   "``get_valid_time``", "", "", "", "", "", "", "", "", ""

Export and visualisation
~~~~~~~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7", "8"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4, 4

   "``to_provxml``", "", "✓", "", "", "", "", "", "", ""
   "``view_circuit`` (graph-easy)", "", "✓", "", "", "", "", "", "", ""
   "``provsql.verbose_level``", "", "", "", "", "", "", "", "(✓)", ""
   "ProvSQL Studio (Circuit mode + Where mode)", "", "", "", "", "", "", "✓", "✓", ""
   "ProvSQL Studio (Contributions mode)", "", "", "✓", "", "", "", "", "", ""
