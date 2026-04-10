Build System
============

This page describes how ProvSQL is built, how PostgreSQL version
compatibility is handled, and how CI is configured.


Makefile Structure
------------------

ProvSQL uses two Makefiles:

- **``Makefile``** (top-level) -- user-facing targets: ``make``,
  ``make test``, ``make docs``, ``make website``, ``make deploy``.
  This file delegates to ``Makefile.internal`` for the actual build.

- **``Makefile.internal``** -- the real build file, based on
  PostgreSQL's **PGXS** (PostgreSQL Extension Building Infrastructure).
  It includes ``$(PG_CONFIG) --pgxs`` to inherit compiler flags, install
  paths, and the ``pg_regress`` test runner.

Key variables in ``Makefile.internal``:

- ``MODULE_big = provsql`` -- the shared library name.
- ``OBJS`` -- the list of object files to compile (both C and C++).
- ``DATA`` -- SQL files installed to the extension directory.
- ``REGRESS`` -- test names for ``pg_regress``.
- ``PG_CPPFLAGS`` -- extra compiler flags (C++17, Boost headers).
- ``LINKER_FLAGS`` -- ``-lstdc++ -lboost_serialization``.

LLVM JIT is explicitly disabled (``with_llvm = no``) due to known
PostgreSQL bugs with C++ extensions.


PostgreSQL Version Compatibility
--------------------------------

ProvSQL supports PostgreSQL 10 through 18.  Version-specific code uses
preprocessor guards:

.. code-block:: c

   #if PG_VERSION_NUM >= 140000
     // PostgreSQL 14+ specific code
   #endif

``PGVER_MAJOR`` is extracted from ``pg_config --version`` in
``Makefile.internal`` and used to:

- Conditionally compile version-specific C code.
- Include ``sql/provsql.14.sql`` (PostgreSQL 14+ features like temporal
  functions) into the generated SQL.

:cfile:`compatibility.c` / :cfile:`compatibility.h` provide shim
functions for APIs that changed across PostgreSQL versions (e.g.,
``list_insert_nth``, ``list_delete_cell``).


Generated SQL
-------------

The installed SQL file ``provsql--<version>.sql`` is **generated** by
the Makefile from two sources:

- ``sql/provsql.common.sql`` -- functions for all PostgreSQL versions.
- ``sql/provsql.14.sql`` -- functions requiring PostgreSQL 14+.

The Makefile concatenates them based on ``PGVER_MAJOR``:

.. code-block:: makefile

   sql/provsql.sql: sql/provsql.*.sql
       cat sql/provsql.common.sql > sql/provsql.sql
       if [ $(PGVER_MAJOR) -ge 14 ]; then \
           cat sql/provsql.14.sql >> sql/provsql.sql; \
       fi

**Do not edit** the generated file directly.  Edit the source SQL files
instead.


The ``tdkc`` Tool
-----------------

``make tdkc`` builds the standalone Tree-Decomposition Knowledge
Compiler.  It links the C++ circuit and tree-decomposition code into
an independent binary (no PostgreSQL dependency) that reads a circuit
from a text file and outputs its probability.


Documentation Build
-------------------

``make docs`` builds the full documentation:

1. Generates ``sql/provsql.sql`` from the SQL source files.
2. Runs Doxygen twice (``Doxyfile-c`` for C/C++, ``Doxyfile-sql`` for SQL).
3. Post-processes the SQL Doxygen HTML (C-to-SQL terminology).
4. Runs Sphinx to build the user guide and developer guide.
5. Runs the coherence checker (``check-doc-links.py``) to validate
   all ``:sqlfunc:`` and ``:cfunc:`` cross-references.


CI Workflows
------------

Five GitHub Actions workflows run on every push:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Workflow
     - What it does
   * - ``build_and_test.yml``
     - Builds and tests on Linux with PostgreSQL 10--18 (Docker-based).
       Also builds and pushes the Docker image on tagged releases.
   * - ``macos.yml``
     - Builds and tests on macOS with Homebrew PostgreSQL.
   * - ``wsl.yml``
     - Builds and tests on Windows Subsystem for Linux.
   * - ``docs.yml``
     - Builds the full documentation (Doxygen + Sphinx + coherence check).
       Uses the ``pgxn/pgxn-tools`` container with
       ``postgresql-server-dev-all`` for ``pg_config``.
   * - ``codeql.yml``
     - Static analysis via GitHub CodeQL.

All five must pass before merging to ``master``.
