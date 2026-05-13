ProvSQL Studio
==============

ProvSQL Studio is a Python-backed web UI for the ProvSQL extension. It
runs as a separate package, connects to any PostgreSQL database with
ProvSQL installed, and lets you inspect provenance interactively
through two complementary inspection modes:

* **Circuit mode** renders the provenance directed acyclic graph
  (DAG) behind a result's UUID or aggregate token, with frontier
  expansion, a node inspector, and on-the-fly :doc:`semiring
  evaluation <semirings>`.
* **Where mode** highlights the source cells that contributed to each
  output value, against the live content of the provenance-tracked
  relations.

A schema panel, a configuration panel, and a mode-switcher round out
the UI; all three are described below.

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

For Studio developers, install from a local checkout in editable
mode, so edits to the source take effect without reinstalling:

.. code-block:: bash

    pip install -e ./studio

The Docker demonstration container ships a pre-installed Studio
alongside the extension; see :ref:`docker-container`.

.. _studio-connecting:

Connecting
----------

Launch Studio with a `data source name (DSN)
<https://www.postgresql.org/docs/current/libpq-connect.html#LIBPQ-CONNSTRING>`_:

.. code-block:: bash

    provsql-studio --dsn postgresql://user@localhost:5432/mydb

If ``--dsn`` is omitted, Studio resolves the connection target in
this order:

* The ``DATABASE_URL`` environment variable, used as a DSN.
* libpq's standard `environment variables
  <https://www.postgresql.org/docs/current/libpq-envars.html>`_
  (``PGDATABASE``, ``PGSERVICE``, ``PGHOST``…).
* Fallback to the ``postgres`` maintenance database on libpq's
  default host (a local Unix socket on Linux and macOS, ``localhost``
  on Windows). Studio then surfaces a banner inviting you to pick a
  real database from the in-page switcher.

The browser reaches the UI at ``http://127.0.0.1:8000/`` (override
the bind address and port with ``--host`` and ``--port``).

In-page connection editor
^^^^^^^^^^^^^^^^^^^^^^^^^

A plug icon next to the connection-status dot in the top navigation
opens a popover where you can paste a DSN. Studio
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
history first, so **Alt+↑** recovers it.

.. _studio-connecting-search-path:

Search path
^^^^^^^^^^^

Studio pins ``provsql`` at the end of the per-batch ``search_path``
automatically, so the case-study idiom
``SET search_path TO public, provsql;`` is unnecessary. The header
shows a lock chip for the pinned ``provsql``; the rest of
``search_path`` is editable under the *Session* group of the
Config panel (see `Configuration`_).

Studio does not enforce a read-only PostgreSQL role: the query box
accepts arbitrary SQL, including DDL (``CREATE TABLE``), DML
(``INSERT``, ``UPDATE``), and management helpers
(:sqlfunc:`add_provenance`, :sqlfunc:`set_prob`,
:sqlfunc:`create_provenance_mapping`).
Connect Studio with a role that has the privileges your workflow
expects.

.. _studio-query-box:

Query box
---------

The query box is a syntax-highlighted SQL textarea: PostgreSQL
keywords, strings, comments, and identifiers are coloured inline.

Submit the current batch with **Ctrl+Enter** (or **⌘+Enter** on
macOS) inside the textarea, or by clicking the :guilabel:`Send query` button
next to it.

Past queries are kept in a session history. **Alt+↑** and **Alt+↓**
step through them in place; the :guilabel:`History` button opens a list view
of recent queries to pick from.

The textarea accepts multiple semicolon-separated statements;
Studio splits and runs them in a single transaction. Only the
last statement's result is rendered in the result table;
preceding statements are useful for setup (``SET``,
``CREATE TABLE``, fixture ``INSERT``\ s…) before the interesting
``SELECT``.

The result table is capped at :guilabel:`Result rows` (default 1000 rows,
tunable in `Configuration`_). When more rows are available, the
result-table footer shows a ``(first 1000; more available)`` marker
so the truncation is explicit.

.. _studio-query-toggles:

Per-query toggles
^^^^^^^^^^^^^^^^^

Two checkboxes next to the query box flip the connection's
provenance behaviour for the next batch:

* ``where_provenance`` (``provsql.where_provenance``) enables
  where-provenance tracking at circuit-build time: when on, the
  planner adds ``project`` and ``eq`` gates so each output value
  records the source cell it was copied from (see
  :doc:`where-provenance`). Where mode locks this toggle on and
  shows a lock icon next to it (without those gates the
  hover-to-trace would have nothing to display); Circuit mode
  leaves the toggle free, useful when you want a circuit that
  already carries where-provenance.
* ``update_provenance`` (``provsql.update_provenance``) carries
  provenance through ``INSERT``, ``UPDATE``, and ``DELETE``
  statements so the rows they create or modify keep their lineage
  (see :doc:`data-modification`). The toggle is freely
  user-controllable in both modes.

Toggle states are sent to the server alongside each query.

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

Hovering a node lights up its subtree; clicking pins it and opens
the inspector panel. Drag a node to reposition it; the offset is
preserved across frontier expansions and reset on every fresh
circuit load. A :guilabel:`Reset node positions` toolbar button undoes all
drag-to-move offsets at once. Wheel-to-zoom is supported on the
canvas, and a :guilabel:`Fit to screen` button resets zoom and pan. A
:guilabel:`Fullscreen` toggle pins the canvas to the viewport (Esc exits).

.. _studio-circuit-example:

Worked example
^^^^^^^^^^^^^^

Ask for the distinct cities mentioned in ``personnel``:

.. code-block:: sql

    SELECT DISTINCT city FROM personnel;

The result table shows one row per city with a ``provsql`` UUID
column. Clicking a UUID cell renders the corresponding circuit: a
single ``⊕`` (``plus``) gate over the input gates of the underlying
``personnel`` rows.

.. _studio-circuit-frontier:

Frontier expansion
^^^^^^^^^^^^^^^^^^

Studio caps each circuit fetch at the value of
``--max-circuit-nodes`` (default 200, tunable in
`Configuration`_). Nodes whose children were not fetched in the
current request carry a small gold ``+`` badge: clicking it requests
another breadth-first search (BFS) layer rooted at the frontier node
and merges it into the current scene. The cap is per-fetch, not per-scene, so a circuit
grows interactively as you expand the frontiers that interest you.

.. _studio-circuit-inspector:

Inspector panel
^^^^^^^^^^^^^^^

Pinning a node opens the inspector. Each gate type (see
:ref:`circuit-gates` for the catalogue) renders a gate-specific
metadata block: function and result type for ``agg``, left and right
attribute for ``eq``, value for ``mulinput``, relation id and
column list for ``input`` and ``update``, and so on.
``input`` and ``update`` gates additionally show the stored
probability: clicking the value swaps it for a number input, Enter
sends a :sqlfunc:`set_prob` to the server, Esc or blur cancels.
Out-of-range values and ProvSQL errors land inline.

.. _studio-circuit-eval-strip:

Semiring evaluation strip
^^^^^^^^^^^^^^^^^^^^^^^^^

Below the canvas, the :doc:`semiring evaluation <semirings>` strip
targets the pinned node (or, when no node is pinned, the root). The
semiring select organises compiled entries into four sub-groups, with
custom and "Other" entries below:

* **Compiled semirings**

  * *Boolean*: ``boolexpr``, ``boolean``.
  * *Lineage*: ``formula``, ``how``, ``why``, ``which``. ``formula``
    pretty-prints the provenance circuit as a symbolic expression
    (Green-Karvounarakis-Tannen); ``how`` is the same algebra in
    canonical :math:`\mathbb{N}[X]` sum-of-products form, so two
    semantically-equal circuits collapse to identical strings :
    suitable for provenance-aware equivalence checks. ``why`` and
    ``which`` are the set-valued projections.
  * *Numeric*: ``counting``, ``tropical``, ``viterbi``,
    ``lukasiewicz``. Łukasiewicz is the continuous-valued fuzzy logic
    on numeric values in :math:`[0, 1]`; for the discrete fuzzy /
    trust shape on a user-defined enum lattice, see ``maxmin`` under
    *User-enum* below.
  * *Intervals*: ``interval-union``. One UI option backed by the
    :sqlfunc:`sr_temporal`, :sqlfunc:`sr_interval_num` and
    :sqlfunc:`sr_interval_int` kernels: the strip picks the right one
    from the selected mapping's multirange type
    (``tstzmultirange`` / ``nummultirange`` / ``int4multirange``);
    requires PostgreSQL 14+. See :doc:`temporal` for the
    interval-union algebra.
  * *User-enum*: ``minmax`` and ``maxmin``. One UI option per shape,
    polymorphic over any user-defined PostgreSQL enum carrier (the
    bottom and top of the lattice come from
    :literal:`pg_enum.enumsortorder`). ``minmax`` is the security shape
    (alternatives combine to enum-min, joins to enum-max); ``maxmin``
    is the discrete fuzzy / availability / trust shape (alternatives
    to enum-max, joins to enum-min). Backed by :sqlfunc:`sr_minmax`
    and :sqlfunc:`sr_maxmin` respectively.

