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

What each case study covers
---------------------------

:doc:`Case study 1 -- The Intelligence Agency <casestudy1>`
    The broadest single tour of provenance *evaluation*. A
    security-classification scenario over seven agents drives a custom
    min-clearance semiring, where-provenance, circuit export
    (PROV-XML, ``graph-easy``), and the full probability-method line-up
    side by side (possible-worlds, Monte-Carlo, tree-decomposition, and
    knowledge compilation through ``d4`` / ``c2d`` / ``dsharp`` /
    ``minic2d``). Start here for a panoramic view.

:doc:`Case study 2 -- The Open Science Database <casestudy2>`
    Evidence synthesis over a (fictional) biomedical literature corpus:
    single-source vs. replicated claims, contradictory findings, and
    strength-of-evidence ranking. This is the home of **Shapley and
    Banzhaf** values -- attributing a result to the studies behind it --
    alongside counting / why-provenance, a custom evidence-grade
    semiring, ``HAVING``, and where-provenance.

:doc:`Case study 3 -- Île-de-France Public Transit <casestudy3>`
    **Boolean provenance at real-world scale.** On the STIF GTFS dataset
    (hundreds of routes, tens of thousands of stops) it answers a
    reachability-and-accessibility question -- which stops are reachable
    from Bagneux, and is the whole journey wheelchair-accessible -- where
    a result token is true iff every record along the path carries the
    accessibility flag.

:doc:`Case study 4 -- Government Ministers Over Time <casestudy4>`
    The **temporal** extension and **data-modification tracking**. Over a
    database of French and Singaporean ministers, every fact carries a
    validity interval; the study demonstrates time-travel, history,
    timeslice, and an ``INSERT`` / ``DELETE`` round-trip that is then
    rolled back with ``undo``.

:doc:`Case study 5 -- The Wildlife Photo Archive <casestudy5>`
    Uncertainty that comes out of a machine-learning detector. Candidate
    species per bounding box are modelled with :sqlfunc:`repair_key` and
    the ``mulinput`` gate (block-correlated alternatives); the study
    contrasts probabilistic ranking against naive confidence
    thresholding, and computes :sqlfunc:`expected` species counts.

:doc:`Case study 6 -- The City Air-Quality Sensor Network <casestudy6>`
    The **continuous-distribution** surface, end to end: ``random_variable``
    columns (Normal / Uniform / Exponential / Erlang / categorical /
    mixture), arithmetic and comparison on them, analytic moments and
    Monte-Carlo fallback, and conditional inference. It is the first
    study driven primarily through :doc:`ProvSQL Studio <studio>`, where
    PDFs, mixture DAGs, and simplifier before/after views make the
    machinery visible.

:doc:`Case study 7 -- Peer-Review Assignment and Knowledge Compilation <casestudy7>`
    How the **shape of a query, together with the keys the schema
    declares**, fixes the shape of the Boolean provenance circuit and
    thereby which probability method is cheap. A peer-reviewing scenario
    asks one coverage question three ways -- safe by shape, safe by a
    key, and genuinely :math:`\#P`-hard -- and exposes the
    knowledge-compilation pipeline (Tseytin CNF, d-DNNF compilation,
    :sqlfunc:`ddnnf_stats`, the ``provsql.boolean_provenance`` rewriter)
    before closing on **recursive** reachability as network reliability.
    Also driven through Studio.

.. _case-study-coverage:

Feature coverage matrix
-----------------------

The tables below cross-reference every user-facing feature documented
under the User Guide against the tutorial and the seven case studies.
They are the single source of truth for *which example exercises what*;
when adding a feature, extend the relevant table, and when extending the
tutorial or a case study, mark the additional cells.

Columns:

- **T** -- :doc:`Tutorial <tutorial>` (*Who Killed Daphine?*)
- **1** -- :doc:`Case study 1 <casestudy1>` (*The Intelligence Agency*)
- **2** -- :doc:`Case study 2 <casestudy2>` (*The Open Science Database*)
- **3** -- :doc:`Case study 3 <casestudy3>` (*Île-de-France Public Transit*)
- **4** -- :doc:`Case study 4 <casestudy4>` (*Government Ministers Over Time*)
- **5** -- :doc:`Case study 5 <casestudy5>` (*The Wildlife Photo Archive*)
- **6** -- :doc:`Case study 6 <casestudy6>` (*The City Air-Quality Sensor Network*)
- **7** -- :doc:`Case study 7 <casestudy7>` (*Peer-Review Assignment and Knowledge Compilation*)

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
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4

   "``add_provenance``", "✓", "✓", "✓", "✓", "✓", "✓", "✓", "✓"
   "``remove_provenance``", "", "", "", "", "", "✓", "", ""
   "``provenance()`` (SELECT-list)", "✓", "✓", "✓", "✓", "✓", "✓", "✓", "✓"
   "``create_provenance_mapping`` (table)", "✓", "✓", "✓", "✓", "", "", "", "✓"
   "``create_provenance_mapping_view``", "", "", "", "", "✓", "", "", ""
   "Hand-built mapping table", "", "", "", "", "", "✓", "", ""
   "``provsql.active`` GUC", "", "", "", "", "", "", "", ""

