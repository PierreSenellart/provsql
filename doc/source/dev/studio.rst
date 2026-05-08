ProvSQL Studio
==============

ProvSQL Studio (``studio/``, distributed on PyPI as
``provsql-studio``) is a Flask + vanilla-JavaScript web UI that
points a browser at a ProvSQL-enabled PostgreSQL database. This
chapter documents the layout for contributors; the user-facing
behaviour is in :doc:`../user/studio`, and the release plumbing
is in :ref:`studio-releases`.

Studio's version stream is independent of the extension's. Each
Studio release lists a minimum extension version in the
:ref:`compatibility table <studio-compatibility>`; the runtime
check that enforces it lives in
``studio/provsql_studio/cli.py``
(``REQUIRED_PROVSQL_VERSION``).


Module Layout
-------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Path
     - What it contains
   * - ``provsql_studio/__init__.py``
     - Holds ``__version__``. ``setuptools.dynamic`` reads this
       string for the wheel's metadata: bump it before tagging.
   * - ``provsql_studio/__main__.py``
     - Lets ``python -m provsql_studio`` reach ``cli.main``.
   * - ``provsql_studio/cli.py``
     - argparse front end + extension-version preflight check.
       Resolves the connection target (``--dsn`` ▸ ``DATABASE_URL``
       ▸ libpq ``PG*`` ▸ ``dbname=postgres`` fallback). The CLI
       flags mirror the persisted Config-panel options: the CLI
       wins on startup, the panel writes back to the on-disk JSON.
   * - ``provsql_studio/app.py``
     - Flask app factory + all HTTP routes. Owns the
       :class:`~psycopg_pool.ConnectionPool` that every request
       borrows from. The factory eagerly applies persisted
       options (search_path, statement_timeout, panel GUCs) so
       that an in-page settings change survives a server restart.
   * - ``provsql_studio/db.py``
     - Everything that touches PostgreSQL: pool construction,
       per-batch GUC application, ``exec_batch`` (the
       statement-timeout-bounded multi-statement runner that backs
       ``/api/exec``), schema / relation / mapping / custom-semiring
       discovery, the compiled-semiring evaluator dispatch, and the
       on-disk Config-panel persistence.
   * - ``provsql_studio/circuit.py``
     - Circuit fetch + DOT layout pipeline behind
       ``/api/circuit/<token>`` and its ``/expand`` companion. Uses
       :sqlfunc:`circuit_subgraph` server-side, then shells out to
       GraphViz ``dot -Tjson`` for the layout coordinates.
   * - ``provsql_studio/static/index.html``
     - The single page; ``app.js`` wires it up at load time.
   * - ``provsql_studio/static/app.js``
     - Common shell + Where-mode logic: nav, schema panel, config
       panel, query box, history, result rendering, hover-to-trace.
   * - ``provsql_studio/static/circuit.js``
     - Circuit-mode logic: DAG layout, frontier expansion, node
       inspector, eval-strip dispatching.
   * - ``provsql_studio/static/app.css``
     - BEM-style stylesheet split into two disjoint prefix
       namespaces. ``wp-`` (originally "where_panel", inherited
       from the legacy PHP UI Studio's shell was bootstrapped
       from) covers the shared chrome plus Where-mode classes:
       ``wp-nav``, ``wp-card``, ``wp-form``, ``wp-editor``,
       ``wp-result``, ``wp-toggle``, ``wp-history``, ``wp-btn``,
       ``wp-config``, ``wp-schema``. ``cv-`` ("circuit view")
       covers Circuit-mode classes: ``cv-canvas``, ``cv-eval``,
       ``cv-inspector``, ``cv-toolbar``, plus gate / node / edge
       classes.
   * - ``provsql_studio/static/colors_and_type.css``
     - Design tokens (palette, typography). Edit this rather than
       inlining colors into ``app.css``.
   * - ``provsql_studio/static/fonts/``
     - Self-hosted webfonts (EB Garamond, Fira Code, Jost), with
       OFL license files alongside.
   * - ``tests/``
     - Pytest unit suite (one file per Flask blueprint area:
       ``test_circuit.py``, ``test_evaluate.py``, ``test_exec.py``,
       …) plus ``tests/e2e/`` Playwright smoke scenarios.
   * - ``scripts/``
     - Developer-facing demo loaders (``load_demo_temporal.sql``,
       ``big_demo_queries.sql``, …). Not shipped in the wheel.
   * - ``CHANGELOG.md``
     - Per-release notes consumed by ``studio-release.yml``: see
       :ref:`studio-releases`.


