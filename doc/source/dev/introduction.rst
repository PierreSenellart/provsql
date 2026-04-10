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

:doc:`architecture`
   High-level overview: how the extension is loaded, how its C and C++
   components fit together, and how data flows from an SQL query to a
   provenance evaluation result.  Covers the ``constants_t`` OID cache
   and the ``gate_type`` enum.

:doc:`query-rewriting`
   Detailed walkthrough of :cfile:`provsql.c` -- the planner hook,
   the ``process_query`` function, and every phase of query rewriting
   (CTE inlining, set operations, aggregation, expression building,
   splicing, HAVING, where-provenance, INSERT...SELECT).

:doc:`memory`
   How provenance circuits are persisted across transactions via
   memory-mapped files, the background worker architecture, IPC via
   anonymous pipes, and the shared-memory coordination layer.

:doc:`adding-semiring`
   The ``Semiring<V>`` interface, walkthroughs of the Boolean and
   Counting semirings, and a step-by-step guide for adding a new
   semiring to ProvSQL.

:doc:`testing`
   How ProvSQL's ``pg_regress``-based test suite works, how to write
   new tests, and how to read test failures.

:doc:`debugging`
   Debug builds, the ``provsql.verbose_level`` GUC, circuit inspection
   SQL functions, DOT visualization, and GDB tips.

:doc:`build-system`
   The two-Makefile structure, PGXS integration, PostgreSQL version
   guards, generated SQL files, and CI workflows.
