Getting ProvSQL
===============

There are three ways to run ProvSQL:

* `Installing from Source`_ (recommended) – full-featured, suitable for
  production use and development.
* `Via PGXN`_ – the PostgreSQL Extension Network wraps the source build
  behind a one-line ``pgxn install provsql`` command.
* `Docker Container`_ – no installation required, ideal for quickly trying
  ProvSQL without modifying your system.

.. tip::

   Just want to try ProvSQL without installing anything? The **ProvSQL
   Playground** runs PostgreSQL, ProvSQL and ProvSQL Studio entirely in
   your browser (WebAssembly) at
   `provsql.org/playground/ <https://provsql.org/playground/>`_, on demo
   databases. It needs a recent browser with WebAssembly JSPI; the
   Playground's landing page lists current browser support. For real use,
   install locally as below.

Installing from Source
-----------------------

This is the recommended approach. It gives full access to all features,
including external knowledge-compilation tools.

Getting the Source
^^^^^^^^^^^^^^^^^^

Releases are available on the `ProvSQL website </releases/>`_
and on the `GitHub releases page
<https://github.com/PierreSenellart/provsql/releases>`_.
The source repository is hosted at
`<https://github.com/PierreSenellart/provsql>`_.

To clone the development version directly:

.. code-block:: bash

    git clone https://github.com/PierreSenellart/provsql.git

Prerequisites
^^^^^^^^^^^^^

1. **PostgreSQL ≥ 10.** ProvSQL has been tested with versions 10–18 under
   Linux, macOS (x86-64 and ARM), and Windows Subsystem for Linux.

2. **Build tools.** ``make``, a C/C++ compiler supporting C++17, and
   PostgreSQL development headers (e.g., ``postgresql-server-dev-XX`` on
   Debian-based systems, or the ``postgresql`` Homebrew formula on macOS).

3. `uuid-ossp <https://www.postgresql.org/docs/current/uuid-ossp.html>`_
   PostgreSQL extension (included with most PostgreSQL packages; compile
   ``contrib/`` when building from source).

4. **Boost libraries** – ``libboost-dev`` and ``libboost-serialization-dev``
   (Debian/Ubuntu), or equivalent.

5. **(Optional) Knowledge-compilation tools** for probability computation:

   * `c2d <http://reasoning.cs.ucla.edu/c2d/download.php>`_
   * `d4 <https://github.com/crillab/d4>`_ and its rewrite
     `d4v2 <https://github.com/jm62300/d4>`_
   * `dsharp <https://github.com/QuMuLab/dsharp>`_
   * `minic2d <http://reasoning.cs.ucla.edu/minic2d/>`_ (also requires
     ``hgr2htree``)
   * `Panini (KCBox) <https://github.com/meelgroup/KCBox>`_
   * `weightmc <https://bitbucket.org/kuldeepmeel/weightmc/src/master/>`_,
     `Ganak <https://github.com/meelgroup/ganak>`_,
     `SharpSAT-TD <https://github.com/Laakeri/sharpsat-td>`_,
     `DPMC <https://github.com/vardigroup/DPMC>`_ (weighted model counters)

   All of these are optional: ProvSQL ships with an in-process
   tree-decomposition compiler that needs no external binary. One of
   the compilers is, however, the **final-fallback compiler** for
   :sqlfunc:`probability_evaluate` (default ``d4``, configurable via
   ``provsql.fallback_compiler``; see :doc:`configuration`): if you
   install only one knowledge compiler, install that one so the
   fallback step in the default-strategy chain has something to
   invoke.

   Each tool must be installed as an executable reachable in the PATH of
   the PostgreSQL server process (e.g., ``/usr/local/bin/``). If the tools
   live outside that PATH (a Conda environment, ``$HOME/local/bin``,
   ``/opt/...``), point ProvSQL at them with the
   ``provsql.tool_search_path`` GUC; see :doc:`configuration`.

6. **(Optional) graph-easy** for circuit visualisation (``libgraph-easy-perl``
   on Debian-based systems, or CPAN). Same PATH considerations as above
   apply.

Installation
^^^^^^^^^^^^

1. Compile:

   .. code-block:: bash

       make

   To select a specific PostgreSQL installation, adjust the ``pg_config``
   path in ``Makefile.internal``.