* **Custom semirings**: any user-defined wrapper over
  :sqlfunc:`provenance_evaluate` discovered in the schema.
* **Other**: ``probability`` and ``PROV-XML export``. The
  probability method picker (see :doc:`probabilities`) groups exact
  methods (``(default)``, ``independent``, ``possible-worlds``,
  ``tree-decomposition``, ``compilation``) and approximate methods
  (``monte-carlo``, ``weightmc``). PROV-XML export uses
  :sqlfunc:`to_provxml`; see :doc:`export`.

The mapping picker filters on the selected semiring's expected value
type: only ``boolean``-typed mappings appear under ``boolean``, only
the numeric base types (``smallint`` / ``integer`` / ``bigint`` /
``numeric`` / ``real`` / ``double precision``) under the numeric
group, only multirange-typed mappings under ``interval-union``, and
only mappings whose ``value`` column is a user-defined enum
(``pg_type.typtype = 'e'``) under ``minmax`` / ``maxmin``.
Polymorphic entries (``boolexpr``, ``formula``, ``how``, ``why``,
``which``) accept any mapping. ``boolexpr`` and ``PROV-XML export`` accept the
mapping as *optional*: with one, leaves are labelled by the mapping's
``value`` column; without one, leaves carry their gate UUID
(``PROV-XML``) or a bare ``x<id>`` placeholder (``boolexpr``). Custom-
semiring entries filter to mappings whose value type matches the
wrapper's return type. Mismatches are surfaced before the round-trip
as ``(no compatible mappings : expected …)`` in the picker.

:guilabel:`Run` reports the result inline along with the runtime; Monte-Carlo
runs additionally show a ± confidence band (`Hoeffding bound
<https://en.wikipedia.org/wiki/Hoeffding%27s_inequality>`_, 95 %
probability).
:guilabel:`Clear` wipes the result so a verbose Why or Formula output does
not obscure the canvas; :guilabel:`Copy` writes the just-rendered payload
(with full precision for probability, regardless of the rounded
display) to the clipboard.

The probability cell click-toggles between rounded (per the panel's
:guilabel:`Probability decimals` setting) and full double-precision; copies
always carry the full-precision form.

.. _studio-circuit-oversized:

Oversized circuits
^^^^^^^^^^^^^^^^^^

When a top-level circuit fetch exceeds the cap, Studio shows a
structured banner instead of an error: ``This subgraph has 4,521
nodes; the cap is 200 (rendering at depth 4)`` followed by a
single-click :guilabel:`Render at depth 1, then expand interactively` button
when the depth-1 envelope itself fits under the cap. Wide-bound
circuits (for instance aggregations with high fan-in) leave the
button out rather than promising a render that will exceed the cap
again. The eval strip (above) still works against the unrendered
root: it operates on the token UUID, not the rendered DAG.

.. figure:: /_static/studio/circuit-413.png
   :alt: Oversize-circuit banner showing 'This subgraph has X
         nodes; the cap is Y' and a 'Render at depth 1, then expand
         interactively' button.

   The actionable oversize-circuit banner: when a fetch exceeds the
   cap, Studio surfaces a single button to start at depth 1 and
   expand on demand.

.. _studio-circuit-distribution-profile:

Distribution profile panel
^^^^^^^^^^^^^^^^^^^^^^^^^^

For nodes whose underlying gate is a scalar random-variable root
(``gate_rv``, ``gate_value`` in float8 mode, ``gate_arith``,
``gate_mixture``), the eval strip exposes a *Distribution profile*
entry under the *Distribution* group. Running it returns
header stats (mean :math:`\mu` and variance :math:`\sigma^2`),
an inline-SVG histogram of the sub-circuit's distribution, a
PDF/CDF toggle, per-bar tooltips with :math:`\sigma` markers, and
wheel-zoom on the value axis. The histogram is backed
server-side by :sqlfunc:`rv_histogram`; the sample count comes
from ``provsql.rv_mc_samples`` and the seed from
``provsql.monte_carlo_seed`` (both surfaced in the Config panel).

