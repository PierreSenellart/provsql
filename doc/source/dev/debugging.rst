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

Levels:

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - Level
     - Output
   * - 0
     - Quiet (default).
   * - 1--9
     - Informational messages (warnings about unsupported features,
       provenance loss, etc.).
   * - 10--19
     - Basic debug information.
   * - 20+
     - Full query text before and after ProvSQL rewriting (PostgreSQL
       15+ only, via ``pg_get_querydef``).
   * - 40+
     - Timing of the rewriting phase.
   * - 50+
     - Dump of the PostgreSQL ``Query`` node tree before and after
       rewriting (via ``elog_node_display``).

The ``elog_node_display`` output (level >= 50) shows the full internal
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

:sqlfunc:`view_circuit` exports a provenance circuit as a DOT graph
that can be rendered with Graphviz:

.. code-block:: sql

   SELECT provsql.view_circuit(provenance(), 'my_mapping')
   FROM my_table
   LIMIT 1;

Pipe the output through ``dot -Tpng`` to produce an image.  This is
often the fastest way to verify that the rewriter built the correct
circuit structure.


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

PostgreSQL defines useful GDB macros in ``src/tools/gdbinit`` of the
PostgreSQL source tree.  The most helpful is ``pprint(node)`` which
pretty-prints any PostgreSQL ``Node *``.
