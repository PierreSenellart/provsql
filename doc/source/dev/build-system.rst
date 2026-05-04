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
persistent circuit lives in four memory-mapped files per database
(``provsql_gates.mmap``, ``provsql_wires.mmap``,
``provsql_mapping.mmap``, ``provsql_extra.mmap``), each under
``$PGDATA/base/<db_oid>/``.  Every file starts with a 16-byte header
(magic, version, element size) that is validated on open -- see
the :doc:`memory` chapter for details.

An upgrade also requires that the binary layout of
:cfunc:`GateInformation` (in :cfile:`MMappedCircuit.h`), the
:cfunc:`gate_type` enum (in :cfile:`provsql_utils.h`), and the
:cfunc:`MMappedUUIDHashTable` slot structure are all byte-compatible
between the two versions.  Since 1.0.0 these layouts have been
deliberately **frozen**: zero commits to any of them.  The block
comments at the top of those files explicitly call this out.

If a future contribution has to touch the on-disk layout, the
upgrade story has to change:

1. **Bump the on-disk format version.**  Increment the ``version``
   field in the 16-byte header, write a migration pass in the
   background worker or as a standalone tool, and mention the
   one-time migration cost in the upgrade script.
2. **Break upgrades for that release.**  Document it in the release
   notes and ship a hard failure at worker startup if the existing
   ``*.mmap`` files carry an unrecognised version -- safer than
   silently misreading the bytes.  Users then go through
   ``DROP EXTENSION provsql CASCADE; CREATE EXTENSION provsql``,
   losing only the circuit data (their base tables are unaffected).

**Migration from pre-1.3.0 (flat file layout).**  Before 1.3.0, all
databases shared a single set of files directly in ``$PGDATA/``
(without the ``base/<db_oid>/`` prefix and without a format header).
The standalone tool ``provsql_migrate_mmap`` (built with
``make provsql_migrate_mmap``) migrates those old flat files to the
new per-database layout.  It must be run as the ``postgres`` user
*before* restarting the server with the 1.3.0 binaries:

.. code-block:: bash

   provsql_migrate_mmap -D $PGDATA -c "host=/var/run/postgresql"

The tool connects via libpq to enumerate databases, collects root
UUIDs from provenance-tracked tables, BFS-traverses the old circuit,
writes per-database files, and deletes the old flat files on success.
The upgrade script ``provsql--1.2.3--1.3.0.sql`` raises a
``WARNING`` if old flat files are still present when
``ALTER EXTENSION provsql UPDATE`` is run.

Workflow During Development
^^^^^^^^^^^^^^^^^^^^^^^^^^^

Release-engineering for upgrades follows this rhythm:

- **When a contributor adds or modifies SQL** in
  ``provsql.common.sql`` or ``provsql.14.sql``, they *do not* have to
  write a committed upgrade script themselves.  The maintainer who
  cuts the next release is responsible for writing it.  During the
  dev cycle, the Makefile auto-generates an empty dev-cycle
  upgrade script (see above) so that ``ALTER EXTENSION provsql
  UPDATE`` is structurally reachable to ``default_version``; that
  file is purely a placeholder and does **not** replay the SQL
  deltas introduced during the cycle.
- **When cutting a release**, ``release.sh`` checks for
  ``sql/upgrades/provsql--<prev>--<new>.sql`` and refuses to proceed
  if it is missing unless ``git diff`` shows no SQL source changes
  since the previous tag (in which case it auto-generates a no-op
  script and commits it).
- **When touching any mmap-serialised struct or enum**, the
  contributor must either preserve binary compatibility (e.g., by
  appending a new :cfunc:`gate_type` enumerator at the end, never in
  the middle) or coordinate a format-version bump with the
  maintainer -- see the warning block at the top of
  :cfile:`provsql_utils.h` and :cfile:`MMappedCircuit.h`.

Automated Testing of the Upgrade Path
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The upgrade chain is exercised end-to-end by a pg_regress test,
``test/sql/extension_upgrade.sql``.  Because the test is
destructive (it ``DROP``\ s the extension ``CASCADE`` to replay
the upgrade from a clean 1.0.0 state), it must run strictly
**after** every other test in the suite, including the
``schedule.14``-only tests that follow ``schedule.common`` on
PostgreSQL 14+.  To guarantee that ordering regardless of which
source schedule files are active, the `Makefile.internal`
rule that assembles ``test/schedule`` appends a single
``test: extension_upgrade`` line **after** concatenating
``schedule.common`` and (where applicable) ``schedule.14``:

.. code-block:: makefile

   test/schedule: $(wildcard test/schedule.*)
       cat test/schedule.common > test/schedule
       if [ $(PGVER_MAJOR) -ge 14 ]; then \
           cat test/schedule.14 >> test/schedule; \
       fi
       echo "test: extension_upgrade" >> test/schedule

so the final schedule always ends with ``extension_upgrade``.
The test is therefore **not** listed in either ``schedule.common``
or ``schedule.14`` source files.

The test itself:

1. Drops the current provsql extension (``CASCADE`` destroys all
   provenance-tracked state from preceding tests).
2. Installs the oldest supported version via
   ``CREATE EXTENSION provsql VERSION '1.0.0'``.  PostgreSQL picks
   up ``provsql--1.0.0.sql`` from the extensions directory -- a
   frozen install-script fixture generated at build time from
   ``sql/fixtures/provsql--1.0.0-common.sql`` and
   ``sql/fixtures/provsql--1.0.0-14.sql`` (themselves exact copies
   of the historical 1.0.0 source files, extracted via
   ``git show v1.0.0:sql/...``).