When the pinned node resolves to a recognised closed-form shape
the panel overlays the analytical PDF (or CDF, depending on the
toggle) on the histogram as a smooth terracotta curve, and
point masses as vertical stems capped by a small disc. The
overlay makes the simplifier's analytical wins visible: when
``2 * Exp(0.4)`` folds to ``Exp(0.2)`` the panel shows the
exact exponential decay curve over the MC-sampled bars, so the
user can verify by eye that the fold matched the distribution.

The recognised shapes are:

* a bare ``gate_rv`` of Normal / Uniform / Exponential / Erlang,
  optionally with a one-interval conditioning event -- smooth
  curve;
* a Dirac (``provsql.as_random(c)``) -- single stem at ``c``;
* a categorical (``provsql.categorical``) -- one stem per
  outcome, height proportional to its probability mass;
* a Bernoulli mixture (``provsql.mixture(p, X, Y)``) over any
  recursively-matched shape -- weighted sum of the per-arm
  curves, with Dirac / categorical arms contributing stems
  whose mass propagates through the Bernoulli weight.

Conditioning (a ``WHERE`` predicate, or any conditioning
provenance UUID) applies to every shape: bare-RV curves clip
to the conditioning interval and renormalise; mixture arms
truncate individually and the Bernoulli weight rebalances by
the ratio of arm masses; categorical outcomes outside the
interval are dropped and surviving masses renormalise to 1;
Diracs survive iff their value sits in the interval, and an
infeasible event raises a clean "conditioning event is
infeasible" error without running 100,000 wasted MC samples.

The curve is computed server-side by
:sqlfunc:`rv_analytical_curves`; shapes outside the closed-form
table (``gate_arith`` composites of independent RVs that the
simplifier cannot fold, non-integer Erlang shapes) render
histogram-only without an overlay.

For pure-discrete shapes (a Dirac, a categorical, or a nested
mixture whose every arm is one of those) the CDF mode draws a
true staircase: horizontal flats joined by vertical jumps at
each outcome, running from 0 at the chart's left edge to 1 on
the right.

.. figure:: /_static/studio/distribution-profile.png
   :alt: The eval-strip Distribution profile panel showing the
         support interval, the mean and standard deviation, an
         inline-SVG histogram of the sub-circuit's distribution,
         and the analytical Normal(28, 2) PDF curve overlaid on
         the MC bars.

   The *Distribution profile* eval-strip panel on a ``gate_rv``
   ``N(28, 2)`` leaf: header stats (support, :math:`\mu`,
   :math:`\sigma`), an inline-SVG histogram with the analytical
   PDF overlaid as a smooth terracotta curve, and a PDF/CDF
   toggle on the right.

The same group hosts a *Sample* entry that draws raw samples
via :sqlfunc:`rv_sample`; the result renders as a collapsible
panel with a six-value inline preview and a "show full list"
expander. When the conditioning event's acceptance
rate truncates the run below the requested ``n``, the panel
surfaces an actionable hint pointing at ``provsql.rv_mc_samples``.

The *Moment* entry on the same strip computes :sqlfunc:`moment`
or :sqlfunc:`central_moment` for a chosen ``k`` (raw vs central
toggle), and the *Support* entry returns the closed-form
:sqlfunc:`support` interval.

.. _studio-circuit-conditioning:

