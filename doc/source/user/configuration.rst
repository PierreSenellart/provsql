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
        for every commutative (m-)semiring, recursive queries over cyclic
        data included (see :doc:`querying`).

    ``'absorptive'``
        Circuits may additionally be valid only for *absorptive*
        semirings (those where :math:`1 \oplus a = 1`: probability,
        Boolean, min-plus over nonnegative costs, Viterbi…). Recursive
        reachability on bounded-treewidth data is then compiled so that
        it evaluates exactly under all of them, e.g., min-cost
        reachability (see :doc:`semirings` and :doc:`probabilities`),
        and circuits are simplified accordingly.

    ``'boolean'``
        Implies ``'absorptive'``, and additionally enables every
        optimisation sound only when provenance is interpreted as a
        Boolean function, chiefly the safe-query rewriting, which
        computes in linear time the probability of self-join-free
        hierarchical queries over TID / BID tables. Semirings that are
        not Boolean-faithful refuse to evaluate the resulting circuits
        (see :doc:`probabilities` and the :ref:`compatibility note
        <semiring-boolean-compat>` in :doc:`semirings`), and the
        multiplicity of result rows may change.

``provsql.update_provenance`` (default: ``off``)
    Enable provenance tracking for ``INSERT``, ``UPDATE``, and ``DELETE``
    statements (see :doc:`data-modification`). Requires PostgreSQL ≥ 14.

.. _provsql-implicit-freeze:

``provsql.implicit_freeze`` (default: ``'warn'``)
    What happens when part of a query cannot be tracked (see
    :ref:`plain-sql`) while the rest of the statement tracks the same
    relations: ``'warn'`` runs it with a ``WARNING``; ``'error'``
    refuses the query (SQLSTATE ``0A000``). A part marked with
    :sqlfunc:`plain`, or an aggregate read through an explicit cast
    (``count(*)::numeric``), is never refused.

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

    ProvSQL Studio enables it automatically (see :doc:`studio`).

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
    Developer debugging aid: checks the query tree after each step of the
    rewriting, and raises an error naming the step that broke it.

``provsql.trace_rewrite`` (default: ``off``)
    Developer debugging aid: a ``NOTICE`` for each step of the rewriting
    that changed the query.

``provsql.aggtoken_text_as_uuid`` (default: ``off``)
    When ``on``, an ``agg_token`` cell renders as its provenance UUID
    instead of ``"value (*)"`` (ProvSQL Studio sets this per session);
    :sqlfunc:`agg_token_value_text` gives the display string of such a
    UUID. Only the text output changes.

.. _provsql-monte-carlo-seed:

``provsql.monte_carlo_seed`` (default: ``-1``)
    Seed for all Monte Carlo sampling (probabilities and continuous
    distributions). The default ``-1`` seeds randomly; any other integer
    value (including ``0``) is used as a fixed seed, making
    ``probability_evaluate(..., 'monte-carlo', 'n')`` and random-variable
    sampling reproducible across runs.

.. _provsql-rv-mc-samples:

``provsql.rv_mc_samples`` (default: ``10000``)
    Sample count when :sqlfunc:`expected`, :sqlfunc:`variance`,
    :sqlfunc:`moment`, :sqlfunc:`rv_sample` or :sqlfunc:`rv_histogram`
    cannot compute a result exactly and falls back to sampling. Set to
    ``0`` to get an error instead, when only exact answers are
    acceptable. Does not affect ``probability_evaluate(...,
    'monte-carlo', 'n')``, whose sample count is an argument.

.. _provsql-ess-warn-fraction:

``provsql.ess_warn_fraction`` (default: ``0.1``)
    Posterior inference over latent variables warns that its estimate is
    unreliable when the effective sample size falls below this fraction
    of the accepted draws; raise ``provsql.rv_mc_samples`` then. ``0``
    silences the warning.

.. _provsql-simplify-on-load:

``provsql.simplify_on_load`` (default: ``on``)
    Simplify provenance circuits when they are loaded for evaluation
    (e.g., comparisons decidable from the supports of their operands
    become constants), for every consumer, :sqlfunc:`view_circuit` and
    Studio included. Set to ``off`` to inspect the raw circuit. See
    :doc:`continuous-distributions`.

.. _provsql-gate-cache-size:

``provsql.gate_cache_size`` (default: ``64MB``)
    Per-session cache of circuit gates. Raise it for queries that create
    many gates before reading them back (a hash aggregate over a large
    table); count roughly 100 bytes per gate.

