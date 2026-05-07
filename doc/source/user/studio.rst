ProvSQL Studio
==============

ProvSQL Studio is a Python-backed web UI for the ProvSQL extension. It
runs as a separate package, connects to any PostgreSQL database with
ProvSQL installed, and lets you inspect provenance interactively
through two complementary modes:

* **Circuit mode** renders the provenance DAG behind a result's UUID
  or aggregate token, with frontier expansion, a node inspector, and
  on-the-fly semiring evaluation.
* **Where mode** highlights the source cells that contributed to each
  output value, against the live content of the provenance-tracked
  relations.

Studio is a query-and-inspect tool, not a database loader. Bulk
fixture loads (``psql -d mydb -f setup.sql``) still go through
``psql``; Studio takes over once the data is in place.

.. _studio-installation:

Installation
------------

Studio targets Python ≥ 3.10. The released package is on PyPI:

.. code-block:: bash

    pip install provsql-studio

Studio assumes the ProvSQL extension is already installed in the
target database (see :doc:`getting-provsql`) and is running at least
the version listed under `Compatibility`_.

For contributors hacking on Studio, install in editable mode from a
checkout of the repository:

.. code-block:: bash

    pip install -e ./studio

The Docker demonstration container ships a pre-installed Studio
alongside the extension; see the *Docker Container* section of
:doc:`getting-provsql`.

.. _studio-connecting:

Connecting
----------

Launch Studio with a DSN:

.. code-block:: bash

    provsql-studio --dsn postgresql://pierre@localhost:5432/mydb

If ``--dsn`` is omitted, libpq's standard environment variables
(``PGDATABASE``, ``PGSERVICE``, ``DATABASE_URL``, ...) are honoured.
When neither is set, Studio connects to the ``postgres`` maintenance
database and surfaces a banner inviting you to pick a real database
from the in-page switcher.

The browser reaches the UI at ``http://127.0.0.1:8000/`` (override
the bind address and port with ``--host`` and ``--port``).

In-page connection editor
^^^^^^^^^^^^^^^^^^^^^^^^^

A plug icon next to the connection-status dot in the top navigation
opens a popover where you can paste a libpq connection string. Studio
probes the new DSN with ``SELECT 1`` before swapping pools, so a typo
or wrong password leaves the existing connection up and surfaces the
PostgreSQL error inline. The status dot polls every 5 seconds; its
tooltip shows the active ``user@host:port`` endpoint.

Database switcher
^^^^^^^^^^^^^^^^^

Clicking the database name in the top nav lists every accessible
database on the current server; pick one and the page reloads onto
it. The query box is wiped on switch (the previous query rarely
makes sense against a different database) but is pushed onto the
history first, so Alt+↑ recovers it.

.. _studio-connecting-search-path:

Search path
^^^^^^^^^^^

Studio pins ``provsql`` at the end of the per-batch ``search_path``
automatically, so the case-study idiom
``SET search_path TO public, provsql;`` is unnecessary. The header
shows a lock chip when the pin is in effect; the user-visible part
of ``search_path`` is editable under the *Session* group of the
Config panel (see `Configuration`_).

Studio does not enforce a read-only PostgreSQL role: the query box
accepts arbitrary SQL, including DDL (``add_provenance``,
``CREATE TABLE``), DML (``INSERT``, ``UPDATE``), and management
helpers (:sqlfunc:`set_prob`, :sqlfunc:`create_provenance_mapping`).
Connect Studio with a role that has the privileges your workflow
expects.

.. _studio-circuit-mode:

Circuit mode
------------

Circuit mode is the visual counterpart to :sqlfunc:`view_circuit`
and the programmatic walks via :sqlfunc:`get_gate_type`,
:sqlfunc:`get_children`, and :sqlfunc:`identify_token`. The query
runs unwrapped, so the ``provsql`` UUID column and any ``agg_token``
cells appear in the result table as raw values; clicking one renders
its provenance DAG in the sidebar.

.. figure:: /_static/studio/circuit-mode.png
   :alt: Studio Circuit mode showing a small DISTINCT-circuit with the
         input-gate inspector open and the stored probability
         visible.

   Circuit mode: a ``DISTINCT`` query renders a ``⊕``-rooted DAG.
   Pinning an input gate opens the inspector with its metadata and
   the stored probability, click-to-edit.

Worked example: ask for the distinct cities mentioned in
``personnel``,

.. code-block:: sql

    SELECT DISTINCT city FROM personnel;

The result table shows one row per city with a ``provsql`` UUID
column. Clicking a UUID cell renders the corresponding circuit:
typically a single ``⊕`` (``plus``) gate over the input gates of the
underlying ``personnel`` rows.

Hovering a node lights up its subtree; clicking pins it and opens
the inspector panel. Drag a node to reposition it; the offset is
preserved across frontier expansions and reset on every fresh
circuit load. A ``Reset layout`` toolbar button wipes the offsets
in one click. Wheel-to-zoom is supported on the canvas, and a
``Fullscreen`` toggle pins the canvas to the viewport (Esc exits).