Conditioning and the row-prov auto-preset
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The eval strip carries a *Condition on* text input that takes any
provenance UUID, when populated, every distribution-shaped
evaluation (profile, sample, moment, support) routes through the
conditional path. Clicking a result-table cell auto-presets the
field to the row's provenance UUID, with a :guilabel:`Conditioned
by:` badge visible underneath the input. Clicking the active
badge clears the conditioning and reverts to the unconditional
answer; clicking the muted badge restores the row provenance.
Manual edits stick within a row and reset on row navigation.

Combined with the distribution profile, this makes side-by-side
comparison of unconditional vs conditional shape two clicks: pin
the random variable, run *Distribution profile* unconditional,
toggle the badge, run it conditional. The truncated closed-form
table (Normal via Mills ratio, Uniform on the intersected
support, Exponential by memorylessness) takes over when
applicable; otherwise the panel reflects the rejection-sampling
estimate at the configured budget.

.. _studio-circuit-simplify-on-load:

Simplified-circuit rendering
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Circuit mode honours the ``provsql.simplify_on_load`` GUC
(see :doc:`configuration` and :doc:`continuous-distributions`) and
renders the in-memory peephole-simplified graph via the
:sqlfunc:`simplified_circuit_subgraph` SRF. Toggling the GUC in
the Config panel switches between the raw, gate-creation view
(useful when debugging RV constructors or the comparison-rewriter
path) and the simplified, evaluation-time view (what every
downstream consumer sees). Comparators decidable from the
propagated support (a Normal restricted to ``x > 2`` reduces
trivially when shifted out of the support) collapse to Bernoulli
``gate_input`` leaves before the canvas renders, so the visible
graph matches what the semiring evaluators and Monte-Carlo
sampler actually consume.

.. _studio-where-mode:

Where mode
----------

Where mode is the visual counterpart to :sqlfunc:`where_provenance`
(see :doc:`where-provenance`). It enables ``provsql.where_provenance``
on the connection and wraps every ``SELECT`` so the result carries
the where-provenance of each output value. Hovering a result cell highlights the source cells (in
the sidebar) that contributed to it. No explicit
:sqlfunc:`where_provenance` call is required.

.. figure:: /_static/studio/where-mode.png
   :alt: Studio Where mode with a result row hovered, highlighting
         the contributing rows in the personnel sidebar.

   Where mode: source relations on the right; hovering a cell
   highlights the personnel rows that produced it.

The sidebar lists every provenance-tracked relation. Each panel
header reports its row count: ``personnel – 100 of ~50000 tuples``
makes the per-relation cap explicit when a relation is too large to
materialise in full (default 100 rows, tunable in
`Configuration`_). The :guilabel:`Input gates only` toggle (default on)
hides relations whose first ``provsql`` token is not an ``input``
gate, so derived materialisations do not crowd the panel.

Each result row gets a :guilabel:`→ Circuit` button that switches to Circuit
mode and pre-loads the provenance DAG of that row's token; see
`Mode-switching`_.

.. _studio-where-example:

Worked example
^^^^^^^^^^^^^^

Ask for the people listed in Paris:

.. code-block:: sql

    SELECT * FROM personnel WHERE city = 'Paris';

Each output cell becomes hover-aware: hovering ``name``,
``classification``, or ``city`` lights up the matching cells in the
``personnel`` panel of the sidebar.

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

* :sc:`prov` (purple) on a relation whose ``provsql`` column is
  injected by the planner: provenance tracking is active.
* :sc:`mapping` (gold) on a relation shaped
  ``(value <T>, provenance uuid)``, including views from
  :sqlfunc:`create_provenance_mapping_view`. The two pills are
  mutually exclusive: a mapping view that also carries a planner-
  injected ``provsql`` column is classified as :sc:`mapping` (the
  more specific category).

On a tracked table, each column is a click target that prefills
``SELECT create_provenance_mapping('<table>_<col>_mapping',
'<schema>.<table>', '<col>');`` into the query box, so a fresh
mapping is two clicks away.

On any provenance-eligible plain table, :guilabel:`+ prov` and
:guilabel:`− prov` action chips prefill ``SELECT add_provenance(...)``
/ ``SELECT remove_provenance(...)``. They are hidden on views
(regular and materialised) and on foreign tables, since the
underlying ``ALTER TABLE ADD COLUMN`` does not work on those
relation kinds, and on mappings.

.. _studio-configuration:

Configuration
-------------

The Config panel groups its options into three sections:

* **Provenance** mirrors the user-level Grand Unified Configuration
  (GUC) parameters documented in :doc:`configuration`:
  ``provsql.active``, ``provsql.verbose_level``, and
  ``provsql.tool_search_path``.
  ``provsql.where_provenance`` and ``provsql.update_provenance`` live
  next to the query box instead (see `Per-query toggles`_), since
  they are typically flipped per query rather than per session.
  Studio also forces ``provsql.aggtoken_text_as_uuid = on`` for the
  whole session: clickable ``agg_token`` cells in the result table
  need that.
* **Session** wraps the per-request session state: the
  ``statement_timeout`` applied to every batch, and the visible part
  of ``search_path`` (``provsql`` is always pinned at the end).
* **Display limits** holds the size knobs: :guilabel:`Max circuit depth`
  (the BFS depth cap on the initial fetch), :guilabel:`Nodes per fetch` (the
  per-fetch node cap), :guilabel:`Sidebar rows per relation` (per-relation
  row cap in the Where-mode sidebar), :guilabel:`Result rows` (row cap on
  the result table), and :guilabel:`Probability decimals` (number of
  decimals in the eval-strip's probability display).

.. figure:: /_static/studio/config-panel.png
   :alt: Studio Config panel with the three section headings
         (Provenance, Session, Display limits) visible.

   The Config panel: three section headings above the per-row
   options.

All values persist on disk in ``provsql-studio/config.json`` under
the platform's user-config directory, and survive a Studio restart:

* Linux: ``$XDG_CONFIG_HOME/provsql-studio/`` (defaults to
  ``~/.config/provsql-studio/``).
* macOS: ``~/Library/Application Support/provsql-studio/``.
* Windows: ``%APPDATA%\provsql-studio\`` (typically
  ``C:\Users\<user>\AppData\Roaming\provsql-studio\``).

