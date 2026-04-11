Build System
============

This page describes how ProvSQL is built, how PostgreSQL version
compatibility is handled, and how CI is configured.


Makefile Structure
------------------

ProvSQL uses two Makefiles:

- ``Makefile`` (top-level) -- user-facing targets: ``make``,
  ``make test``, ``make docs``, ``make website``, ``make deploy``.
  This file delegates to ``Makefile.internal`` for the actual build.

- ``Makefile.internal`` -- the real build file, based on
  PostgreSQL's **PGXS** (PostgreSQL Extension Building Infrastructure).
  It includes ``$(PG_CONFIG) --pgxs`` to inherit compiler flags, install
  paths, and the ``pg_regress`` test runner.

Key variables in ``Makefile.internal``:

- ``MODULE_big = provsql`` -- the shared library name.
- ``OBJS`` -- the list of object files to compile (both C and |cpp|).
- ``DATA`` -- SQL files installed to the extension directory.
- ``REGRESS`` -- test names for ``pg_regress``.
- ``PG_CPPFLAGS`` -- extra compiler flags (|cpp17|, Boost headers).
- ``LINKER_FLAGS`` -- ``-lstdc++ -lboost_serialization``.

LLVM JIT is explicitly disabled (``with_llvm = no``) due to known
PostgreSQL bugs with |cpp| extensions.


PostgreSQL Version Compatibility
--------------------------------

ProvSQL supports PostgreSQL 10 through 18.  Version-specific C code
uses the ``PG_VERSION_NUM`` macro (from PostgreSQL's own ``pg_config.h``):

.. code-block:: c

   #if PG_VERSION_NUM >= 140000
     // PostgreSQL 14+ specific code
   #endif

:cfile:`compatibility.c` / :cfile:`compatibility.h` provide shim
functions for APIs that changed across PostgreSQL versions (e.g.,
:cfunc:`list_insert_nth`, ``list_delete_cell``).


Generated SQL
-------------

The SQL layer is assembled from two hand-edited sources:

- ``sql/provsql.common.sql`` -- functions for all PostgreSQL versions.
- ``sql/provsql.14.sql`` -- functions requiring PostgreSQL 14+.

The Makefile concatenates them into an intermediate
:sqlfile:`provsql.sql` based on the major version reported by
``pg_config``:

.. code-block:: makefile

   sql/provsql.sql: sql/provsql.*.sql
       cat sql/provsql.common.sql > sql/provsql.sql
       if [ $(PGVER_MAJOR) -ge 14 ]; then \
           cat sql/provsql.14.sql >> sql/provsql.sql; \
       fi

:sqlfile:`provsql.sql` is then copied to
``sql/provsql--<version>.sql``.  The two files have identical content,
but the version-suffixed name is the one PostgreSQL's extension
machinery expects: ``CREATE EXTENSION provsql`` looks up
``provsql--<version>.sql`` in the extensions directory.  The
unsuffixed :sqlfile:`provsql.sql` also serves as the Doxygen input
file for the SQL API reference.

**Do not edit** either generated file directly.  Edit the source SQL
files (``provsql.common.sql`` / ``provsql.14.sql``) instead.


The ``tdkc`` Tool
-----------------

``make tdkc`` builds the standalone Tree-Decomposition Knowledge
Compiler.  It links the |cpp| circuit and tree-decomposition code into
an independent binary (no PostgreSQL dependency) that reads a circuit
from a text file and outputs its probability.


Documentation Build
-------------------

``make docs`` builds the full documentation:

1. Generates :sqlfile:`provsql.sql` from the SQL source files.
2. Runs Doxygen twice (``Doxyfile-c`` for C/|cpp|, ``Doxyfile-sql`` for SQL).
3. Post-processes the SQL Doxygen HTML (C-to-SQL terminology).
4. Runs Sphinx to build the user guide and developer guide.
5. Runs the coherence checker (``check-doc-links.py``) to validate
   all ``sqlfunc`` and ``cfunc`` cross-references.


Releases
--------

``release.sh <version>`` (e.g. ``./release.sh 1.2.0``) automates
creating a new release.  It:

1. Checks that ``gh``, ``gpg``, and a configured GPG signing key are
   available, and that the version string is newer than any existing
   ``vX.Y.Z`` tag.
2. Opens ``$EDITOR`` to collect release notes (a pre-filled template;
   leaving it unchanged aborts).
3. Updates ``default_version`` in ``provsql.common.control`` and
   prepends a new entry to ``website/_data/releases.yml``.
4. Commits the bump, creates a **GPG-signed** annotated tag
   ``vX.Y.Z``, and offers to push the commit and tag.
5. Offers to create a GitHub Release via ``gh release create`` using
   the collected notes.
6. Offers a post-release bump of ``default_version`` on ``master`` to
   the next ``X.(Y+1).0-dev`` (or a user-provided ``NEXT_VERSION``).

The signed tag push is what the Linux CI workflow keys on to build
and push the ``inriavalda/provsql`` Docker image for the new version.


Website and Deployment
----------------------

``make website`` builds the public-facing Jekyll site under
``website/``.  It depends on ``make docs``, copies the Sphinx HTML
output and the two Doxygen outputs into the Jekyll source tree
(``website/docs/``, ``website/doxygen-sql/html/``,
``website/doxygen-c/html/``), copies shared branding assets, and
runs ``jekyll build`` to produce ``website/_site/``.

``make deploy`` builds the website and then rsyncs
``website/_site/`` to the live server (``provsql.org``).  The rsync
uses ``--checksum`` so that files Jekyll rewrote without content
change are not retransferred.


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
