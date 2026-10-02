Configuration Reference
========================

ProvSQL is controlled by `GUC (Grand Unified Configuration)
<https://www.postgresql.org/docs/current/config-setting.html>`_ variables,
all settable per session with ``SET`` or permanently in
`postgresql.conf <https://www.postgresql.org/docs/current/config-setting.html>`_
or with `ALTER DATABASE <https://www.postgresql.org/docs/current/sql-alterdatabase.html>`_
/ `ALTER ROLE <https://www.postgresql.org/docs/current/sql-alterrole.html>`_.

.. _provsql-active:

``provsql.active`` (default: ``on``)
    Master switch. When ``off``, ProvSQL drops all provenance annotations
    silently, as if the extension were not loaded. Useful to temporarily
    disable provenance tracking without unloading the extension.

.. _provsql-provenance-class:
.. _provsql-boolean-provenance:

``provsql.provenance`` (default: ``'semiring'``)
    The *provenance class* of the session: the most specific class of
    provenance semantics circuits must remain valid for. A circuit built
    under a narrower class records it, and evaluating it under a semiring
    outside that class raises an error. From the most general to the most
    specialised:

    ``'where'``
        Universal semiring provenance *plus* where-provenance tracking
        (see :doc:`where-provenance`): each output value records its
        source cell. Not the default due to overhead.

    ``'semiring'``
        Universal semiring provenance (the default): circuits are valid
        for every commutative (m-)semiring. A recursive query in which a
        tuple is derived through itself is rejected: cyclic data does
        that, and so does a null-padded row that re-derives itself, or a
        projection onto constants, on acyclic data.

    ``'absorptive'``
        Circuits may additionally be valid only for *absorptive*
        semirings (those where :math:`1 \oplus a = 1`: probability,
        Boolean, min-plus over nonnegative costs, Viterbi…).
        Concretely:

        * a recursive query in which a tuple is derived through itself
          (over **cyclic** data, or through a null-padded row that
          re-derives itself on acyclic data) stops once every minimal,
          tuple-repetition-free derivation is covered, instead of
          failing; non-absorptive evaluations (counting,
          why-provenance) refuse the resulting tokens, following
          :cite:`DBLP:conf/icdt/DeutchMRT14`;
        * **recursive reachability on bounded-treewidth data** is
          compiled so that it evaluates exactly for probability and for
          every absorptive semiring, e.g., min-cost reachability through
          nonnegative min-plus (see :doc:`semirings` and
          :doc:`probabilities`);
        * circuits are simplified with the identities of absorptive
          semirings (:math:`a \oplus a = a`, :math:`1 \oplus a = 1`,
          :math:`a \oplus a \otimes b = a`).

    ``'boolean'``
        Implies ``'absorptive'``, and additionally enables every
        optimisation sound only when provenance is interpreted as a
        Boolean function:

        * **Safe-query rewriting.** Self-join-free hierarchical
          conjunctive queries (and UCQs of such queries) over TID / BID
          base tables are rewritten so that their probability is computed
          in linear time. Other queries are unchanged.

        * **Boolean circuit simplification**, with the identities
          :math:`a \otimes a = a` and :math:`a \otimes (a \oplus b) = a`,
          independently of
          :ref:`provsql.simplify_on_load <provsql-simplify-on-load>`.

        Semirings that are not Boolean-faithful refuse to evaluate the
        resulting circuits; see :doc:`probabilities` and the
        :ref:`compatibility note <semiring-boolean-compat>` in
        :doc:`semirings`. Not the default because the rewriting changes
        the multiset of result rows, so it is unsuitable for per-row
        provenance and for non-Boolean-faithful semirings.

``provsql.update_provenance`` (default: ``off``)
    Enable provenance tracking for ``INSERT``, ``UPDATE``, and ``DELETE``
    statements (see :doc:`data-modification`). Requires PostgreSQL ≥ 14.

.. _provsql-implicit-freeze:

``provsql.implicit_freeze`` (default: ``'warn'``)
    What a part of a query whose provenance is not tracked (see
    :ref:`plain-sql`) does when the rest of the statement tracks the same
    relations. ``'warn'`` emits a
    ``WARNING`` naming such a relation; ``'error'`` refuses the query
    (SQLSTATE ``0A000``, ``feature_not_supported``). A
    part that reads only relations the rest does not track, that is
    marked with :sqlfunc:`plain`, or an aggregate result read through an
    explicit cast (``count(*)::numeric``), is never refused.

.. _provsql-classify-top-level:

``provsql.classify_top_level`` (default: ``off``)
    Emit a ``NOTICE`` for every top-level ``SELECT`` reporting the
    certified kind of the result relation under the
    ``provsql_table_kind`` taxonomy (``TID`` / ``BID`` / ``OPAQUE``) and
    the provenance-tracked base relations it touches:

    .. code-block:: text

        NOTICE:  ProvSQL: query result is TID (sources: public.personnel)
        NOTICE:  ProvSQL: query result is OPAQUE
        NOTICE:  ProvSQL: query result is TID (no provenance-tracked sources)

    The source list is reported for ``TID`` and ``BID`` results
    (including the explicit ``no provenance-tracked sources`` marker
    for the deterministic case) but omitted for ``OPAQUE`` results,
    where it could be incomplete. Only the outermost statement is
    reported.

    ProvSQL Studio enables this GUC automatically and renders the
    certified kind on the result-table provenance pill; see
    :doc:`studio`.

.. _provsql-verbose-level:

``provsql.verbose_level`` (default: ``0``)
    Controls the verbosity of ProvSQL diagnostic messages. ``0`` is
    silent; ``1``–``9`` enable informational messages, ``10``–``100``
    debug information. Thresholds include:

    * **≥ 1**: report the safe-query / inversion-free certificate
      attached to a rewritten query.
    * **≥ 5**: approximation guarantees of sampling-based probability
      methods, comparison-resolution summaries, reasons for declining the
      safe-query rewrite.
    * **≥ 10**: notices from the SQL-level evaluators (e.g., when the
      reachability route falls back to the generic path).
    * **≥ 20**: print the SQL query before and after provenance
      rewriting (requires PostgreSQL ≥ 15); report the knowledge
      compilation method chosen and the size of its result.
    * **≥ 25**: report the gate count of a d-DNNF obtained by tree
      decomposition.
    * **≥ 30**: debug traces of the probability evaluators and the
      safe-query detector; keep all intermediate temporary files
      (Tseytin, d-DNNF, DOT) instead of deleting them, and report where
      they are.
    * **≥ 40**: also print the time spent on rewriting.
    * **≥ 50**: also print the full parse tree of the query before and
      after rewriting, and the cost-calibration notices of the
      probability method chooser.

``provsql.verify_rewrite`` (default: ``off``)
    A debugging aid for developers: checks the query tree after every
    step of the rewriting, and raises an error naming the step after which
    a column reference, the numbering of the result columns, an aggregate
    or a permission index became inconsistent.

``provsql.trace_rewrite`` (default: ``off``)
    A debugging aid for developers: a ``NOTICE`` for each step of the
    rewriting that changed the query, with the nesting level it ran at.

``provsql.aggtoken_text_as_uuid`` (default: ``off``)
    Controls how an ``agg_token`` cell renders as text. By default it
    renders as ``"value (*)"``, where *value* is the aggregate value.
    When set to ``on``, it renders as the underlying provenance UUID
    instead (ProvSQL Studio sets this per session); the display string
    of such a UUID is obtained with :sqlfunc:`agg_token_value_text`. Has
    no effect on ``EXPLAIN`` output, on storage, or on numeric / casting
    behaviour of ``agg_token``.

.. _provsql-monte-carlo-seed:

``provsql.monte_carlo_seed`` (default: ``-1``)
    Seed for all Monte Carlo sampling (probabilities and continuous
    distributions). The default ``-1`` seeds randomly; any other integer
    value (including ``0``) is used as a fixed seed, making
    ``probability_evaluate(..., 'monte-carlo', 'n')`` and random-variable
    sampling reproducible across runs.

.. _provsql-rv-mc-samples:

``provsql.rv_mc_samples`` (default: ``10000``)
    Default sample count when :sqlfunc:`expected`, :sqlfunc:`variance`,
    :sqlfunc:`moment`, :sqlfunc:`rv_sample` or :sqlfunc:`rv_histogram`
    cannot compute a result analytically and falls back to sampling.
    :sqlfunc:`expected`, :sqlfunc:`variance` and :sqlfunc:`moment` first
    compute the result exactly, by enumerating possible worlds, when it
    involves no continuous random variable and at most 20 input tuples.
    Set to ``0`` to disable sampling: these functions then raise an
    error instead, which is useful when only exact answers are
    acceptable. Unrelated to
    ``probability_evaluate(..., 'monte-carlo', 'n')`` where the sample
    count is an explicit argument.

.. _provsql-ess-warn-fraction:

``provsql.ess_warn_fraction`` (default: ``0.1``)
    Effective-sample-size warning threshold for posterior inference over
    latent variables (likelihood weighting). When the effective sample
    size falls below this fraction of the accepted draws, a warning is
    emitted: the estimate is unreliable (raise
    ``provsql.rv_mc_samples``, or the model has many observations per
    latent variable). Set to ``0`` to silence the warning.

.. _provsql-simplify-on-load:

``provsql.simplify_on_load`` (default: ``on``)
    Simplify provenance circuits when they are loaded for evaluation:
    comparisons decidable from the supports of their operands become
    constant (probability ``0`` or ``1``), deterministic arithmetic is
    folded, and semiring identities are applied. The result is the same
    for every consumer (semiring evaluation, Monte Carlo,
    :sqlfunc:`view_circuit`, PROV-XML export, ProvSQL Studio). Set to
    ``off`` to inspect the raw circuit structure. See
    :doc:`continuous-distributions`.

.. _provsql-gate-cache-size:

``provsql.gate_cache_size`` (default: ``64MB``)
    Size of the per-backend cache of circuit gates the session created or
    read. Gates beyond it are read back from the store when needed, which
    is slower. Raise it for queries that create many gates before reading
    them back (a hash aggregate over a large table); count roughly 100
    bytes per gate.

.. _provsql-max-memory:

``provsql.max_memory`` (default: ``0``, no limit)
    Memory the evaluations of a statement (a semiring, a probability, a
    moment, a sample, Shapley values...) may add to the backend: once its
    resident memory has grown by more than this since the statement's
    first evaluation, the evaluation stops with a
    ``program_limit_exceeded`` error (SQLSTATE ``54000``, tagged
    ``provsql-reason: memory-limit``). An external tool it runs (``d4``,
    ``c2d``...), with the processes the tool forks, is counted with that
    growth, and stopped once the two together exceed the budget. The
    check is periodic, so the budget can be exceeded by what is allocated
    between two checks. Available on Linux, macOS and FreeBSD; elsewhere
    the setting has no effect. Example: ``SET provsql.max_memory = '1GB'``.

.. _provsql-max-worlds:

``provsql.max_worlds`` (default: ``1048576``, that is 2\ :sup:`20`)
    Possible worlds of a group that a condition on its aggregate may have
    enumerated. A condition with no closed form (a comparison on a ``SUM``,
    on two aggregates, on an ``array_agg``...) is evaluated by enumerating
    the worlds of the group, up to 2\ :sup:`n` for *n* rows, in time and
    in memory: beyond this many, the evaluation stops with a
    ``program_limit_exceeded`` error (SQLSTATE ``54000``, tagged
    ``provsql-reason: world-limit``) rather than running out of memory.
    ``0`` for no limit.

.. _provsql-store-synchronous-commit:

``provsql.store_synchronous_commit`` (default: ``off``)
    Force the provenance circuit to stable storage before a transaction
    that wrote to it commits. The circuit is not protected by
    PostgreSQL's WAL (see :doc:`persistence`): on a power loss, gates of
    recently committed transactions can be lost, and a lost gate silently
    reads back as an independent input with probability 1. With ``off``,
    the loss is bounded by the flush interval of the background worker,
    as ``synchronous_commit = off`` bounds that of the table data; ``on``
    removes it, at the price of one flush per transaction that writes to
    the circuit. Provenance queries write to the circuit, reads included.

.. _provsql-store-wal-logging:

``provsql.store_wal_logging`` (default: ``off``, PostgreSQL 15+)
    Write every modification of the circuit to the WAL, so that a
    physical standby replays it and carries the provenance the primary
    computed. Requires ``provsql.store_synchronous_commit``. With it on,
    provenance queries do not run on a hot standby, which refuses to
    write to the circuit. **Superuser only**. See :doc:`persistence`.

.. _provsql-tool-search-path:

``provsql.tool_search_path`` (default: empty)
    Colon-separated list of directories prepended to ``PATH`` when ProvSQL
    runs external command-line tools: every tool of the
    ``provsql.tools`` registry (the d-DNNF compilers ``d4``, ``d4v2``,
    ``c2d``, ``minic2d``, ``dsharp``, the model counters, the GraphViz
    ASCII renderer ``graph-easy``, admin-registered tools…; see
    :doc:`tool-registry`). Tools of ``kind = 'kcmcp'`` are reached over
    a socket and do not involve ``PATH``. The
    server's ``PATH`` is searched as a fallback, so an entry here only needs
    to be set when a tool lives outside the server's default ``PATH`` (e.g.,
    in ``$HOME/local/bin``, a Conda environment, ``/opt/...``). Example:

    .. code-block:: postgresql

        SET provsql.tool_search_path = '/opt/d4:/home/postgres/bin';

    **Superuser only**, since it decides which executables run under the
    server account: only a superuser (or, on PostgreSQL 15 and later, a
    role explicitly granted ``SET`` on the parameter) may change it. A
    non-superuser session uses whatever value an administrator pins for it
    (for example with ``ALTER ROLE ... SET provsql.tool_search_path``) or
    the server's default ``PATH``.

.. _provsql-fallback-compiler:

``provsql.fallback_compiler`` (default: ``d4``)
    Name of the external compiler :sqlfunc:`probability_evaluate` (with
    the empty or ``'default'`` method) invokes as the **final fallback**,
    when no in-process method succeeds. Accepts any compiler
    name :sqlfunc:`probability_evaluate` accepts under the ``'compilation'``
    method: ``d4`` (default), ``d4v2``, ``c2d``, ``minic2d``, ``dsharp``,
    ``panini-obdd``, ``panini-obdd-and``, ``panini-decdnnf``. Useful on
    hosts where ``d4`` is not installed but another compiler is. Example:

    .. code-block:: postgresql

        SET provsql.fallback_compiler = 'c2d';

.. _provsql-last-eval-method:

``provsql.last_eval_method`` (default: empty)
    Read-only report of the probability evaluation method(s) used by
    the most recent :sqlfunc:`probability_evaluate` call (comma-separated
    and deduplicated), to see which method the default automatic
    selection chose. The safe-query rewriting, the joint-width UCQ
    compiler and the reachability compiler report as ``sq-rewrite``,
    ``bounded-jw`` and ``reachability``; see :ref:`route-methods`.

.. _provsql-joint-max-treewidth:

``provsql.joint_max_treewidth`` (default: ``10``)
    Maximum joint treewidth the joint-width UCQ probability compiler
    attempts. Above this bound, evaluation falls back to the other
    probability methods.

.. _provsql-joint-max-states:

``provsql.joint_max_states`` (default: ``65536``)
    Maximum number of dynamic-programming states per bag of the
    joint-width UCQ probability compiler; above it, evaluation falls back
    to the other probability methods. This cap, more than the treewidth
    bound above, limits the cost of the compiler.

.. _provsql-mobius-max-gates:

``provsql.mobius_max_gates`` (default: ``4000000``)
    Cap, in number of gates built, on the data cost of the safe-UCQ
    Möbius-inversion probability route; above it, evaluation falls back
    to the joint-width compiler or the other probability methods.

.. _provsql-mobius-max-cnf:

``provsql.mobius_max_cnf`` (default: ``8``)
    Cap on the query cost of the safe-UCQ Möbius-inversion probability
    route: the maximum number :math:`M` of conjuncts in the CNF of each
    sentence, whose inclusion-exclusion lattice has :math:`2^M` elements.
    Only a very large union, or a self-joining query, reaches it. ``0``
    disables the cap.