Supported SQL constructs
~~~~~~~~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4

   "SELECT-FROM-WHERE / inner JOIN", "✓", "✓", "✓", "✓", "✓", "✓", "✓", "✓"
   "Self-join", "✓", "✓", "", "✓", "", "✓", "", ""
   "Subqueries in FROM / nested", "", "✓", "✓", "", "", "✓", "", ""
   "GROUP BY", "", "✓", "✓", "", "✓", "✓", "✓", "✓"
   "SELECT DISTINCT", "✓", "✓", "", "✓", "", "", "", ""
   "EXCEPT (monus)", "✓", "✓", "", "", "", "✓", "", ""
   "UNION / UNION ALL", "", "", "", "", "", "", "✓", "✓"
   "HAVING", "", "", "✓", "", "", "", "✓", "✓"
   "VALUES", "", "✓", "", "", "", "✓", "", ""
   "CTE (WITH)", "", "", "", "", "", "✓", "", "✓"
   "WITH RECURSIVE", "", "", "", "", "", "", "", "✓"
   "LATERAL", "", "", "", "", "", "", "✓", ""
   "Window functions", "", "", "", "", "", "", "", ""
   "FILTER clause on aggregates", "", "", "", "", "", "", "", ""
   "CREATE TABLE AS SELECT", "✓", "", "", "✓", "", "✓", "", ""
   "Provenance-bearing VIEW", "", "", "✓", "", "✓", "", "", ""
   "INSERT … SELECT (provenance propagation)", "", "", "", "", "", "", "", ""

Aggregation
~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4

   "COUNT / SUM / MIN / MAX / AVG", "", "✓", "✓", "✓", "✓", "✓", "", "✓"
   "``sum`` / ``avg`` / ``product`` over ``random_variable``", "", "", "", "", "", "", "✓", ""
   "``string_agg`` / ``array_agg``", "", "", "", "", "", "", "", ""
   "``COUNT(DISTINCT …)``", "", "", "", "", "", "", "", ""
   "Arithmetic / cast on aggregate result", "", "", "✓", "", "", "", "", ""
   "``choose`` aggregate", "", "", "", "", "", "", "", ""

Circuit inspection
~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4

   "``get_gate_type``", "", "✓", "", "", "", "", "✓", ""
   "``get_children``", "", "✓", "", "", "", "", "", ""
   "``identify_token``", "", "✓", "", "", "", "", "", ""
   "``get_nb_gates``", "", "✓", "", "", "", "", "", ""
   "``get_infos``", "", "", "", "", "", "", "✓", ""
   "``get_extra``", "", "", "", "", "", "", "✓", ""
   "``simplified_circuit_subgraph``", "", "", "", "", "", "", "(✓)", "✓"

Knowledge compilation and safe queries
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4

   "``provsql.boolean_provenance`` GUC", "", "", "", "", "", "", "", "✓"
   "Safe-query rewriter (hierarchical / read-once)", "", "", "", "", "", "", "", "✓"
   "Tseytin CNF export (DIMACS)", "", "", "", "", "", "", "", "✓"
   "``tseytin_cnf`` / ``tseytin_cnf_mapping``", "", "", "", "", "", "", "", "✓"
   "``ddnnf_stats``", "", "", "", "", "", "", "", "✓"
   "``HAVING`` Poisson-binomial pre-pass", "", "", "", "", "", "", "", "✓"

Semiring evaluation
~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4

   "``sr_boolean``", "", "", "", "✓", "", "", "", ""
   "``sr_boolexpr``", "", "✓", "", "", "", "✓", "", ""
   "``sr_formula``", "✓", "✓", "✓", "✓", "", "✓", "", "✓"
   "``sr_counting``", "✓", "", "✓", "", "", "", "", ""
   "``sr_why``", "", "", "✓", "", "", "", "", "(✓)"
   "``sr_how``", "", "", "", "", "", "", "", "(✓)"
   "``sr_which``", "", "", "", "", "", "", "", ""
   "``sr_tropical``", "", "", "", "", "", "", "", ""
   "``sr_viterbi``", "", "", "", "", "", "", "", ""
   "``sr_lukasiewicz``", "", "", "", "", "", "", "", ""
   "``sr_minmax``", "", "✓", "", "", "", "", "", ""
   "``sr_maxmin``", "", "", "", "", "", "", "", ""
   "``sr_temporal``", "", "", "", "", "✓", "", "", ""
   "``sr_interval_num``", "", "", "", "", "", "", "", ""
   "``sr_interval_int``", "", "", "", "", "", "", "", ""
   "Custom semiring via ``provenance_evaluate``", "", "✓", "✓", "", "", "", "", ""
   "``aggregation_evaluate``", "", "", "", "", "", "", "", ""

