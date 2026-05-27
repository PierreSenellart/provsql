External-Tool Registry
======================

For knowledge compilation, weighted model counting, and ASCII circuit
rendering, ProvSQL shells out to external command-line tools: the d-DNNF
compilers ``d4``, ``d4v2``, ``c2d``, ``minic2d``, ``dsharp`` and Panini
(KCBox), the model counters ``ganak``, ``sharpsat-td``, ``dpmc`` and
``weightmc``, and the GraphViz wrapper ``graph-easy``.

The **tool registry** makes the set of these tools, how each is invoked, what
it can do, and which is preferred **data** rather than compiled-in constants.
An administrator can register a new tool, repoint one at a different binary,
reorder preferences, or disable one, all at run time and without recompiling.

Out of the box the registry is seeded with exactly the tools ProvSQL knows
about and their usual invocations, so nothing needs to be configured: a
fresh database behaves identically to before the registry existed.

The catalog: ``provsql.tools``
------------------------------

The read-only view :sqlfunc:`tool_available`'s companion, ``provsql.tools``,
lists every registered tool:

.. code-block:: postgresql

    SELECT name, operations, executable, available, preference, enabled
    FROM provsql.tools ORDER BY preference DESC, name;

Columns:

``name``
    Logical id, e.g. ``d4`` or ``panini-obdd``. This is the value
    :sqlfunc:`probability_evaluate` and :ref:`provsql.fallback_compiler
    <provsql-fallback-compiler>` accept.
``kind``
    ``cli`` (a command-line tool spawned per call) or ``kcmcp`` (a warm,
    socket-attached server reached at ``endpoint``; see `KCMCP servers
    (kind = kcmcp)`_).
``executable``
    The binary resolved on the backend's ``PATH`` (empty for a pipeline tool
    such as ``dpmc``, which runs ``htb | dmc``, and for a ``kcmcp`` tool).
``endpoint``
    For a ``kcmcp`` tool, the server address: ``unix:/path`` or ``host:port``,
    or the literal ``managed`` to use the server launched by
    :ref:`provsql.kcmcp_server <provsql-kcmcp-server>`. Empty for a ``cli``
    tool.
``operations``
    What the tool does, using the names shared with the
    :doc:`KCMCP server protocol </dev/kc-server-protocol>`: ``compile``
    (knowledge compilation), ``wmc`` (weighted model counting), or the
    ProvSQL-local ``render``.
``input_formats`` / ``output_format``
    The problem encodings it reads / the result encoding it produces, again
    KCMCP names (``dimacs-cnf``, ``circuit-bcs12``; ``ddnnf-nnf``, ``decimal``,
    …).
``parser``
    How ProvSQL decodes the tool's raw output (an internal tag).
``argtpl`` / ``argtpl_circuit``
    The command template for a ``cli`` tool: ``argtpl`` for a ``dimacs-cnf``
    input, ``argtpl_circuit`` for the native ``circuit-bcs12`` fast path (set
    only on a tool that accepts that input). Placeholders ``{in}`` / ``{out}``
    (and ``{binary}`` / ``{tmpdir}`` / ``{pivotAC}``) are filled in at call
    time; the executable is prepended when ``{binary}`` is absent. Empty for a
    ``kcmcp`` tool.
``preference``
    Selection order within an operation (higher first).
``enabled``
    Whether the dispatchers may select it.
``available``
    Whether the tool is usable now: for a ``cli`` tool, its executable and
    every dependency resolve on the backend's ``PATH`` (the
    :sqlfunc:`tool_available` test, honouring :ref:`provsql.tool_search_path
    <provsql-tool-search-path>`); for a ``kcmcp`` tool, its ``endpoint`` is
    set (a configured endpoint, not a live connection probe).

Selection
---------

When you name a tool explicitly (``probability_evaluate(t, 'compilation',
'd4')`` or ``probability_evaluate(t, 'wmc', 'ganak')``), ProvSQL uses it, or
raises a clear error if it is unknown, disabled, or not on ``PATH``.

When you do **not** name one (the ``'compilation'`` method with no compiler,
or the ``'wmc'`` method with no counter), ProvSQL picks the
**highest-preference enabled tool whose binary resolves on** ``PATH`` for
that operation. :ref:`provsql.fallback_compiler <provsql-fallback-compiler>`
governs only the final fallback route of the default probability method.

Managing tools
--------------

Four superuser-only functions edit the registry:

:sqlfunc:`register_tool` adds a tool, or replaces the record with the same
name. Its capability triple uses the KCMCP names. For example, registering a
second build of ``d4`` kept under a specific path:

.. code-block:: postgresql

    SELECT provsql.register_tool(
      'd4-custom',
      executable    => '/opt/d4-custom/d4',
      operations    => ARRAY['compile'],
      input_formats => ARRAY['dimacs-cnf'],
      output_format => 'ddnnf-nnf',
      parser        => 'nnf',
      argtpl        => '-dDNNF {in} -out={out}',
      preference    => 120);

