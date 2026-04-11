Debugging
=========

This page covers tools and techniques for diagnosing issues in ProvSQL.


Debug Builds
------------

Build with debug symbols and no optimization:

.. code-block:: bash

   make DEBUG=1

This passes ``-O0 -g`` to the compiler, making GDB-based debugging
practical.


Verbose Logging
---------------

The ``provsql.verbose_level`` GUC controls runtime debug output.
Set it per-session:

.. code-block:: sql

   SET provsql.verbose_level = 50;

The GUC is an integer 0--100.  Only three thresholds actually gate
output:

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - Level
     - Output
   * - 0 (default)
     - Quiet.  Values between 1 and 19 behave the same as 0.
   * - 20+
     - Full query text before and after ProvSQL rewriting (PostgreSQL
       15+ only, via ``pg_get_querydef``), plus verbose output from
       the Boolean circuit and DOT export code.
   * - 40+
     - Adds timing of the rewriting phase.
   * - 50+
     - Adds a dump of the PostgreSQL ``Query`` node tree before and
       after rewriting (via ``elog_node_display``).

The ``elog_node_display`` output (level ≥ 50) shows the full internal
representation of the query tree.  This is invaluable for understanding
exactly what the rewriter changed, but produces very large output.


Inspecting Circuits
-------------------

ProvSQL provides SQL functions for examining the provenance circuit
stored in the mmap backend.  These are useful for verifying that the
rewriter produced the expected circuit structure.

- :sqlfunc:`get_gate_type` -- returns the gate type of a token (e.g.,
  ``times``, ``plus``, ``input``).

- :sqlfunc:`get_children` -- returns the child tokens of a gate.

- :sqlfunc:`get_infos` -- returns the integer annotations of a gate
  (used by aggregation and semimodule gates).

- :sqlfunc:`get_nb_gates` -- returns the total number of gates in the
  circuit store.

- :sqlfunc:`identify_token` -- returns the human-readable label of
  an input gate (from a provenance mapping).

Example session:

.. code-block:: sql

   -- Run a query and capture the provenance token
   SELECT *, provenance() FROM my_table WHERE condition;

   -- Inspect the circuit rooted at a token
   SELECT provsql.get_gate_type('some-uuid');
   SELECT provsql.get_children('some-uuid');


Visualizing Circuits
--------------------

:sqlfunc:`view_circuit` renders a provenance circuit as an ASCII
box-art diagram and is often the fastest way to verify that the
rewriter built the correct circuit structure.  See
:doc:`../user/export` for usage details and requirements.


Disabling Provenance
--------------------

To quickly check whether a bug is in ProvSQL's rewriting or elsewhere:

.. code-block:: sql

   SET provsql.active = false;

This disables the planner hook entirely -- queries run as if the
extension were not loaded (though ``provsql`` columns are still
stripped from results).


GDB Tips
--------

Attach GDB to a PostgreSQL backend process:

.. code-block:: bash

   # Find the backend PID (from a psql session)
   SELECT pg_backend_pid();

   # Attach
   gdb -p <pid>

   # Useful breakpoints
   break provsql_planner
   break process_query
   break make_provenance_expression
   break get_provenance_attributes

When debugging the mmap background worker, attach to the worker
process instead (find it via ``ps aux | grep provsql``).

PostgreSQL ships with ``print(void *)`` and ``pprint(void *)``
functions (``src/backend/nodes/print.c``, declared in
``src/include/nodes/print.h``) that dump any ``Node *`` to the
backend's stderr, with ``pprint`` producing pretty-printed output.
From GDB you can invoke them on the current backend with
``call pprint(q)`` to inspect a ``Query`` tree, or similarly on any
other node pointer.  See the `PostgreSQL Developer FAQ
<https://wiki.postgresql.org/wiki/Developer_FAQ>`_ for more
debugging tips.
