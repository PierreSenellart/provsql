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
    provenance semantics circuits must remain faithful for.
    Constructions are licensed accordingly, and any construction that
    narrows a circuit's validity records it *in the circuit* (an
    ``assumed`` marker gate), so evaluation under a semiring outside
    the recorded class refuses with a clear error rather than
    returning an unjustified value.  From the most general to the most
    specialised:

    ``'where'``
        Universal semiring provenance *plus* where-provenance tracking
        (see :doc:`where-provenance`): ``project`` and ``eq`` gates
        record the source cell of each output value.  Not the default
        due to overhead.

    ``'semiring'``
        Universal semiring provenance (the default): circuits are
        faithful for every commutative (m-)semiring.  Recursive queries
        require the structural fixpoint, so cyclic data is rejected.

    ``'absorptive'``
        Circuits may additionally be sound only for *absorptive*
        semirings (those where :math:`1 \oplus a = 1`: probability,
        Boolean, min-plus over nonnegative costs, Viterbi…).
        Concretely:

        * a recursive query over **cyclic** data stops at the
          absorptive value fixpoint -- once every minimal,
          tuple-repetition-free derivation is covered, the longer
          (cyclic) ones being absorbed -- instead of failing; the
          resulting tokens carry the ``'absorptive'`` assumption
          marker, and non-absorptive evaluations (counting,
          why-provenance -- genuinely infinite on cyclic data) refuse
          them, following :cite:`DBLP:conf/icdt/DeutchMRT14`.
        * **recursive reachability on bounded-treewidth data**
          compiles along a tree decomposition of the data graph into
          certified d-Ds (deterministic and decomposable, but not in
          negation normal form; see :doc:`probabilities`), exact for
          probability and for every absorptive semiring -- e.g.
          min-cost reachability through nonnegative min-plus (see
          :doc:`semirings`); the materialised tokens carry the
          ``'absorptive'`` marker too.
        * at circuit-load time, the simplification rules sound in every
          absorptive semiring apply -- plus-idempotence
          (:math:`a \oplus a = a`), the plus-with-one absorber
          (:math:`1 \oplus a = 1`) and plus-absorbs-times
          (:math:`a \oplus a \otimes b = a`) -- with the rewritten
          gates marked so that non-absorptive evaluation refuses them.

    ``'boolean'``
        Implies ``'absorptive'``, and additionally enables every
        optimisation sound only when provenance is interpreted as a
        Boolean function:

        * **Planner-level safe-query rewriting.**  Self-join-free
          hierarchical conjunctive queries (and UCQs of such queries)
          over TID / BID base tables are rewritten with per-atom
          ``DISTINCT`` projections so the resulting provenance circuit
          is read-once and probability-evaluates in linear time.
          Queries outside the recognised class pass through unchanged.

        * **Load-time Boolean-only circuit simplification.**  On top
          of the ``'absorptive'`` rules above, the rewrites that hold
          for Boolean functions but not in general absorptive
          semirings: times-idempotence (:math:`a \otimes a = a`,
          which fails in min-plus) and times-absorbs-plus
          (:math:`a \otimes (a \oplus b) = a`, the lattice dual).
          Independent of
          :ref:`provsql.simplify_on_load <provsql-simplify-on-load>`.

        The rewritten gates are tagged (persistently for the rewriter,
        in a side-band set for the load-time simplifier) so that
        semirings whose algebra is not Boolean-faithful refuse to
        evaluate them; see :doc:`probabilities` and the
        :ref:`compatibility note <semiring-boolean-compat>` in
        :doc:`semirings`.  Not the default because the rewrites change
        the multiset of result rows / the underlying polynomial and are
        therefore unsound for per-row provenance interrogations and for
        non-Boolean-faithful semirings.

``provsql.update_provenance`` (default: ``off``)
    Enable provenance tracking for ``INSERT``, ``UPDATE``, and ``DELETE``
    statements (see :doc:`data-modification`). Requires PostgreSQL ≥ 14.

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
    for the deterministic case) but omitted for ``OPAQUE`` results:
    when the shape gate trips on a sublink, a set operation, a
    ``GROUP BY``, etc., the rtable walk only reaches the syntactically
    visible sources, so a printed list would be partial and
    misleadingly suggest completeness.

    The classifier runs on the user's parsed ``Query`` before any
    rewriting and only on the user's outermost statement; PL/pgSQL
    helpers the rewriter calls into (``provenance_times``,
    ``provenance_aggregate``…) do not produce extra notices.

    ProvSQL Studio enables this GUC automatically and renders the
    certified kind on the result-table provenance pill; see
    :doc:`studio`.

.. _provsql-verbose-level:

``provsql.verbose_level`` (default: ``0``)
    Controls the verbosity of ProvSQL diagnostic messages. ``0`` is silent.
    The meaningful thresholds are:

    * **≥ 20** – print the rewritten SQL query before and after provenance
      rewriting (requires PostgreSQL ≥ 15); print the Tseytin circuit and
      compiled d-DNNF filenames during knowledge compilation; report which
      d-DNNF method was chosen (direct interpretation, tree decomposition,
      or external compilation) and its gate count; keep all intermediate
      temporary files (Tseytin, d-DNNF, DOT) instead of deleting them.
    * **≥ 40** – also print the time spent by the planner on rewriting.
    * **≥ 50** – also print the full internal parse-tree representation of
      the query before and after rewriting.

``provsql.aggtoken_text_as_uuid`` (default: ``off``)
    Controls how an ``agg_token`` cell renders as text. By default the
    output function returns the human-friendly ``"value (*)"`` form, where
    *value* is the running aggregate state. When set to ``on``, it returns
    the underlying provenance UUID instead. UI layers (notably ProvSQL
    Studio) flip this on per session so aggregate cells expose the circuit
    root UUID for click-through; the user-facing display string is recovered
    via :sqlfunc:`agg_token_value_text` for any such UUID. Has no effect on
    ``EXPLAIN`` output, on the underlying storage, or on numeric / casting
    behaviour of ``agg_token``.

.. _provsql-monte-carlo-seed:

``provsql.monte_carlo_seed`` (default: ``-1``)
    Seed for the Monte Carlo sampler used throughout the
    probability and continuous-distribution paths. The default
    ``-1`` seeds from ``std::random_device`` for non-deterministic
    sampling; any other integer value (including ``0``) is used as
    a literal seed for ``std::mt19937_64``, making
    ``probability_evaluate(..., 'monte-carlo', 'n')`` reproducible
    across runs and across the Bernoulli and continuous
    (``gate_rv``) sampling paths.

.. _provsql-rv-mc-samples:

``provsql.rv_mc_samples`` (default: ``10000``)
    Default sample count for the Monte-Carlo fallback inside the
    analytical evaluators (:sqlfunc:`expected`, :sqlfunc:`variance`,
    :sqlfunc:`moment`, :sqlfunc:`rv_sample`, :sqlfunc:`rv_histogram`)
    when a sub-circuit cannot be decomposed and must be sampled.
    Set to ``0`` to disable the fallback entirely: callers raise an
    exception rather than sampling, which is useful when only
    analytical answers are acceptable. Unrelated to
    ``probability_evaluate(..., 'monte-carlo', 'n')`` where the sample
    count is an explicit argument.

.. _provsql-simplify-on-load:

``provsql.simplify_on_load`` (default: ``on``)
    Apply the universal peephole simplifier (currently the
    ``RangeCheck`` cmp-resolution pass; future passes plug into the
    same pipeline) when loading a provenance circuit from the
    mmap store into memory. Every comparator decidable from the
    propagated support intervals collapses to a Bernoulli
    ``gate_input`` with probability ``0`` or ``1``, transparent to
    every downstream consumer (semiring evaluators, Monte Carlo,
    ``view_circuit``, PROV-XML export, ProvSQL Studio). Set to
    ``off`` to inspect raw circuit structure (e.g. when debugging
    gate-creation paths). See :doc:`continuous-distributions` for
    the broader hybrid-evaluation context.

.. _provsql-tool-search-path:

``provsql.tool_search_path`` (default: empty)
    Colon-separated list of directories prepended to ``PATH`` when ProvSQL
    spawns external command-line tools: the d-DNNF compilers (``d4``,
    ``c2d``, ``minic2d``, ``dsharp``), the WeightMC weighted model counter
    (``weightmc``), and the GraphViz ASCII renderer (``graph-easy``). The
    server's ``PATH`` is searched as a fallback, so an entry here only needs
    to be set when a tool lives outside the server's default ``PATH`` (e.g.
    in ``$HOME/local/bin``, a Conda environment, ``/opt/...``). Example:

    .. code-block:: postgresql

        SET provsql.tool_search_path = '/opt/d4:/home/postgres/bin';

    **Superuser only.** This parameter dictates which directories the
    PostgreSQL server's operating-system user searches for executables, so
    letting an unprivileged role change it would let that role have an
    arbitrary binary run under the server account. It therefore has
    ``SUSET`` scope: only a superuser (or, on PostgreSQL 15 and later, a
    role explicitly granted ``SET`` on the parameter) may change it. A
    non-superuser session uses whatever value an administrator pins for it
    (for example with ``ALTER ROLE ... SET provsql.tool_search_path``) or
    the server's default ``PATH``.

.. _provsql-fallback-compiler:

``provsql.fallback_compiler`` (default: ``d4``)
    Name of the external compiler ProvSQL invokes as the **final fallback**
    in :sqlfunc:`probability_evaluate` (with the empty or ``'default'``
    method) when neither the direct interpret-as-d-DNNF reading nor the
    in-process tree-decomposition builder succeeds. Accepts any compiler
    name :sqlfunc:`probability_evaluate` accepts under the ``'compilation'``
    method: ``d4`` (default), ``d4v2``, ``c2d``, ``minic2d``, ``dsharp``,
    ``panini-obdd``, ``panini-obdd-and``, ``panini-decdnnf``. Useful on
    hosts where ``d4`` is not installed but another compiler is, or where
    you want benchmarks to converge on a single fallback. Example:

    .. code-block:: postgresql

        SET provsql.fallback_compiler = 'c2d';