That compiler is then usable immediately:

.. code-block:: postgresql

    SELECT probability_evaluate(t, 'compilation', 'd4-custom') FROM mytable;

:sqlfunc:`unregister_tool` removes a tool (a seeded default becomes hidden);
:sqlfunc:`set_tool_enabled` turns a tool off or on without forgetting its
configuration; :sqlfunc:`set_tool_preference` reorders it. Each errors on an
unknown name rather than silently doing nothing:

.. code-block:: postgresql

    SELECT provsql.set_tool_enabled('dsharp', false);  -- stop offering dsharp
    SELECT provsql.set_tool_preference('c2d', 95);     -- prefer c2d over d4v2

KCMCP servers (``kind = kcmcp``)
--------------------------------

A ``kcmcp`` tool compiles over a **warm, socket-attached server** speaking the
:doc:`KCMCP protocol </dev/kc-server-protocol>` instead of spawning a process
per call. ProvSQL keeps one connection per backend for the session's life, so
repeated compilations skip the connect/handshake (and a future caching engine
keeps its cross-query cache warm). The standalone ``tdkc --kcmcp`` is a
reference server; in practice the server drives a knowledge compiler such as
d4. The result is byte-for-byte the same d-DNNF as the CLI path, so probability
/ Shapley results are identical, and ProvSQL falls back to the CLI path if the
server is unreachable.

There are two ways to point a ``kcmcp`` tool at a server.

**Endpoint mode** — connect to a server you run and supervise yourself, at a
fixed address (a local Unix socket, or ``host:port`` for a remote server):

.. code-block:: postgresql

    SELECT provsql.register_tool(
      'kc-server', kind => 'kcmcp',
      operations    => ARRAY['compile'],
      input_formats => ARRAY['dimacs-cnf'],
      output_format => 'ddnnf-nnf',
      parser        => 'nnf',
      endpoint      => 'unix:/run/provsql/kc.sock');
    SELECT probability_evaluate(t, 'compilation', 'kc-server') FROM mytable;

**Managed mode** — let ProvSQL launch and supervise the server. Set
:ref:`provsql.kcmcp_server <provsql-kcmcp-server>` to the launch command (with
a ``{endpoint}`` placeholder); a supervisor background worker starts it,
publishes its address, and restarts it if it exits. Register a tool whose
``endpoint`` is the literal ``managed``:

.. code-block:: postgresql

    -- in postgresql.conf, or:
    ALTER SYSTEM SET provsql.kcmcp_server = 'tdkc --kcmcp {endpoint}';
    SELECT pg_reload_conf();

    SELECT provsql.register_tool(
      'kc-managed', kind => 'kcmcp',
      operations => ARRAY['compile'], input_formats => ARRAY['dimacs-cnf'],
      output_format => 'ddnnf-nnf', parser => 'nnf',
      endpoint => 'managed');

KCMCP v1 has no authentication, so a remote ``host:port`` endpoint must be
secured out of band (a private network, an SSH tunnel, or a TLS proxy); a local
Unix socket is scoped by filesystem permissions. See the
:doc:`protocol page </dev/kc-server-protocol>` for the wire format and the
managed-server architecture.

Persistence
-----------

Registry changes are persisted in the ``provsql.tool_overrides`` table,
overlaid on the compiled-in defaults, so a registration made in one session is
honoured by every backend and survives a server restart. The table is marked
as extension configuration, so ``pg_dump`` carries your registrations with the
database. An empty ``provsql.tool_overrides`` means exactly the built-in
defaults; to reset everything, ``DELETE FROM provsql.tool_overrides``.

.. _tool-registry-security:

Security
--------

**A tool record names an executable that ProvSQL runs as the PostgreSQL
operating-system user.** Editing the registry is therefore equivalent to
OS-level trust on the server account, exactly like setting
:ref:`provsql.tool_search_path <provsql-tool-search-path>` or dropping a binary
into a directory on it. Consequently:

- :sqlfunc:`register_tool`, :sqlfunc:`unregister_tool`,
  :sqlfunc:`set_tool_enabled` and :sqlfunc:`set_tool_preference` are
  **superuser-only**. A non-superuser can read ``provsql.tools`` and *select*
  among the already-registered tools, but cannot add one or repoint a binary,
  so gains no command execution it did not already have.
- Keep tool binaries somewhere the PostgreSQL user can reach but ordinary
  users cannot tamper with. In particular, do **not** put binaries the server
  runs under a ``$HOME``-reachable, user-writable path.
- ProvSQL does not sandbox a tool: a registered ``argtpl`` is a shell command
  line. Treat tool records and ``provsql.tool_search_path`` as trusted input.
