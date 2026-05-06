PostgreSQL Extension Primer
===========================

The rest of the developer guide assumes some familiarity with how
PostgreSQL extensions are built and what the relevant internal APIs
look like.  This chapter is a short refresher on the bits ProvSQL
relies on most heavily.  It is *not* a substitute for the
`PostgreSQL Server Programming documentation
<https://www.postgresql.org/docs/current/server-programming.html>`_;
when in doubt, that is the authoritative reference.


Shared Libraries and ``shared_preload_libraries``
-------------------------------------------------

A PostgreSQL extension is a shared library that PostgreSQL loads at
runtime.  There are two loading paths:

- **On-demand loading** -- the library is loaded the first time one
  of its functions is called.  This is the default for most
  extensions.
- **Preloading** -- the library is loaded when the postmaster starts
  up, by listing it in ``shared_preload_libraries`` in
  ``postgresql.conf``.  Preloading is mandatory for any extension
  that installs a global hook or registers a background worker,
  because those happen during postmaster initialisation and cannot
  be retro-fitted.

ProvSQL **must** be preloaded.  Its :cfunc:`_PG_init` function
installs a planner hook and registers the mmap background worker
(see :doc:`memory`); both require running before any backend has
started executing queries.  Forgetting to add ``provsql`` to
``shared_preload_libraries`` is the single most common installation
mistake.


Hooks
-----

A *hook* is a global function pointer that PostgreSQL exposes at a
well-defined point in its query-processing pipeline.  An extension
overwrites the pointer in :cfunc:`_PG_init`, *chains* to the previous
value (if any), and runs its own logic before delegating to the
default implementation.

ProvSQL uses one hook:

- ``planner_hook`` -- called for every query just before planning.
  ProvSQL installs :cfunc:`provsql_planner`, which inspects the
  ``Query`` tree and rewrites it to track provenance.

The pattern is always the same:

.. code-block:: c

   prev_planner = planner_hook;
   planner_hook = my_planner;

   ...

   PlannedStmt *my_planner(Query *q, ...) {
     // do work
     return prev_planner ? prev_planner(q, ...) : standard_planner(q, ...);
   }

Other extensions may install their own ``planner_hook`` after ProvSQL,
which is why chaining via ``prev_planner`` matters.


The ``Query`` Node Tree
-----------------------

PostgreSQL represents a parsed-and-analysed SQL query as a ``Query``
node defined in ``nodes/parsenodes.h``.  The fields ProvSQL touches
most are:

- ``commandType`` -- ``CMD_SELECT``, ``CMD_INSERT``, etc.
- ``rtable`` -- the *range table*, a list of ``RangeTblEntry``
  (RTE) nodes describing every relation referenced in the query
  (base tables, subqueries, joins, function calls, ``VALUES``
  lists...).
- ``targetList`` -- the ``SELECT`` list, a list of ``TargetEntry``
  nodes wrapping ``Expr`` trees.
- ``jointree`` -- the ``FROM`` and ``WHERE`` clauses encoded as a
  tree of ``FromExpr`` and ``JoinExpr`` nodes.
- ``groupClause`` -- the ``GROUP BY`` clause.
- ``havingQual`` -- the ``HAVING`` predicate.
- ``sortClause``, ``distinctClause``, ``setOperations``... --
  the rest of the ``SELECT`` clause.
- ``hasAggs``, ``hasSubLinks``, ``hasDistinctOn`` -- boolean flags
  used by the planner to skip work.

A ``Var`` node references a column by ``(varno, varattno)``: a
1-based index into ``rtable`` and a 1-based attribute number within
that RTE.  Most of the rewriting in :doc:`query-rewriting` is about
inserting, deleting, and renumbering ``Var`` nodes correctly.

Extension code rarely constructs ``Query`` trees by hand; instead it
uses the helper constructors in ``nodes/makefuncs.h`` (``makeVar``,
``makeNode``...).


Background Workers
------------------

A *background worker* is a separate server process, distinct from
any client backend.  Workers are described by a
``BackgroundWorker`` struct that the extension fills in and passes
to ``RegisterBackgroundWorker`` from :cfunc:`_PG_init` (or to
``RegisterDynamicBackgroundWorker`` from a running backend, for
on-demand workers).  The fields that matter most:

- ``bgw_library_name`` -- the name of the extension shared object.
- ``bgw_function_name`` -- the name of the C function to invoke as
  the worker's entry point.
- ``bgw_start_time`` -- when the postmaster should launch the
  worker (``BgWorkerStart_PostmasterStart``,
  ``BgWorkerStart_ConsistentState``, or
  ``BgWorkerStart_RecoveryFinished``).  ProvSQL's mmap worker
  uses ``BgWorkerStart_PostmasterStart``, so it is up before any
  client backend connects.

When the postmaster decides to start the worker, it dynamically
loads ``bgw_library_name`` and looks up ``bgw_function_name`` by
name -- effectively a ``dlsym()`` call.  The entry function must
therefore have **default ELF visibility**; on some build
configurations, ``-fvisibility=hidden`` is in effect and a plain
``void f()`` declaration is not exported.  ProvSQL works around
this by declaring its background worker entry point with
``PGDLLEXPORT``, which expands to
``__attribute__((visibility("default")))`` on GCC/Clang.  See
:cfunc:`provsql_mmap_worker` and :cfunc:`RegisterProvSQLMMapWorker`.