.. _provsql-kcmcp-server:

``provsql.kcmcp_server`` (default: empty)
    Launch command for a **managed** KCMCP knowledge-compiler server (see
    :doc:`the KCMCP server protocol </dev/kc-server-protocol>`). When
    non-empty, ProvSQL runs this command to start a server, restarts it
    if it exits, and a registry tool of ``kind = 'kcmcp'`` whose
    ``endpoint`` is ``'managed'`` compiles over that server instead of
    spawning a process per call. The literal ``{endpoint}`` is replaced
    by a Unix-socket path (it already carries the ``unix:`` scheme).
    Empty (default) launches no server. Example:

    .. code-block:: postgresql

        ALTER SYSTEM SET provsql.kcmcp_server = 'tdkc --kcmcp {endpoint}';
        SELECT pg_reload_conf();

    Set in the configuration file or with ``ALTER SYSTEM``, and applied
    on reload; not settable per session, since it runs an arbitrary
    command as the PostgreSQL operating-system user.

All variables above **except** ``provsql.tool_search_path``,
``provsql.store_wal_logging`` and ``provsql.kcmcp_server`` can be changed by any
user for their own session.

.. _search-path:

Schema and ``search_path``
--------------------------

ProvSQL installs all its types, functions, and operators into a schema
named ``provsql``. Functions and operators are resolved through
PostgreSQL's `search_path
<https://www.postgresql.org/docs/current/ddl-schemas.html#DDL-SCHEMAS-PATH>`_,
so unless ``provsql`` is on the path you must qualify every name
(``provsql.expected(...)``, ``OPERATOR(provsql.+)``…). The convenient
setup keeps ``provsql`` on the path so unqualified names just work:

.. code-block:: postgresql

    -- per database (persistent; affects new sessions):
    ALTER DATABASE mydb SET search_path = "$user", public, provsql;

    -- or for the current session only:
    SET search_path TO "$user", public, provsql;

What goes wrong without it
^^^^^^^^^^^^^^^^^^^^^^^^^^^

When ``provsql`` is not on the path:

* **Random-variable comparisons and arithmetic** (``v < w``, ``v + w``,
  ``sum(v)`` over a ``random_variable`` column ``v``) raise
  ``operator does not exist: provsql.random_variable …``.

* **Aggregate-token comparisons** on a materialised ``agg_token`` column
  (``WHERE s > 15``) likewise raise an error, instead of silently
  comparing the bare value and losing the provenance.

* **Plain ProvSQL function calls** (``expected(...)``, ``provenance()``,
  the ``sr_*`` semiring evaluators, ``probability(...)``…) raise
  ``function … does not exist``.

The fix is always the same: put ``provsql`` on the ``search_path`` (or
qualify the name).

The ``setup_search_path()`` helper
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``CREATE EXTENSION provsql`` prints a ``NOTICE`` when the database's
default ``search_path`` does not include ``provsql``. The bundled
helper does the edit for you:

.. code-block:: postgresql

    SELECT provsql.setup_search_path();

It appends ``provsql`` to the database's ``search_path`` setting if it
is not already present (keeping the existing entries in order), and
applies the result with ``ALTER DATABASE``. It is idempotent and reports
what it did with a ``NOTICE``. Only **new** sessions pick up the change
-- reconnect (or ``SET search_path`` in the current session) to use
unqualified names right away. The caller must be the database owner or a
superuser, and role-level ``search_path`` settings (if any) take
precedence over the database-level one and are left untouched.

ProvSQL never edits your ``search_path`` on its own: ``CREATE EXTENSION``
only advises, and ``setup_search_path()`` runs only when you call it.

The Build in Use
----------------

The extension's version, ``1.13.0-dev`` for instance, is the same for every
build of a development cycle. ``provsql.build_id()`` tells the builds apart: it
returns the commit the loaded library was built from, as
``git describe --tags --always --dirty`` gives it (e.g.,
``v1.12.0-431-g577cc730``, with ``-dirty`` for a build from uncommitted
changes), or ``unknown`` for a build outside a git checkout. It is worth
quoting in a bug report, or recording beside results that depend on the
build.
