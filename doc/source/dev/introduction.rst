Developer Guide
===============

This section is for anyone who wants to understand or contribute to
ProvSQL's internals.  It complements the Doxygen API reference with
explanations of design decisions, data flow, and extension points that
are not apparent from the code alone.

The primary reference for the system design is the ICDE 2026 paper
:cite:`sen2026provsql`.

Chapters
--------

:doc:`postgresql-primer`
   The PostgreSQL extension concepts the rest of this guide assumes
   you know: ``shared_preload_libraries``, planner hooks, the
   ``Query`` node tree, background workers, ``palloc``, ``ereport``,
   SQL-callable C functions, OIDs, and GUCs.  Read this first if
   you have not written a PostgreSQL extension before.

:doc:`architecture`
   High-level overview: how the extension is loaded, how its C and |cpp|
   components fit together, and how data flows from an SQL query to a
   provenance evaluation result.  Covers the :cfunc:`constants_t` OID
   cache and the :cfunc:`gate_type` enum.

:doc:`query-rewriting`
   Detailed walkthrough of :cfile:`provsql.c` -- the planner hook,
   the :cfunc:`process_query` function, and every phase of query rewriting
   (CTE inlining, set operations, aggregation, expression building,
   splicing, ``HAVING``, where-provenance, ``INSERT ... SELECT``).

:doc:`memory`
   How provenance circuits are persisted across transactions via
   memory-mapped files, the background worker architecture, IPC via
   anonymous pipes, and the shared-memory coordination layer.

:doc:`where-provenance`
   The where-provenance subsystem: how :cfunc:`WhereCircuit` differs
   from the semiring world, how ``project`` and ``eq`` gates are
   built and interpreted, and how the column map produced by
   :cfunc:`build_column_map` ties query rewriting to per-cell
   locator output.

:doc:`data-modification`
   Tracking the provenance of ``INSERT`` / ``UPDATE`` / ``DELETE``
   statements: the ``provsql.update_provenance`` GUC, the trigger
   machinery, the ``gate_update`` gate type, the
   ``update_provenance`` housekeeping table, and the temporal
   features (:sqlfunc:`undo`, :sqlfunc:`timetravel`,
   :sqlfunc:`timeslice`, :sqlfunc:`history`) built on top.

:doc:`aggregation`
   The semimodule data model for aggregate provenance, the role of
   the :cfunc:`agg_token` type, the ``agg`` / ``semimod`` / ``value``
   / ``delta`` gates, the aggregation-specific phases of
   :cfunc:`process_query`, and a step-by-step guide for adding a
   new compiled aggregate.

:doc:`semiring-evaluation`
   The :cfunc:`Semiring` interface, walkthroughs of the Boolean and
   Counting semirings, a step-by-step guide for adding a new
   compiled semiring, and notes on symbolic-representation
   semirings such as :sqlfunc:`sr_formula`.

:doc:`probability-evaluation`
   The probability-evaluation dispatcher in
   :cfile:`probability_evaluate.cpp`, the d-DNNF data structure,
   the Tseytin encoding, knowledge compilation through external
   compilers, weighted model counting, the tree-decomposition
   path, and a step-by-step guide for plugging in a new method.

:doc:`coding-conventions`
   Naming, error reporting, memory management, the C/|cpp|
   boundary, and the small set of project-specific conventions
   that reviewers will ask new contributors to follow.

:doc:`testing`
   How ProvSQL's ``pg_regress``-based test suite works, how to write
   new tests, the external-tool skip pattern, and how to read test
   failures.

:doc:`debugging`
   Debug builds, the ``provsql.verbose_level`` GUC, circuit inspection
   SQL functions, circuit visualization, and GDB tips.

:doc:`build-system`
   The two-Makefile structure, PGXS integration, PostgreSQL version
   guards, generated SQL files, ``make website`` / ``make deploy``,
   ``release.sh``, the Studio release pipeline, and CI workflows.

:doc:`studio`
   ProvSQL Studio's source-code architecture: the Python module
   layout, the HTTP API surface, the Where / Circuit frontend
   split, per-batch GUC application, on-disk Config persistence,
   the circuit-fetch + frontier-expansion pipeline, and the unit +
   Playwright test harness.