2. Install (as a user with write access to the PostgreSQL directories):

   .. code-block:: bash

       make install

3. Enable the extension hook. Add the following line to
   `postgresql.conf <https://www.postgresql.org/docs/current/config-setting.html>`_
   (typically ``/etc/postgresql/VERSION/main/postgresql.conf`` on Linux):

   .. code-block:: ini

       shared_preload_libraries = 'provsql'

   Then restart the PostgreSQL server:

   .. code-block:: bash

       service postgresql restart

   .. important::

      The `shared_preload_libraries <https://www.postgresql.org/docs/current/runtime-config-client.html#GUC-SHARED-PRELOAD-LIBRARIES>`_
      step is mandatory. ProvSQL installs a planner hook that rewrites
      queries transparently; without it the extension loads but provenance
      tracking is silently disabled.

4. In each database where you want to use ProvSQL, load the extension:

   .. code-block:: postgresql

       CREATE EXTENSION provsql CASCADE;

   The ``CASCADE`` keyword automatically installs the required
   ``uuid-ossp`` extension if it is not already present.

5. Make sure ``provsql`` is in your ``search_path``. ProvSQL's
   operators and functions live in the ``provsql`` schema and are
   resolved through ``search_path`` (see :ref:`search-path` for what
   goes wrong without it). ``CREATE EXTENSION`` prints a ``NOTICE``
   if it is missing; the easiest fix is the bundled helper:

   .. code-block:: postgresql

       SELECT provsql.setup_search_path();

   which appends ``provsql`` to the database's ``search_path`` (only
   new sessions are affected). See :ref:`search-path` for the manual
   alternative and the details.

Upgrading an Existing Installation
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Starting with **1.2.1**, ProvSQL ships PostgreSQL extension upgrade
scripts covering every released version from ``1.0.0`` onwards (see
``sql/upgrades/`` in the source tree for the exact chain available in
your checkout). To upgrade an existing installation:

1. Check out the new source, build, and install (as a user with write
   access to the PostgreSQL directories):

   .. code-block:: bash

       make
       make install

2. **If you are upgrading across 1.3.0** (i.e. from any pre-1.3.0
   release), migrate the on-disk provenance store before restarting
   PostgreSQL. 1.3.0 changed the memory-mapped layout from a flat
   ``$PGDATA/provsql_*.mmap`` set to per-database files under
   ``$PGDATA/base/<db_oid>/`` with a versioned header. Build and run
   the bundled migration tool as the ``postgres`` system user with
   the server stopped (adjust the connection string for your
   socket directory if different):

   .. code-block:: bash

       make provsql_migrate_mmap
       sudo -u postgres ./provsql_migrate_mmap -D $PGDATA -c "host=/var/run/postgresql"

   The tool removes the old flat files on success. Skip this step
   when upgrading between 1.3.0+ releases.

3. Restart PostgreSQL so the new shared library is loaded:

   .. code-block:: bash

       service postgresql restart

4. In each database where ``provsql`` is already installed, issue:

   .. code-block:: postgresql

       ALTER EXTENSION provsql UPDATE;

   PostgreSQL will find the chain of upgrade scripts between your
   current version and the newly installed one and apply them in
   order, inside a single transaction. The persistent provenance
   circuit (stored in memory-mapped files) is preserved across the
   upgrade.

.. note::

   Upgrades from **pre-1.0.0** development snapshots are not
   supported: no upgrade script is provided and you must
   ``DROP EXTENSION provsql CASCADE; CREATE EXTENSION provsql``
   instead. Any provenance tokens previously stored in user tables
   become orphans and will not be resolvable against the new
   circuit store.

.. note::

   In-place ``ALTER EXTENSION provsql UPDATE`` is not supported on
   **PostgreSQL < 12** across any version boundary whose upgrade
   script appends gate-type enum values (1.5.0 and several later
   releases do): such scripts run ``ALTER TYPE ... ADD VALUE``
   statements, which PostgreSQL 10 and 11 reject inside the single
   transaction PostgreSQL wraps the upgrade chain in. Fresh installs
   (``CREATE EXTENSION provsql``) work on every supported PostgreSQL
   version; the restriction only affects the in-place upgrade path.
   To move an existing database forward under PostgreSQL 10 or 11,
   upgrade to PostgreSQL 12+ first (the provenance store carries over
   unchanged) and then run ``ALTER EXTENSION provsql UPDATE``, or
   drop and recreate the extension (losing stored provenance).

