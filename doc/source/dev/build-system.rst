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


Extension Upgrades
------------------

ProvSQL supports in-place upgrades between released versions via
PostgreSQL's standard mechanism:

.. code-block:: sql

   ALTER EXTENSION provsql UPDATE;

When this statement is issued, PostgreSQL looks for a chain of
*upgrade scripts* named ``provsql--<from>--<to>.sql`` in the
extensions directory and applies them in order.  ProvSQL's upgrade
scripts live under ``sql/upgrades/`` in the source tree and are
installed alongside the main ``provsql--<version>.sql`` file by the
``DATA`` variable in ``Makefile.internal``:

.. code-block:: makefile

   UPGRADE_SCRIPTS = $(wildcard sql/upgrades/$(EXTENSION)--*--*.sql)
   DATA = sql/$(EXTENSION)--$(EXTVERSION).sql $(UPGRADE_SCRIPTS)

Upgrade support starts with **1.2.1**: there is a chain
``1.0.0 → 1.1.0 → 1.2.0 → 1.2.1`` of committed upgrade scripts, so
users on any of those versions can run a single
``ALTER EXTENSION provsql UPDATE`` to reach 1.2.1.

Writing an Upgrade Script
^^^^^^^^^^^^^^^^^^^^^^^^^

Each upgrade script is a hand-written delta file that re-runs the
SQL changes made during one release cycle.  Since every release so
far has consisted of in-place ``CREATE OR REPLACE FUNCTION`` rewrites
or purely additive ``CREATE FUNCTION`` / ``CREATE CAST`` statements,
writing an upgrade script is usually a matter of copy-pasting the
modified function bodies from ``sql/provsql.common.sql`` (or
``sql/provsql.14.sql``) into a new file under ``sql/upgrades/``:

.. code-block:: sql

   /**
    * @file
    * @brief ProvSQL upgrade script: A.B.C → X.Y.Z
    *
    * <one-paragraph summary of SQL-surface changes>
    */

   SET search_path TO provsql;

   CREATE OR REPLACE FUNCTION ... ;
   -- or CREATE FUNCTION, CREATE CAST, etc.

The script runs inside an implicit transaction (the whole
``ALTER EXTENSION UPDATE`` is one transaction), so any error rolls
everything back.  The script is executed **once** when transitioning
from A.B.C to X.Y.Z; it does not have to be idempotent on its own.

Non-idempotent statements such as ``CREATE SCHEMA``, ``CREATE TYPE``
(without ``IF NOT EXISTS``), ``CREATE TABLE``, ``CREATE OPERATOR``,
``CREATE CAST``, and ``CREATE AGGREGATE`` are allowed **only** if the
upgrade is the first to introduce the corresponding object --
PostgreSQL will not re-run the script against an already-upgraded
installation.

If a release genuinely introduces no SQL-surface change, the upgrade
script still has to exist so that PostgreSQL can offer the update
path, but it may be a no-op (just the header comment and a
``SET search_path``).  The ``release.sh`` script (see below)
auto-generates such a no-op file when it detects no SQL diff since
the previous tag.

The On-Disk mmap ABI
^^^^^^^^^^^^^^^^^^^^

In-place upgrades only cover the *SQL catalog* state.  ProvSQL's
persistent circuit lives in four memory-mapped files
(``provsql_gates.mmap``, ``provsql_wires.mmap``,
``provsql_mapping.mmap``, ``provsql_extra.mmap``) that are pure
plain-old-data dumps of C and |cpp| structs -- no format version
header, no magic bytes, no schema.

An upgrade therefore requires that the binary layout of
:cfunc:`GateInformation` (in :cfile:`MMappedCircuit.h`), the
:cfunc:`gate_type` enum (in :cfile:`provsql_utils.h`), and the
:cfunc:`MMappedUUIDHashTable` slot structure are all byte-compatible
between the two versions.  Since 1.0.0 these layouts have been
deliberately **frozen**: zero commits to any of them.  The block
comments at the top of those files explicitly call this out.

If a future contribution has to touch the on-disk layout, the
upgrade story has to change.  Two reasonable options:

1. **Bump the on-disk format version.**  Add a small format-version
   header to each of the four ``*.mmap`` files, write a migration
   pass that rewrites the old format to the new one at worker
   startup, and gate the new pass on detecting an old header.  The
   upgrade script for that release also needs to mention the one-time
   migration cost.
2. **Break upgrades for that release.**  Document it in the release
   notes and ship a hard failure at worker startup if the existing
   ``*.mmap`` files look like an old format -- safer than silently
   misreading the bytes.  Users then go through
   ``DROP EXTENSION provsql CASCADE; CREATE EXTENSION provsql``,
   losing only the circuit data (their base tables are unaffected).

Workflow During Development
^^^^^^^^^^^^^^^^^^^^^^^^^^^

Release-engineering for upgrades follows this rhythm:

- **When a contributor adds or modifies SQL** in
  ``provsql.common.sql`` or ``provsql.14.sql``, they *do not* have to
  write the upgrade script themselves.  The maintainer who cuts the
  next release is responsible for writing it.
- **When cutting a release**, ``release.sh`` checks for
  ``sql/upgrades/provsql--<prev>--<new>.sql`` and refuses to proceed
  if it is missing unless ``git diff`` shows no SQL source changes
  since the previous tag (in which case it auto-generates a no-op
  script).
- **When touching any mmap-serialised struct or enum**, the
  contributor must either preserve binary compatibility (e.g., by
  appending a new :cfunc:`gate_type` enumerator at the end, never in
  the middle) or coordinate a format-version bump with the
  maintainer -- see the warning block at the top of
  :cfile:`provsql_utils.h` and :cfile:`MMappedCircuit.h`.

Manual Testing of the Upgrade Path
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

There is no automated CI test for ``ALTER EXTENSION UPDATE`` yet:
exercising it requires a frozen install script for an *old* version
to be present in the extensions directory alongside the current
install script, which in turn requires shipping the old install
script as a test fixture.  That is tractable but not yet done.

To verify an upgrade chain manually:

.. code-block:: bash

   # 1. Check out the old version and install it.
   git checkout v1.2.0
   make && sudo make install
   sudo systemctl restart postgresql
   psql -c "CREATE EXTENSION provsql CASCADE;"
   # ... exercise the extension, populate some provenance state ...

   # 2. Check out the new version and install it without re-creating.
   git checkout v1.2.1
   make && sudo make install
   sudo systemctl restart postgresql
   psql -c "ALTER EXTENSION provsql UPDATE;"
   psql -c "SELECT extversion FROM pg_extension WHERE extname='provsql';"
   # ... verify the extension still works; mmap state should be intact ...


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
3. Checks that
   ``sql/upgrades/provsql--<prev>--<new>.sql`` exists (auto-generating
   a no-op script if the SQL sources have not changed since the
   previous tag, aborting otherwise).
4. Updates ``default_version`` in ``provsql.common.control``,
   ``version:`` and ``date-released:`` in ``CITATION.cff``, and
   prepends a new entry to ``website/_data/releases.yml``.
5. Commits the bumped files, creates a **GPG-signed** annotated tag
   ``vX.Y.Z``, and offers to push the commit and tag.
6. Offers to create a GitHub Release via ``gh release create`` using
   the collected notes.
7. Offers a post-release bump of ``default_version`` on ``master`` to
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