.. _studio-circuit-frontier:

Frontier expansion
^^^^^^^^^^^^^^^^^^

Studio caps each circuit fetch at the value of
``--max-circuit-nodes`` (default 200, tunable in
`Configuration`_). Nodes whose children were not fetched in the
current request carry a small gold ``+`` badge: clicking it requests
another BFS layer rooted at the frontier node and merges it into the
current scene. The cap is per-fetch, not per-scene, so a circuit
grows interactively as you expand the frontiers that interest you.

.. _studio-circuit-oversized:

Oversized circuits
^^^^^^^^^^^^^^^^^^

When a top-level circuit fetch exceeds the cap, Studio shows a
structured banner instead of an error: ``Circuit has 4521 nodes;
cap is 200 (rendering at depth 4)`` followed by a single-click
``Render at depth 1, then expand interactively`` button when the
depth-1 envelope itself fits under the cap. Wide-bound circuits
(for instance aggregations with high fan-in) leave the button out
rather than promising a render that will 413 again. The eval strip
(below) still works against the unrendered root: it operates on
the token UUID, not the rendered DAG.

.. figure:: /_static/studio/circuit-413.png
   :alt: 413 banner showing 'Circuit has X nodes; cap is Y' and a
         'Render at depth 1, then expand interactively' button.

   The actionable 413 banner: oversized circuits surface a single
   button to start at depth 1 and expand on demand.

.. _studio-circuit-inspector:

Inspector panel
^^^^^^^^^^^^^^^

Pinning a node opens the inspector. Each gate type renders a
gate-specific metadata block: function and result type for ``agg``,
left and right attribute for ``eq``, value for ``mulinput``,
relation id and column list for ``input`` and ``update``, and so on.
``input`` and ``update`` gates additionally show the stored
probability: clicking the value swaps it for a number input, Enter
sends a :sqlfunc:`set_prob` to the server, Esc or blur cancels.
Out-of-range values and ProvSQL errors land inline.

.. _studio-circuit-eval-strip:

Semiring evaluation strip
^^^^^^^^^^^^^^^^^^^^^^^^^

Below the canvas, the eval strip targets the pinned node (or, when
no node is pinned, the root). The semiring select groups available
evaluations into three optgroups:

* **Compiled semirings**: ``boolean``, ``boolexpr``, ``counting``,
  ``why``, ``which``, ``formula``, ``tropical``, ``viterbi``,
  ``temporal`` (PostgreSQL 14+).
* **Custom semirings**: any user-defined wrapper over
  :sqlfunc:`provenance_evaluate` discovered in the schema. Mappings
  are filtered by value-type compatibility for custom entries.
* **Other**: ``probability`` (with a method picker for
  ``compilation``, ``monte-carlo``, ``weightmc``) and
  ``PROV-XML export`` (using :sqlfunc:`to_provxml`; see
  :doc:`export`).

Run reports the result inline along with the runtime; Monte-Carlo
runs additionally show the Hoeffding 95 % absolute-error bound.
``Clear`` wipes the result so a verbose Why or Formula output does
not obscure the canvas; ``Copy`` writes the just-rendered payload
(with full precision for probability, regardless of the rounded
display) to the clipboard.

The probability cell click-toggles between rounded (per the panel's
``Probability decimals`` setting) and full double-precision; copies
always carry the full-precision form.

.. _studio-where-mode:

Where mode
----------

Where mode is the visual counterpart to :sqlfunc:`where_provenance`.
It enables ``provsql.where_provenance`` on the connection and wraps
every ``SELECT`` so the result carries the where-provenance of each
output value. Hovering a result cell highlights the source cells (in
the sidebar) that contributed to it. No explicit
:sqlfunc:`where_provenance` call is required.

.. figure:: /_static/studio/where-mode.png
   :alt: Studio Where mode with a result row hovered, highlighting
         the contributing rows in the personnel sidebar.

   Where mode: source relations on the right; hovering a cell
   highlights the personnel rows that produced it.

Worked example: ask for the people listed in Paris,

.. code-block:: sql

    SELECT * FROM personnel WHERE city = 'Paris';

Each output cell becomes hover-aware: hovering ``name``,
``classification``, or ``city`` lights up the matching cells in the
``personnel`` panel of the sidebar.

The sidebar lists every provenance-tracked relation. Each panel
header reports its row count: ``personnel – 100 of ~50000 tuples``
makes the per-relation cap explicit when a relation is too large to
materialise in full (default 100 rows, tunable in
`Configuration`_). The ``Input gates only`` toggle (default on)
hides relations whose first ``provsql`` token is not an ``input``
gate, so derived materialisations do not crowd the panel.

Each result row gets a ``→ Circuit`` button that switches to Circuit
mode and pre-loads the provenance DAG of that row's token; see
`Mode-switching`_.

.. _studio-where-wrap-fallback:

Wrap-fallback notice
^^^^^^^^^^^^^^^^^^^^