Testing Your Installation
^^^^^^^^^^^^^^^^^^^^^^^^^

Run the full regression suite as a PostgreSQL superuser:

.. code-block:: bash

    make test

If tests fail, the pager (usually ``less``) is launched on the diff between
expected and actual output.

.. note::

   To run the tests as a non-default user, grant yourself superuser rights
   first:

   .. code-block:: postgresql

       ALTER USER your_login WITH SUPERUSER;

   This assumes ``your_login`` is already a PostgreSQL user; on Debian-based
   systems you can create it by running ``createuser your_login`` as the
   ``postgres`` system user.

If your PostgreSQL server does not listen on the default port (5432), add
``--port=xxxx`` to the ``EXTRA_REGRESS_OPTS`` line of ``Makefile.internal``.

Tests that depend on optional external tools (``c2d``, ``d4``, ``dsharp``,
``minic2d``, ``weightmc``, ``graph-easy``) are automatically skipped if the
tool is not found in ``$PATH``.

Uninstalling
^^^^^^^^^^^^

.. code-block:: bash

    make uninstall

Then remove the ``provsql`` entry from ``shared_preload_libraries`` in
``postgresql.conf`` and restart the server.

Via PGXN
--------

ProvSQL is distributed on the `PostgreSQL Extension Network
<https://pgxn.org/dist/provsql/>`_.  If you have the
`pgxnclient <https://pgxn.github.io/pgxnclient/>`_ tool installed,
a single command (run as a user with write access to the PostgreSQL
directories) downloads, builds, and installs the extension:

.. code-block:: bash

    pgxn install provsql

You still need the build prerequisites (C++17 compiler, Boost
headers, PostgreSQL server development headers, and a working
``pg_config`` on your ``PATH``) and you still need to add ``provsql``
to ``shared_preload_libraries`` in ``postgresql.conf`` and restart
the server afterwards -- ``pgxn install`` wraps the source build but
does not modify your server configuration.

To install a specific version:

.. code-block:: bash

    pgxn install provsql=X.Y.Z


.. _docker-container:

Docker Container
----------------

For a quick trial without any local installation, a demonstration Docker
container is available. Alongside the extension and a pre-installed ProvSQL
Studio it bundles a set of the optional external tools: the ``d4`` and
``dsharp`` knowledge compilers, the ``ganak``, ``sharpsat-td`` and
``weightmc`` weighted model counters, and ``graph-easy`` for ASCII circuit
rendering.

.. code-block:: bash

    docker run -p 5433:5432 -p 8001:8000 inriavalda/provsql

The ``-p host:container`` flags publish the container's PostgreSQL (5432) and
ProvSQL Studio (8000) on host ports of your choice, which works uniformly
across native Docker, Docker Desktop, and rootless podman. The example maps
them to ``5433`` and ``8001`` to avoid clashing with a PostgreSQL or Studio
you may already run locally on the default ``5432`` / ``8000``; pick whatever
free host ports you like. To use a specific release version:

.. code-block:: bash

    docker run -p 5433:5432 -p 8001:8000 inriavalda/provsql:X.Y.Z

Connect with a PostgreSQL client (``psql -h localhost -p 5433 tutorial test``)
or open Studio at ``http://localhost:8001`` (matching the host ports you
chose); the container also prints these instructions on startup.

The image is seeded with the same tutorial and case-study databases as the
ProvSQL Playground (``tutorial``, ``cs1``, ``cs2``, ``cs4``–``cs7``). Studio
lands on ``tutorial``; switch between them from its connection chip, or point
``psql`` at any of them (e.g. ``psql -h localhost -p 5433 cs1 test``).

ProvSQL Studio
--------------

ProvSQL Studio is a Python-backed web UI for ProvSQL, distributed as a
separate package on PyPI. It connects to any ProvSQL-enabled database
and provides interactive provenance inspection (Where mode), circuit
visualisation (Circuit mode), and on-the-fly semiring evaluation. See
:doc:`studio` for installation and usage.

License
-------

ProvSQL is open-source software distributed under the **MIT License**.
See the ``LICENSE`` file at the root of the repository or
`view it on GitHub <https://github.com/PierreSenellart/provsql/blob/master/LICENSE>`_.
