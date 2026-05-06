Configuration Reference
========================

ProvSQL is controlled by four `GUC (Grand Unified Configuration)
<https://www.postgresql.org/docs/current/config-setting.html>`_ variables,
all settable per session with ``SET`` or permanently in
`postgresql.conf <https://www.postgresql.org/docs/current/config-setting.html>`_
or with `ALTER DATABASE <https://www.postgresql.org/docs/current/sql-alterdatabase.html>`_
/ `ALTER ROLE <https://www.postgresql.org/docs/current/sql-alterrole.html>`_.

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
    Controls how an :sql:`agg_token` cell renders as text. By default the
    output function returns the human-friendly ``"value (*)"`` form, where
    *value* is the running aggregate state. When set to ``on``, it returns
    the underlying provenance UUID instead. UI layers (notably ProvSQL
    Studio) flip this on per session so aggregate cells expose the circuit
    root UUID for click-through; the user-facing display string is recovered
    via :sqlfunc:`agg_token_value_text` for any such UUID. Has no effect on
    ``EXPLAIN`` output, on the underlying storage, or on numeric / casting
    behaviour of :sql:`agg_token`.

All five variables have user-level scope: any user can change them for their
own session without superuser privileges.