Workers communicate with normal backends through PostgreSQL shared
memory and (in ProvSQL's case) anonymous pipes -- see
:doc:`memory`.


Memory Management: ``palloc`` and Memory Contexts
-------------------------------------------------

PostgreSQL replaces ``malloc`` / ``free`` with a *memory context*
system: ``palloc(size)`` allocates from the current context, and
contexts can be reset or destroyed wholesale.  Most allocations
happen in a per-query or per-transaction context that PostgreSQL
frees automatically when the query (or transaction) ends, so a lot
of extension code never explicitly frees anything.

C++ code that allocates with ``new`` or stores objects in STL
containers does **not** participate in this scheme.  ProvSQL's C++
side uses ordinary heap allocation; the boundary between the two
worlds is the C ``Datum`` interface (see below).


Error Reporting: ``ereport`` / ``elog``
---------------------------------------

PostgreSQL extensions report errors with the ``ereport()`` and
``elog()`` macros.  ``ereport`` is the modern, preferred form for
user-facing diagnostics (it lets you attach an SQL error code via
``errcode()``, a primary message via ``errmsg()``, optional
``errdetail()`` / ``errhint()``, etc.); ``elog`` is the older
single-argument shortcut and is best reserved for internal /
debug messages.  At ``ERROR`` level (or higher) both abort
execution of the current query and do not return to the caller;
at ``WARNING``, ``NOTICE``, or ``LOG`` they emit a message and
return normally.

ProvSQL wraps both forms in convenience macros declared in
:cfile:`provsql_error.h`:

- :cfunc:`provsql_error` -- aborts with ``"ProvSQL: ..."`` prefix.
- :cfunc:`provsql_warning`, :cfunc:`provsql_notice`,
  :cfunc:`provsql_log` -- non-aborting levels.

Because :cfunc:`provsql_error` ultimately raises an ``ERROR``,
control never returns from it.  Inside C++ code that needs to
release resources on the way out, use exceptions and let the
SQL-callable wrapper catch them and re-raise as a PostgreSQL
``ERROR``.  Throwing exceptions across the C/C++ boundary is
undefined behaviour, so the catch must happen in the C++ side.


SQL-Callable C Functions
------------------------

PostgreSQL exposes C functions to SQL through a fixed calling
convention: every function takes a ``FunctionCallInfo`` (which
holds the arguments) and returns a ``Datum``.  A function is
registered for SQL via the ``PG_FUNCTION_INFO_V1`` macro:

.. code-block:: c

   PG_FUNCTION_INFO_V1(my_function);

   Datum my_function(PG_FUNCTION_ARGS) {
     int32 arg = PG_GETARG_INT32(0);
     // ...
     PG_RETURN_INT32(result);
   }

A ``Datum`` is a fixed-width opaque value that can hold any
PostgreSQL type, with conversion macros (``PG_GETARG_*``,
``PG_RETURN_*``, ``DatumGet*``, ``*GetDatum``) on each side.  The
``CREATE FUNCTION`` SQL statement then declares the function with
``LANGUAGE C`` and the appropriate argument and return types.

The ``PG_FUNCTION_INFO_V1`` macro emits a ``PGDLLEXPORT`` info
struct so the function is reachable through ``dlsym()``.  This is
why most ProvSQL SQL-callable C functions do not need an explicit
``PGDLLEXPORT`` -- the macro takes care of it.


OIDs and Catalog Lookups
------------------------

Almost everything in PostgreSQL has an *Object Identifier* (OID):
tables, columns, types, functions, operators, schemas...  When
ProvSQL builds expression trees by hand, it must reference its own
types and functions by OID, not by name.  The OIDs are not stable
across installations, so ProvSQL caches them at first use.  See
:cfunc:`constants_t` and :cfunc:`get_constants` (covered in
:doc:`architecture`).


GUCs (Configuration Parameters)
-------------------------------

PostgreSQL exposes server- and session-scoped settings as *Grand
Unified Configuration* (GUC) variables, registered via
``DefineCustom*Variable`` from :cfunc:`_PG_init`.  ProvSQL exposes
five:

- ``provsql.active`` -- master switch.
- ``provsql.where_provenance`` -- enable where-provenance tracking
  (see :doc:`where-provenance`).
- ``provsql.update_provenance`` -- enable data-modification tracking
  (see :doc:`data-modification`).
- ``provsql.verbose_level`` -- diagnostic verbosity (see
  :doc:`debugging`).
- ``provsql.tool_search_path`` -- colon-separated directories
  prepended to ``PATH`` when ProvSQL spawns external tools (the
  d-DNNF compilers d4, c2d, minic2d, dsharp; the ``weightmc``
  weighted model counter; the ``graph-easy`` DOT renderer). The
  helper :cfunc:`run_external_tool` in :cfile:`external_tool.cpp`
  reads this GUC, ``setenv``\ s ``PATH`` for the duration of the
  ``system()`` call, and restores it afterwards. Two companion
  helpers handle the surrounding error reporting:
  :cfunc:`find_external_tool` pre-flights tool availability by
  delegating to ``/bin/sh -c 'command -v ...'`` (so it sees the same
  PATH resolution that the eventual invocation will), and
  :cfunc:`format_external_tool_status` decodes the @c system() return
  value -- distinguishing exit 127 (not found), exit 126 (not
  executable), termination by signal, and plain nonzero exit -- into
  a single human-readable message used by the throws in
  :cfunc:`BooleanCircuit::compilation`,
  :cfunc:`BooleanCircuit::WeightMC`, and :cfunc:`DotCircuit::render`.

GUCs can be set in ``postgresql.conf``, with ``ALTER SYSTEM``,
per-session with ``SET``, or per-transaction with ``SET LOCAL``.
