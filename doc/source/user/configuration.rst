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

``provsql.where_provenance`` (default: ``off``)
    Enable where-provenance tracking (see :doc:`where-provenance`).
    Adds ``project`` and ``eq`` gates to record the source cell of each
    output value. Disabled by default due to overhead.

``provsql.update_provenance`` (default: ``off``)
    Enable provenance tracking for ``INSERT``, ``UPDATE``, and ``DELETE``
    statements (see :doc:`data-modification`). Requires PostgreSQL ≥ 14.

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

``provsql.tool_search_path`` (default: empty)
    Colon-separated list of directories prepended to ``PATH`` when ProvSQL
    spawns external command-line tools: the d-DNNF compilers (``d4``,
    ``c2d``, ``minic2d``, ``dsharp``), the WeightMC weighted model counter
    (``weightmc``), and the GraphViz ASCII renderer (``graph-easy``). The
    server's ``PATH`` is searched as a fallback, so an entry here only needs
    to be set when a tool lives outside the server's default ``PATH`` (e.g.
    in ``$HOME/local/bin``, a Conda environment, ``/opt/...``). Example::

        SET provsql.tool_search_path = '/opt/d4:/home/postgres/bin';

All variables above have user-level scope: any user can change them for their
own session without superuser privileges.