If a Where-mode query touches no provenance-tracked relation, Studio
silently drops the wrap and surfaces an INFO banner instead of
raising. This is the right default for case-study scripts that do
bulk setup before the first interesting ``SELECT``.

.. _studio-schema-panel:

Schema panel
------------

A button in the top nav opens a searchable popover listing every
``SELECT``-able relation. Each gets one of two pills:

* ``prov`` (purple) on a relation whose ``provsql`` column is
  injected by the planner: provenance tracking is active.
* ``mapping`` (gold) on a relation shaped
  ``(value <T>, provenance uuid)``, including views from
  :sqlfunc:`create_provenance_mapping_view`. The two pills are
  mutually exclusive: a mapping view that also carries a planner-
  injected ``provsql`` column is classified as ``mapping`` (the
  more specific category).

On a tracked table, each column is a click target that prefills
``SELECT create_provenance_mapping('<table>_<col>_mapping',
'<schema>.<table>', '<col>');`` into the query box, so a fresh
mapping is two clicks away.

On any provenance-eligible plain table, ``+ prov`` and ``− prov``
action chips prefill ``SELECT add_provenance(...)`` /
``SELECT remove_provenance(...)``. They are hidden on views,
materialised views, and foreign tables (the underlying
``ALTER TABLE`` rejects them) and on mappings.

.. _studio-configuration:

Configuration
-------------

The Config panel groups its options into three sections:

* **Provenance** mirrors the user-level GUCs documented in
  :doc:`configuration`: ``provsql.active``, ``provsql.verbose_level``.
  ``provsql.where_provenance`` is managed by the active mode and is
  not exposed here.
* **Session** wraps the per-request session state: the
  ``statement_timeout`` applied to every batch, and the visible part
  of ``search_path`` (``provsql`` is always pinned at the end).
* **Display limits** holds the size knobs: ``Sidebar rows per
  relation``, ``Result rows``, ``Nodes per fetch`` (the per-fetch
  circuit cap), and ``Probability decimals`` (number of decimals in
  the eval-strip's probability display).

.. figure:: /_static/studio/config-panel.png
   :alt: Studio Config panel with the three section headings
         (Provenance, Session, Display limits) visible.

   The Config panel: three section headings above the per-row
   options.

All values persist on disk at ``~/.config/provsql-studio/config.json``
and survive a Studio restart. Each row is also exposed on the CLI as
a flag (``--statement-timeout``, ``--max-sidebar-rows``,
``--max-result-rows``, ``--max-circuit-nodes``, ``--search-path``);
the CLI wins on startup, the panel writes back to the JSON.

.. _studio-mode-switching:

Mode-switching
--------------

The mode tabs in the top nav switch between Where and Circuit. A
switch carries the current SQL forward via ``sessionStorage``; it
auto-replays only when the new mode's wrapping differs and the user
just ran the query, so unrun drafts and plain reloads never
auto-execute (important for side-effecting statements like
:sqlfunc:`add_provenance`). The DB switcher follows the same rule.

In Where mode, every result row gets a ``→ Circuit`` button that
switches to Circuit mode and pre-loads the circuit for that row's
provenance UUID, so a hover-and-trace exploration can cross over to
the DAG without retyping the query.

Limitations
-----------

* **No SQL-file loader**: ``psql -d mydb -f setup.sql`` is still the
  way to apply bulk fixture scripts. Studio is a query-and-inspect
  tool; the query box is for ad-hoc statements, not for sourcing a
  hundred-line setup script.
* **Per-fetch circuit cap, not per-scene**: ``--max-circuit-nodes``
  bounds each fetch independently, so a scene assembled by repeated
  frontier expansions can exceed the cap. This is intentional: the
  cap exists to keep the browser responsive on initial render, not
  to prevent drilling deep where you want to.
* **Verbose semiring outputs are unbounded**: ``formula``, ``why``,
  ``which``, and ``PROV-XML export`` can return multi-megabyte
  strings on large circuits. Compute time is bounded by
  ``statement_timeout``; output size is not. Use ``Clear`` after a
  bulky run, or evaluate against a pinned subnode to scope the
  output.

.. _studio-compatibility:

Compatibility
-------------

Studio versions track ProvSQL extension versions independently:
Studio publishes ``studio-vX.Y.Z`` tags on PyPI, the extension
publishes ``vX.Y.Z`` tags on PGXN. Each Studio release lists a
minimum required extension version.

.. list-table::
   :header-rows: 1
   :widths: 25 25 50

   * - Studio
     - ProvSQL extension
     - Notes
   * - ``0.1.x``
     - ``≥ 1.4.0``
     - First public release. Requires
       :sqlfunc:`circuit_subgraph` and :sqlfunc:`resolve_input` (added
       in 1.4.0) and the ``provsql.aggtoken_text_as_uuid`` GUC for
       clickable ``agg_token`` cells.

When the installed extension predates this minimum, Studio's startup
check prints the mismatch and exits. Pass ``--ignore-version`` to
override the check, for instance when running against a development
branch.