Probabilities
~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4

   "``set_prob``", "✓", "✓", "✓", "", "", "✓", "✓", "✓"
   "``get_prob``", "", "", "", "", "", "", "✓", ""
   "``probability_evaluate`` (default fallback)", "", "✓", "✓", "", "", "✓", "", "✓"
   "``'independent'`` method", "", "", "", "", "", "", "✓", "✓"
   "``'possible-worlds'`` method", "✓", "✓", "", "", "", "", "", ""
   "``'monte-carlo'`` method", "(✓)", "✓", "", "", "", "", "✓", "✓"
   "``'tree-decomposition'`` method", "(✓)", "✓", "", "", "", "✓", "✓", "✓"
   "``'compilation'`` (d4 / c2d / dsharp / minic2d)", "(✓)", "✓", "", "", "", "", "", "✓"
   "``'weightmc'`` method", "", "", "", "", "", "", "", "✓"
   "``expected(COUNT/SUM/MIN/MAX)``", "", "", "", "", "", "✓", "✓", ""
   "``repair_key`` (block-independent, ``mulinput``)", "", "", "", "", "", "✓", "", "✓"
   "``provsql.monte_carlo_seed`` GUC", "", "", "", "", "", "", "✓", ""
   "``provsql.rv_mc_samples`` GUC", "", "", "", "", "", "", "✓", ""
   "``provsql.simplify_on_load`` GUC", "", "", "", "", "", "", "✓", ""

Continuous random variables
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7"
   :widths: 44, 4, 4, 4, 4, 4, 4, 4, 4

   "``random_variable`` type / ``provsql.normal``", "", "", "", "", "", "", "✓", ""
   "``provsql.uniform`` / ``provsql.exponential``", "", "", "", "", "", "", "✓", ""
   "``provsql.erlang`` / ``provsql.categorical``", "", "", "", "", "", "", "✓", ""
   "``provsql.mixture`` (Bernoulli and ad-hoc overloads)", "", "", "", "", "", "", "✓", ""
   "``provsql.as_random`` and implicit numeric→rv casts", "", "", "", "", "", "", "✓", ""
   "Arithmetic on ``random_variable`` (``+ - * /``, unary ``-``)", "", "", "", "", "", "", "✓", ""
   "Comparison ``< <= = <> >= >`` (planner-hook rewrite)", "", "", "", "", "", "", "✓", ""
   "``expected(random_variable)`` (unconditional)", "", "", "", "", "", "", "✓", ""
   "``variance(random_variable)``", "", "", "", "", "", "", "✓", ""
   "``moment`` / ``central_moment`` / ``support`` over rv", "", "", "", "", "", "", "✓", ""
   "Conditional inference via ``provenance()`` argument", "", "", "", "", "", "", "✓", ""
   "``rv_sample`` / ``rv_histogram``", "", "", "", "", "", "", "✓", ""

Shapley and Banzhaf values
~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4

   "``shapley``", "", "", "✓", "", "", "", "", ""
   "``shapley_all_vars``", "", "", "✓", "", "", "", "", ""
   "``banzhaf``", "", "", "✓", "", "", "", "", ""
   "``banzhaf_all_vars``", "", "", "✓", "", "", "", "", ""

Where-provenance
~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4

   "``provsql.where_provenance`` GUC", "", "✓", "✓", "", "", "", "", ""
   "``where_provenance(col)``", "", "✓", "✓", "", "", "", "", ""

Data-modification tracking
~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4

   "``provsql.update_provenance`` GUC", "", "", "", "", "✓", "", "", ""
   "INSERT / UPDATE / DELETE tracked", "", "", "", "", "✓", "", "", ""
   "``update_provenance`` log table", "", "", "", "", "✓", "", "", ""
   "``undo``", "", "", "", "", "✓", "", "", ""

Temporal features
~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4

   "``union_tstzintervals``", "", "", "", "", "✓", "", "", ""
   "``timeslice``", "", "", "", "", "✓", "", "", ""
   "``timetravel``", "", "", "", "", "✓", "", "", ""
   "``history``", "", "", "", "", "✓", "", "", ""
   "``time_validity_view`` extension", "", "", "", "", "✓", "", "", ""
   "``get_valid_time``", "", "", "", "", "", "", "", ""

Export and visualisation
~~~~~~~~~~~~~~~~~~~~~~~~~

.. csv-table::
   :class: coverage-matrix
   :header: "Feature", "T", "1", "2", "3", "4", "5", "6", "7"
   :widths: 40, 4, 4, 4, 4, 4, 4, 4, 4

   "``to_provxml``", "", "✓", "", "", "", "", "", ""
   "``view_circuit`` (graph-easy)", "", "✓", "", "", "", "", "", ""
   "``provsql.verbose_level``", "", "", "", "", "", "", "", "✓"
   "ProvSQL Studio (Circuit mode + Where mode)", "", "", "", "", "", "", "✓", "✓"