HTTP API
--------

The browser only talks to Studio itself, never directly to
PostgreSQL. All data lookups go through the routes below; the
front end is otherwise self-contained (no third-party CDN at
runtime, fonts are bundled).

.. list-table::
   :header-rows: 1
   :widths: 25 10 65

   * - Path
     - Method
     - Purpose
   * - ``/``, ``/where``, ``/circuit``
     - GET
     - Static shell. Mode is URL-driven; the body class is set
       server-side (``mode-where`` / ``mode-circuit``) so the
       initial render does not flicker.
   * - ``/static/<path>``
     - GET
     - Static asset passthrough.
   * - ``/api/conn``
     - GET / POST
     - GET returns the active endpoint summary (user, host, db,
       PG server version) used by the connection chip and the
       polling status dot. POST swaps pools after probing the
       new DSN with ``SELECT 1``; the existing pool stays up if
       the probe fails.
   * - ``/api/databases``
     - GET
     - Lists databases the current role can access; powers the
       in-nav database switcher.
   * - ``/api/relations``
     - GET
     - Per-relation row dumps for the Where-mode sidebar
       (capped at ``--max-sidebar-rows``). Skips relations whose
       first ``provsql`` token is not an ``input`` gate when the
       "Input gates only" toggle is on.
   * - ``/api/schema``
     - GET
     - Schema-panel data: every selectable relation with its
       PROV / MAPPING classification, columns, and click-target
       hints (``can_add_provenance``, ``can_create_mapping``).
   * - ``/api/exec``
     - POST
     - Statement-timeout-bounded multi-statement runner. Splits
       on ``;``, runs each statement in a single transaction,
       wraps the last ``SELECT`` with ``provsql.where_provenance``
       in Where mode, and returns a ``StatementResult`` with
       columns, rows, runtime, and any wrap-fallback notice.
   * - ``/api/cancel/<request_id>``
     - POST
     - Sends ``pg_cancel_backend`` to the backend running
       ``request_id``. The originating ``/api/exec`` call returns
       a ``57014`` error which the renderer surfaces inline.
   * - ``/api/circuit/<token>``
     - GET
     - Returns the BFS-bounded subgraph rooted at ``token``,
       plus a layout from ``dot -Tjson``. Returns
       :exc:`~provsql_studio.circuit.CircuitTooLarge` (encoded as
       a structured 413 banner) when the cap would be exceeded.
   * - ``/api/circuit/<token>/expand``
     - POST
     - Same as above but rooted at a frontier node, used by the
       gold-``+`` badges in the canvas.
   * - ``/api/leaf/<token>``
     - GET
     - Resolves a leaf gate to its (relation, primary-key, value)
       triple via :sqlfunc:`resolve_input`; powers the inspector's
       leaf metadata block.
   * - ``/api/set_prob``
     - POST
     - Click-to-edit probability on ``input`` / ``update`` gates.
       Calls :sqlfunc:`set_prob`; out-of-range values land as
       inline errors.
   * - ``/api/provenance_mappings``
     - GET
     - Mapping picker contents for the eval strip.
   * - ``/api/custom_semirings``
     - GET
     - User-defined semiring wrappers discovered in the schema.
       Drives the "Custom Semirings" optgroup in the eval strip.
   * - ``/api/evaluate``
     - POST
     - Compiled-semiring evaluation against a pinned token. The
       handler in ``db.evaluate_circuit`` resolves the right
       compiled-semiring helper (the ``sr_*`` family in
       :doc:`/user/semirings`, or :sqlfunc:`probability_evaluate`, or
       :sqlfunc:`to_provxml`) for the (semiring, mapping) pair
       and returns the raw value plus an optional confidence
       bound for Monte-Carlo runs.
   * - ``/api/config``
     - GET / POST
     - Read / write the persisted Config-panel state (panel
       options + GUC overrides). POST validates each field via
       :func:`provsql_studio.db.validate_panel_option` before
       persisting; rejection comes back inline.

The two write endpoints that mutate the database
(``/api/set_prob``, ``/api/exec``) trust the connecting role for
authorization: Studio does not enforce a read-only PostgreSQL
role itself. Connect with the privileges your workflow expects.


Frontend
--------

The single ``index.html`` is rendered identically for both modes;
``app.js`` reads ``document.body.classList`` to discover the active
mode and lazy-loads ``circuit.js`` only when the body carries
``mode-circuit``.