.. _provsql-kcmcp-server:

``provsql.kcmcp_server`` (default: empty)
    Launch command for a **managed** KCMCP knowledge-compiler server (see
    :doc:`the KCMCP server protocol </dev/kc-server-protocol>`). When
    non-empty, a ProvSQL supervisor background worker runs this command to
    start a warm server, supervises it (restarting it if it exits), and
    publishes its address in shared memory; a registry tool of
    ``kind = 'kcmcp'`` whose ``endpoint`` is ``'managed'`` then compiles over
    that server instead of spawning a CLI process per call. The literal
    ``{endpoint}`` is replaced by a Unix-socket path the worker chooses (it
    already carries the ``unix:`` scheme). Empty (default) launches no server.
    Example:

    .. code-block:: postgresql

        ALTER SYSTEM SET provsql.kcmcp_server = 'tdkc --kcmcp {endpoint}';
        SELECT pg_reload_conf();

    Configured in the configuration file or with ``ALTER SYSTEM`` and applied
    on reload (``PGC_SIGHUP``): it runs an arbitrary command as the PostgreSQL
    operating-system user, so like ``provsql.tool_search_path`` it is not
    settable per session.

All variables above **except** ``provsql.tool_search_path`` and
``provsql.kcmcp_server`` have user-level scope: any user can change them for
their own session without superuser privileges. ``provsql.tool_search_path``
is superuser-only and ``provsql.kcmcp_server`` is config-file/reload-only, for
the security reasons given in their entries above.

.. _search-path:

Schema and ``search_path``
--------------------------

ProvSQL installs all its types, functions, and operators into a schema
named ``provsql``. Functions and operators are resolved through
PostgreSQL's `search_path
<https://www.postgresql.org/docs/current/ddl-schemas.html#DDL-SCHEMAS-PATH>`_,
so unless ``provsql`` is on the path you must qualify every name
(``provsql.expected(...)``, ``OPERATOR(provsql.+)`` …). The convenient
setup keeps ``provsql`` on the path so unqualified names just work:

.. code-block:: postgresql

    -- per database (persistent; affects new sessions):
    ALTER DATABASE mydb SET search_path = "$user", public, provsql;

    -- or for the current session only:
    SET search_path TO "$user", public, provsql;

What goes wrong without it
^^^^^^^^^^^^^^^^^^^^^^^^^^^

Crucially, while functions and operators follow ``search_path``, **casts
do not** -- a cast is bound to a type pair globally. When ``provsql`` is
absent from the path an operator lookup does not necessarily fail:
an implicit cast can reroute it to a built-in operator with different
semantics. ProvSQL therefore keeps the cross-domain casts that could do
this (``random_variable`` → ``uuid``, ``agg_token`` → ``numeric``) at
*assignment* level rather than *implicit*, precisely so that such a
misresolution becomes a clean error instead of a silent wrong result.
The practical consequences when ``provsql`` is not on the path:

* **Random-variable comparisons and arithmetic** (``v < w``, ``v + w``,
  ``sum(v)`` over a ``random_variable`` column ``v``) raise
  ``operator does not exist: provsql.random_variable …``.

* **Aggregate-token comparisons** on a materialised ``agg_token`` column
  (``WHERE s > 15``) likewise fail rather than silently comparing the
  bare scalar value and losing the provenance conditioning.

* **Plain ProvSQL function calls** (``expected(...)``, ``provenance()``,
  the ``sr_*`` semiring evaluators, ``probability(...)`` …) raise
  ``function … does not exist``.

All of these are loud, self-explanatory errors. The fix is always the
same: put ``provsql`` on the ``search_path`` (or qualify the name).

The ``setup_search_path()`` helper
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``CREATE EXTENSION provsql`` prints a ``NOTICE`` when the database's
default ``search_path`` does not include ``provsql``. The bundled
helper does the edit for you:

.. code-block:: postgresql

    SELECT provsql.setup_search_path();

It reads the database's current ``search_path`` setting, appends
``provsql`` if it is not already present (never reordering or dropping
the existing entries), and applies the result with ``ALTER DATABASE``.
It is idempotent and reports what it did with a ``NOTICE``. Only **new**
sessions pick up the change -- reconnect (or ``SET search_path`` in the
current session) to use unqualified names right away. The caller must be
the database owner or a superuser, and role-level ``search_path``
settings (if any) take precedence over the database-level one and are
left untouched.

ProvSQL never edits your ``search_path`` on its own: ``CREATE EXTENSION``
only advises, and ``setup_search_path()`` runs only when you call it.