Each option is also exposed on the CLI as a flag
(``--statement-timeout``, ``--max-sidebar-rows``, ``--max-result-rows``,
``--max-circuit-nodes``, ``--search-path``, ``--tool-search-path``);
the CLI wins on startup, the panel writes back to the JSON.

.. _studio-mode-switching:

Mode-switching
--------------

The mode tabs in the top nav switch between Where and Circuit. A
switch carries the current SQL forward via ``sessionStorage``; it
auto-replays only when the user just ran the query, so unrun drafts
and plain reloads never auto-execute (important for side-effecting
statements like :sqlfunc:`add_provenance`).

In Where mode, every result row gets a :guilabel:`→ Circuit` button that
switches to Circuit mode and pre-loads the circuit for that row's
provenance UUID, so a hover-and-trace exploration can cross over to
the DAG without retyping the query.

Limitations
-----------

* **Per-fetch circuit cap, not per-scene**: ``--max-circuit-nodes``
  bounds each fetch independently, so a scene assembled by repeated
  frontier expansions can exceed the cap. This is intentional: the
  cap exists to keep the browser responsive on initial render, not
  to prevent drilling deep where you want to.
* **Verbose semiring outputs are unbounded**: ``formula``, ``how``,
  ``why``, ``which``, and ``PROV-XML export`` can return multi-megabyte
  strings on large circuits. Compute time is bounded by
  ``statement_timeout``; output size is not. Use :guilabel:`Clear` after a
  bulky run, or evaluate against a pinned subnode to scope the
  output.

.. _studio-compatibility:

Compatibility
-------------

Studio's version stream is independent of the extension's. Studio
is released on PyPI as ``provsql-studio`` from ``studio-vX.Y.Z``
git tags; the extension is released on PGXN as ``provsql`` from
``vX.Y.Z`` git tags. Each Studio release lists a minimum required
extension version.

.. list-table::
   :header-rows: 1
   :widths: 25 25 50

   * - Studio
     - ProvSQL extension
     - Notes
   * - ``1.0.x``
     - ``≥ 1.4.0``
     - First public release. Requires :sqlfunc:`circuit_subgraph`,
       :sqlfunc:`resolve_input`, and the
       ``provsql.aggtoken_text_as_uuid`` GUC (used for clickable
       ``agg_token`` cells), all introduced in 1.4.0.
   * - ``1.1.x``
     - ``≥ 1.5.0``
     - Adds renderers for the continuous-distribution gate
       family (``gate_rv``, ``gate_arith``, ``gate_mixture``,
       float8 ``gate_value``), the *Distribution profile* /
       *Sample* / *Moment* / *Support* evaluators, the
       *Condition on* row-provenance auto-preset, and
       simplified-circuit rendering driven by
       ``provsql.simplify_on_load``. Backed by the
       :sqlfunc:`simplified_circuit_subgraph`, :sqlfunc:`rv_histogram`
       and :sqlfunc:`rv_sample` C entry points introduced in 1.5.0.
       See :doc:`continuous-distributions`.

When the installed extension predates this minimum, Studio's startup
check prints the mismatch and exits. Pass ``--ignore-version`` to
override the check, for instance when running against a development
branch.