A small ``window.__provsqlStudio`` shared-state object exposes the
current mode and a few utilities (``escapeHtml`` / ``escapeAttr`` /
``formatCell``) so ``circuit.js`` and the inline ``runQuery`` form
handler can talk to ``app.js`` without globals.

Mode switching uses anchor-tag navigations (``<a href="/circuit">``)
rather than ``history.pushState``: each mode is its own
server-rendered page so the JS bootstraps from a clean slate. The
*query* carries forward via ``sessionStorage``; auto-replay only
fires when the SQL was actually run (an unrun draft survives the
switch but does not auto-execute, which matters for
side-effecting statements like :sqlfunc:`add_provenance`).


Per-Batch GUC Application
-------------------------

Every ``/api/exec`` request runs in a single transaction. Before
the user's SQL, ``exec_batch`` issues a ``SET LOCAL`` for each
GUC the panel and per-query toggles imply:

- ``provsql.active`` (panel)
- ``provsql.verbose_level`` (panel, 0..100)
- ``provsql.tool_search_path`` (panel)
- ``provsql.where_provenance`` (per-query toggle; locked on in
  Where mode)
- ``provsql.update_provenance`` (per-query toggle, free in both
  modes)
- ``provsql.aggtoken_text_as_uuid`` (always ``on``: clickable
  ``agg_token`` cells need the underlying UUID exposed in text
  representation)
- ``statement_timeout`` (panel, in milliseconds)
- ``search_path``, with ``provsql`` always pinned at the end (see
  :func:`provsql_studio.db.compose_search_path`)

``SET LOCAL`` scopes the change to the transaction so a parallel
request on the same connection cannot see the override.


Config Persistence
------------------

Panel options and GUC overrides persist as a JSON file under the
platform's user-config directory (``platformdirs.user_config_path``):
Linux ``$XDG_CONFIG_HOME/provsql-studio/config.json``, macOS
``~/Library/Application Support/provsql-studio/``, Windows
``%APPDATA%\provsql-studio\``. The path is overridable via
``$PROVSQL_STUDIO_CONFIG_DIR`` (the unit and e2e tests use this
to keep their state out of the developer's real config).

Validation lives in ``db.validate_panel_option`` and is called
both from ``POST /api/config`` (rejection comes back as an inline
error) and from the app factory (rejection at startup logs and
falls back to defaults). The helper rejects forbidden characters
(``;``, ``--``, ``/*``, ``*/``) up front to keep an obvious
SQL-injection vector out of the per-batch ``SET`` line.


Circuit Fetch and Frontier Expansion
------------------------------------

A circuit fetch issues :sqlfunc:`circuit_subgraph` server-side,
which performs a BFS rooted at a token and returns at most
``--max-circuit-nodes`` rows. ``circuit.py`` then builds a DOT
source from the rows and runs ``dot -Tjson`` to obtain layout
coordinates; the front end consumes the JSON directly.

Nodes whose children were not fetched in the current request
(their gate has more children than the BFS layer admitted) are
flagged as *frontier* nodes and rendered with a gold ``+``
badge. Clicking the badge calls ``/api/circuit/<token>/expand``
rooted at the frontier node, the response gets merged into the
existing scene, and ``app.js`` repaints. The cap is per-fetch,
not per-scene, so a circuit grows interactively as you expand
the frontiers that interest you.


Test Harness
------------

``studio/tests/conftest.py`` provides a session-scoped
``test_dsn`` fixture that creates a unique database, installs
the extension, and runs ``test/sql/setup.sql`` plus
``test/sql/add_provenance.sql`` (the same fixture the extension
regression suite uses). The teardown drops the database.

Override with the ``$PROVSQL_STUDIO_TEST_DSN`` environment
variable to reuse an existing database (CI uses this). In that
case the harness only checks that the extension is installed and
the ``personnel`` table is present, leaving setup to the caller.

The unit suite exercises the Flask layer through
``app.test_client()`` (no live HTTP). The Playwright e2e suite
under ``tests/e2e/`` spawns the Studio CLI in a subprocess
against the same ``test_dsn`` (with ``PROVSQL_STUDIO_CONFIG_DIR``
pointed at a tempdir so a developer's persisted UI settings can
not override the ``--search-path provsql_test`` we pass at
startup) and drives the live UI through a ``chromium`` instance.

Both suites live under the same ``pytest`` invocation: ``make
studio-test`` from the repo root runs ``ruff check`` first, then
``pytest tests`` (which walks both directories). For
finer-grained runs, ``pytest tests --ignore=tests/e2e`` skips the
chromium tax.