.. _provsql-max-memory:

``provsql.max_memory`` (default: ``0``, no limit)
    Memory the evaluations of a statement (semiring, probability,
    Shapley values…), external tools included, may add to the backend;
    beyond it, the evaluation stops with a ``program_limit_exceeded``
    error (SQLSTATE ``54000``, ``provsql-reason: memory-limit``). The
    check is periodic, so the limit is approximate. Linux, macOS and
    FreeBSD only. Example: ``SET provsql.max_memory = '1GB'``.

.. _provsql-max-worlds:

``provsql.max_worlds`` (default: ``1048576``, that is 2\ :sup:`20`)
    Maximum number of possible worlds of a group enumerated to evaluate a
    condition on its aggregate that has no closed form (a comparison on
    a ``SUM``, on two aggregates…; up to 2\ :sup:`n` worlds for *n*
    rows). Beyond it, the evaluation stops with a
    ``program_limit_exceeded`` error (SQLSTATE ``54000``,
    ``provsql-reason: world-limit``). ``0`` for no limit.

.. _provsql-store-synchronous-commit:

``provsql.store_synchronous_commit`` (default: ``off``)
    Flush the provenance circuit to stable storage before a transaction
    that wrote to it commits, so that a machine crash cannot lose the
    gates of recently committed transactions (see :doc:`persistence`).
    Costs one flush per such transaction, and provenance queries write
    to the circuit, reads included.

.. _provsql-store-wal-logging:

``provsql.store_wal_logging`` (default: ``off``, PostgreSQL 15+)
    Write every modification of the circuit to the WAL, so that a
    physical standby carries the provenance the primary computed.
    Requires ``provsql.store_synchronous_commit``; provenance queries
    then do not run on the standby. **Superuser only**. See
    :doc:`persistence`.

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

    **Superuser only** (or, on PostgreSQL 15+, a role granted ``SET`` on
    the parameter), since it decides which executables the server runs;
    an administrator can pin it for other roles with ``ALTER ROLE ...
    SET``.

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
    Read-only: the probability evaluation method(s) used by the most
    recent :sqlfunc:`probability_evaluate` call, to see what the
    automatic selection chose; see :ref:`route-methods`.

.. _provsql-joint-max-treewidth:

``provsql.joint_max_treewidth`` (default: ``10``)
    Maximum joint treewidth the joint-width UCQ probability compiler
    attempts. Above this bound, evaluation falls back to the other
    probability methods.

.. _provsql-joint-max-states:

``provsql.joint_max_states`` (default: ``65536``)
    Maximum number of dynamic-programming states per bag of the
    joint-width UCQ probability compiler; above it, evaluation falls back
    to the other probability methods.

.. _provsql-mobius-max-gates:

``provsql.mobius_max_gates`` (default: ``4000000``)
    Cap, in number of gates built, on the data cost of the safe-UCQ
    Möbius-inversion probability route; above it, evaluation falls back
    to the joint-width compiler or the other probability methods.

.. _provsql-mobius-max-cnf:

``provsql.mobius_max_cnf`` (default: ``8``)
    Cap on the query cost of the safe-UCQ Möbius-inversion probability
    route (the number of conjuncts in the CNF of each sentence, a cost
    exponential in it). Only a very large union, or a self-joining query,
    reaches it. ``0`` disables the cap.

.. _provsql-kcmcp-server:

``provsql.kcmcp_server`` (default: empty)
    Launch command for a **managed** KCMCP knowledge-compiler server (see
    :doc:`the KCMCP server protocol </dev/kc-server-protocol>`). When
    non-empty, ProvSQL keeps a server started with this command running,
    and a registry tool of ``kind = 'kcmcp'`` whose ``endpoint`` is
    ``'managed'`` compiles over it. ``{endpoint}`` is replaced by a
    Unix-socket path (``unix:`` scheme included). Example:

    .. code-block:: postgresql

        ALTER SYSTEM SET provsql.kcmcp_server = 'tdkc --kcmcp {endpoint}';
        SELECT pg_reload_conf();

    Set in the configuration file or with ``ALTER SYSTEM``, and applied
    on reload.

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

``provsql.build_id()`` returns the commit the loaded library was built
from, as ``git describe`` gives it (e.g., ``v1.12.0-431-g577cc730``), or
``unknown`` for a build outside a git checkout. Quote it in a bug report
on a development version.
