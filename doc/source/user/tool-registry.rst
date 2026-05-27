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

Out of the box the registry is seeded with exactly the tools ProvSQL has
always known about and their usual invocations, so nothing needs to be
configured: a fresh database behaves identically to before the registry
existed.

The catalog: ``provsql.tools``
------------------------------

The read-only view :sqlfunc:`tool_available`'s companion, ``provsql.tools``,
lists every registered tool::

    SELECT name, operations, executable, available, preference, enabled
    FROM provsql.tools ORDER BY preference DESC, name;

Columns:

``name``
    Logical id, e.g. ``d4`` or ``panini-obdd``. This is the value
    :sqlfunc:`probability_evaluate` and :ref:`provsql.fallback_compiler
    <provsql-fallback-compiler>` accept.
``kind``
    ``cli`` (a command-line tool; the only kind today).
``executable``
    The binary resolved on the backend's ``PATH`` (empty for a pipeline tool
    such as ``dpmc``, which runs ``htb | dmc``).
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
``preference``
    Selection order within an operation (higher first).
``enabled``
    Whether the dispatchers may select it.
``available``
    Whether the executable and every dependency currently resolve on the
    backend's ``PATH`` (honouring :ref:`provsql.tool_search_path
    <provsql-tool-search-path>`). This is exactly the
    :sqlfunc:`tool_available` test, applied to each tool.

Selection
---------

When you name a tool explicitly (``probability_evaluate(t, 'compilation',
'd4')`` or ``probability_evaluate(t, 'wmc', 'ganak')``), ProvSQL uses it, or
raises a clear error if it is unknown, disabled, or not on ``PATH``.

When you do **not** name one (the ``'compilation'`` method with no compiler,
or the ``'wmc'`` method with no counter), ProvSQL picks the **highest-
preference enabled tool whose binary resolves on ``PATH``** for that
operation. :ref:`provsql.fallback_compiler <provsql-fallback-compiler>`
governs only the final fallback route of the default probability method.

Managing tools
--------------

Four superuser-only functions edit the registry:

:sqlfunc:`register_tool` adds a tool, or replaces the record with the same
name. Its capability triple uses the KCMCP names; ``argtpl`` is the command
template, with ``{in}`` / ``{out}`` substituted by the input/output temporary
files (and ``{binary}`` / ``{tmpdir}`` / ``{pivotAC}`` where a tool needs
them). For example, registering a second build of ``d4`` kept under a
specific path::

    SELECT provsql.register_tool(
      'd4-jm62300',
      executable    => '/opt/d4-jm62300/d4',
      operations    => ARRAY['compile'],
      input_formats => ARRAY['dimacs-cnf'],
      output_format => 'ddnnf-nnf',
      parser        => 'nnf',
      argtpl        => '-dDNNF {in} -out={out}',
      preference    => 120);

That compiler is then usable immediately::

    SELECT probability_evaluate(t, 'compilation', 'd4-jm62300') FROM …;

:sqlfunc:`unregister_tool` removes a tool (a seeded default becomes hidden);
:sqlfunc:`set_tool_enabled` turns a tool off or on without forgetting its
configuration; :sqlfunc:`set_tool_preference` reorders it. Each errors on an
unknown name rather than silently doing nothing::

    SELECT provsql.set_tool_enabled('dsharp', false);  -- stop offering dsharp
    SELECT provsql.set_tool_preference('c2d', 95);     -- prefer c2d over d4v2

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
  so gains no command execution it did not already have. This is no new
  exposure: anyone able to register a tool could already place a binary on the
  server's ``PATH``.
- Keep tool binaries somewhere the PostgreSQL user can reach but ordinary
  users cannot tamper with. In particular, do **not** put binaries the server
  runs under a ``$HOME``-reachable, user-writable path.
- ProvSQL does not sandbox a tool: a registered ``argtpl`` is a shell command
  line. Treat tool records and ``provsql.tool_search_path`` as trusted input.