3. Runs ``ALTER EXTENSION provsql UPDATE`` (no ``TO`` clause), so
   PostgreSQL advances the extension all the way to whatever the
   current ``default_version`` is -- walking the full chain of
   committed upgrade scripts under ``sql/upgrades/`` and, on a
   development build, the auto-generated empty dev-cycle upgrade
   script described below.
4. Asserts that the post-upgrade ``extversion`` equals
   ``default_version`` (read from
   ``pg_available_extensions``) via a boolean comparison, so the
   expected output never contains a hard-coded version string and
   the test stays correct as master advances.
5. Runs a tiny smoke query against the upgraded extension to
   confirm that the query rewriter, :sqlfunc:`add_provenance`,
   :sqlfunc:`create_provenance_mapping`, and a compiled semiring
   evaluator still work.

The test runs on **every PostgreSQL version in the CI matrix**
(10 through 18), because it lives inside the standard pg_regress
suite.  It catches regressions in: committed upgrade scripts,
the ``DATA`` wildcard in ``Makefile.internal`` that ships them,
and the binary stability of the mmap format across versions.

The Auto-Generated Dev-Cycle Upgrade Script
"""""""""""""""""""""""""""""""""""""""""""

Between releases, HEAD's ``default_version`` is a dev version
(e.g., ``1.3.0-dev``) for which no committed upgrade script
exists.  Rather than maintaining a hand-written dev-cycle script
on master, ``Makefile.internal`` detects dev versions and
generates an **empty** upgrade script at build time:

.. code-block:: makefile

   ifneq ($(findstring -dev,$(EXTVERSION)),)
   LATEST_RELEASE = $(shell git describe --tags --abbrev=0 --match 'v[0-9]*' \
                             2>/dev/null | sed 's/^v//')
   ifneq ($(LATEST_RELEASE),)
   DEV_UPGRADE = sql/$(EXTENSION)--$(LATEST_RELEASE)--$(EXTVERSION).sql
   endif
   endif

If a committed upgrade script already exists for the same
``LATEST_RELEASE → BARE_VERSION`` pair (i.e.
``sql/upgrades/provsql--<prev>--<bare>.sql`` without the ``-dev``
suffix), the Makefile copies it to the dev script so that content
such as migration warnings is exercisable during the dev cycle.
Otherwise the file is created by a ``touch $@`` recipe.  Either way
the file is included in ``DATA`` so ``make install`` ships it, and it
is matched by the existing ``sql/provsql--*.sql`` gitignore pattern
so it never lands in git.

Actual SQL changes made during the dev cycle are *not* captured in
this auto-generated file; they are captured by the hand-written
upgrade script that ``release.sh`` creates (or auto-generates as a
no-op) at release time.  An upgrade from the previous release
directly to an intermediate dev commit therefore works at the
``ALTER EXTENSION`` mechanism level but does **not** replay the
SQL deltas introduced during the dev cycle -- master users who
need a functionally-complete upgrade should wait for the release
tag and use the committed upgrade script.

On release builds (where ``EXTVERSION`` does not end in ``-dev``)
or on dev tarballs without a reachable git tag, ``DEV_UPGRADE``
expands to the empty string and the Makefile falls back to the
committed upgrade scripts only.

Manual Testing
^^^^^^^^^^^^^^

To exercise the same upgrade path interactively:

.. code-block:: bash

   # Install the current build (which now ships the 1.0.0 fixture
   # and all upgrade scripts alongside the current install script).
   # Run as a user with write access to the PostgreSQL directories
   make && make install
   systemctl restart postgresql

   # Install the extension at the old version and populate state.
   psql <<'SQL'
   CREATE DATABASE upgrade_test;
   \c upgrade_test
   CREATE EXTENSION provsql VERSION '1.0.0';
   SELECT extversion FROM pg_extension WHERE extname='provsql';
   -- ... exercise the extension, populate some provenance state ...
   SQL

   # Apply the upgrade chain.
   psql -d upgrade_test -c "ALTER EXTENSION provsql UPDATE TO '1.2.1';"
   psql -d upgrade_test -c "SELECT extversion FROM pg_extension WHERE extname='provsql';"
   # ... verify the extension still works; mmap state is intact ...


Standalone Tools
----------------

``make tdkc`` builds the standalone Tree-Decomposition Knowledge
Compiler.  It links the |cpp| circuit and tree-decomposition code into
an independent binary (no PostgreSQL dependency) that reads a circuit
from a text file and outputs its probability.

``make provsql_migrate_mmap`` builds the mmap migration tool (requires
``libpq-dev``).  It migrates old flat ``$PGDATA/provsql_*.mmap`` files
(pre-1.3.0 format) to the new per-database layout under
``$PGDATA/base/<db_oid>/``.  See the *On-Disk mmap ABI* section
above for usage details.


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
   ``version:`` and ``date-released:`` in ``CITATION.cff``, the
   top-level ``version`` and the ``provides.provsql.version`` in
   ``META.json`` (the PGXN Meta Spec file), and prepends a new
   entry to both ``website/_data/releases.yml`` and
   ``CHANGELOG.md`` (the repo-root changelog mirrors the website
   release-notes block, with the first
   ``## What's new in <version>`` heading stripped to avoid
   duplicating the release heading).
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
