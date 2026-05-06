Getting ProvSQL
===============

There are three ways to run ProvSQL:

* `Installing from Source`_ (recommended) – full-featured, suitable for
  production use and development.
* `Via PGXN`_ – the PostgreSQL Extension Network wraps the source build
  behind a one-line ``pgxn install provsql`` command.
* `Docker Container`_ – no installation required, ideal for quickly trying
  ProvSQL without modifying your system.

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

To clone the development version directly::

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
   * `d4 <https://github.com/crillab/d4>`_
   * `dsharp <https://github.com/QuMuLab/dsharp>`_
   * `minic2d <http://reasoning.cs.ucla.edu/minic2d/>`_ (also requires
     ``hgr2htree``)
   * `weightmc <https://bitbucket.org/kuldeepmeel/weightmc/src/master/>`_

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

1. Compile::

       make

   To select a specific PostgreSQL installation, adjust the ``pg_config``
   path in ``Makefile.internal``.

2. Install (as a user with write access to the PostgreSQL directories)::

       make install

3. Enable the extension hook. Add the following line to
   `postgresql.conf <https://www.postgresql.org/docs/current/config-setting.html>`_
   (typically ``/etc/postgresql/VERSION/main/postgresql.conf`` on Linux)::

       shared_preload_libraries = 'provsql'

   Then restart the PostgreSQL server::

       service postgresql restart

   .. important::

      The `shared_preload_libraries <https://www.postgresql.org/docs/current/runtime-config-client.html#GUC-SHARED-PRELOAD-LIBRARIES>`_
      step is mandatory. ProvSQL installs a planner hook that rewrites
      queries transparently; without it the extension loads but provenance
      tracking is silently disabled.

4. In each database where you want to use ProvSQL, load the extension::

       CREATE EXTENSION provsql CASCADE;

   The ``CASCADE`` keyword automatically installs the required
   ``uuid-ossp`` extension if it is not already present.

Upgrading an Existing Installation
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Starting with **1.2.1**, ProvSQL ships PostgreSQL extension upgrade
scripts for the chain ``1.0.0 → 1.1.0 → 1.2.0 → 1.2.1``. To upgrade
an existing installation:

1. Check out the new source, build, and install (as a user with write
   access to the PostgreSQL directories)::

       make
       make install

2. Restart PostgreSQL so the new shared library is loaded::

       service postgresql restart

3. In each database where ``provsql`` is already installed, issue::

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
   instead. The memory-mapped files in the PostgreSQL data
   directory are still binary-compatible with the current release
   (they have not changed layout since 1.0.0), so any circuit
   tokens still referenced by user tables continue to work after
   the drop-and-create.

Testing Your Installation
^^^^^^^^^^^^^^^^^^^^^^^^^

Run the full regression suite as a PostgreSQL superuser::

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

::

    make uninstall

Then remove the ``provsql`` entry from ``shared_preload_libraries`` in
``postgresql.conf`` and restart the server.

Via PGXN
--------

ProvSQL is distributed on the `PostgreSQL Extension Network
<https://pgxn.org/dist/provsql/>`_.  If you have the
`pgxnclient <https://pgxn.github.io/pgxnclient/>`_ tool installed,
a single command (run as a user with write access to the PostgreSQL
directories) downloads, builds, and installs the extension::

    pgxn install provsql

You still need the build prerequisites (C++17 compiler, Boost
headers, PostgreSQL server development headers, and a working
``pg_config`` on your ``PATH``) and you still need to add ``provsql``
to ``shared_preload_libraries`` in ``postgresql.conf`` and restart
the server afterwards -- ``pgxn install`` wraps the source build but
does not modify your server configuration.

To install a specific version::

    pgxn install provsql=1.2.2


Docker Container
----------------

For a quick trial without any local installation, a demonstration Docker
container is available. It is full-featured except for ``c2d`` and
``minic2d`` support::

    docker run inriavalda/provsql

To use a specific release version::

    docker run inriavalda/provsql:1.0.0

Follow the on-screen instructions to connect to the PostgreSQL server
inside the container with a PostgreSQL client.

License
-------

ProvSQL is open-source software distributed under the **MIT License**.
See the ``LICENSE`` file at the root of the repository or
`view it on GitHub <https://github.com/PierreSenellart/provsql/blob/master/LICENSE>`_.
