# Studio: open features and integration work

Backlog for Studio work (new inspection modes, larger features,
notebook polish, implementation observations). The user guide at
`doc/source/user/studio.rst` documents the compatibility matrix,
version streams, and CLI flags.

## Out of scope

- Tutorial / case-study coverage gaps: covered by `case-studies.md`.

## Plan

### New inspection modes

Two additional modes share the existing chrome (query box,
result-table rendering, mode switcher) and add their own sidebar
plus per-cell click affordances.

#### Contributions mode

A heat-map of per-input Shapley / Banzhaf contributions toward one
chosen result tuple. The mode is best understood as Circuit mode with
the DAG canvas replaced by a ranked contribution chart: it reuses the
same chrome (query box, result-table render, mode switcher) and the
same *pin a result row, inspect its provenance token* interaction, so
the bulk of the wiring already exists. Closes the CS2 §13–15 gap by
turning the `shapley` / `banzhaf` / `*_all_vars` SQL steps into a
point-and-click surface.

**Target selection.** Contributions are always *relative to a result
tuple*, i.e. one provenance token. The mode reuses Circuit mode's
in-mode pin, *not* a per-row button. Circuit mode has no button: it
renders the `provsql` UUID cells as clickable (`data-circuit-uuid`,
`is-clickable`, gated on `clickableUuid = isCircuit || notebook` at
`app.js:2928`) and a cell click calls `loadCircuit` (`app.js:2196`).
Contributions mode does the same: add `contributions` to that
`clickableUuid` set so clicking a result row's token pins it as the
target and triggers a contribution fetch. A short header above the
chart shows the pinned token (abbreviated) with an unpin affordance,
exactly like the circuit canvas root pill.

The per-row **button** is a separate, cross-mode affordance. Today the
`→ Circuit` button (`data-jump-circuit`) lives only in *Where* mode
(`app.js:3085`, gated `isWhere`): it carries a UUID via
`sessionStorage` (`ps.preloadCircuit`) and navigates to `/circuit`. By
the same pattern, Where mode can grow a parallel `→ Contributions`
button that carries the token to `/contributions`. So the button is a
Where-mode → Contributions jump, while in-mode target picking is a
cell click, matching how Circuit mode already splits the two.

**The chart / sidebar.** For the pinned token the sidebar lists every
*input* variable with:

- a mapping-resolved **label** (see *Label resolution* below),
- the signed **contribution value**, and
- a horizontal **bar** scaled to `max |value|` across the set, sorted
  by value descending.

Shapley / Banzhaf on a monotone circuit are non-negative, but `monus`
/ `!` / conditioning circuits can yield signed values, so the bar
diverges from a zero baseline (positive right, negative left) rather
than assuming `[0, max]`. The set is capped (config-driven, like the
circuit-node cap) with a `top N of M` note when truncated, since a
wide input relation can mint thousands of variables.

**Measure and method controls.** A small control row above the chart,
modelled on the Circuit-mode eval strip:

- **Measure** toggle: Shapley vs Banzhaf. These are the `banzhaf`
  boolean flag of the *same* `shapley_all_vars` C function
  (`sql/provsql--1.0.0.sql:1342`, `banzhaf_all_vars` is just
  `shapley_all_vars(…, 't')`), so one endpoint covers both.
- **Method / arguments**: the `method` + `arguments` parameters that
  `shapley_all_vars` already accepts, surfaced with the same
  exact-vs-sampling affordances the eval strip uses for
  `probability_evaluate` (e.g. a Monte-Carlo budget). When an
  approximate method is selected the chart carries the same
  "approximate" annotation the probability path already renders, so a
  sampled contribution is visually distinguished from an exact one.
- **Mapping** picker: the optional provenance-mapping dropdown,
  populated from the existing `/api/provenance_mappings`, that chooses
  how inputs are labelled. Defaults to *none*.

**Label resolution.** Three tiers, server-side, so the sidebar never
shows a bare UUID when something better exists:

1. If a mapping is picked, `LEFT JOIN` it on `provenance = variable`
   (the CS2 §15 idiom: `JOIN study_mapping sm ON sm.provenance =
   sav.variable`) and use its `value` column.
2. Otherwise resolve each variable through `provsql.resolve_input`
   (the `/api/leaf` path, `circuit.py:resolve_input`) to its source
   row(s), and label with the source relation + row, reusing the
   leaf-chip renderer Circuit mode already has.
3. Fall back to the short/full abbreviated UUID pair used everywhere
   else.

The per-input source row and probability that `/api/leaf` returns also
feed a hover tooltip, so an input's contribution sits next to *what it
is* and *how probable it is* without a second click.

**Folding the per-node variants.** The eval-strip `shapley` /
`banzhaf` single-variable numbers (pin a node, pick a variable token)
do not need a home on the circuit canvas: this mode subsumes them. The
pinned target *is* the node; the sidebar *is* the all-variables
enumeration; the single-variable figure is just one row of that list.
So the plan is to drop the per-node Shapley/Banzhaf eval-strip options
(if/when added there) in favour of this mode rather than maintaining
two surfaces.

**Backend.** One new `POST /api/contributions` endpoint and a matching
`db.contributions(pool, token, measure, method, arguments, mapping)`:
run `shapley_all_vars(token, method, arguments, banzhaf := measure ==
'banzhaf')`, resolve labels per the tiers above, sort by value
descending, and return `[{variable, value, label, source_row,
probability}]`. Error/empty handling mirrors `/api/evaluate`: a
constant circuit (no inputs) returns an empty list the UI renders as
"no input variables"; an older extension without `shapley_all_vars`
surfaces a 501 with the missing-function diagnostic, same shape as the
KC-helper unavailability responses.

**Optional cross with Where mode.** Because both the contribution bars
and Where-mode highlighting key off the same input-to-source-row
mapping, a later refinement could tint the contributing source cells
by their contribution magnitude (a true result-space heat-map) rather
than only listing them in the sidebar. Kept out of the first cut: the
sidebar chart is the MVP.

**Implementation steps.**

1. Route + shell: `/contributions` in `app.py` via `_serve_shell`,
   `body.mode-contributions`, a fourth `ps-modeswitch__btn` tab in
   `index.html`.
2. `db.contributions` + `POST /api/contributions` (label-resolution
   tiers, descending sort, 501/empty handling).
3. Front-end: a `setupContributionsMode` branch in `app.js` (peer of
   `setupCircuitMode`); add `contributions` to the `clickableUuid` set
   (`app.js:2928`) so result-token cells pin the target on click; the
   optional Where-mode `→ Contributions` jump button (parallel to the
   `data-jump-circuit` block at `app.js:3085`, carried via
   `sessionStorage` like `ps.preloadCircuit`); the chart/sidebar
   renderer; and the measure / method / mapping control row (reuse the
   eval-strip mapping-picker and approximate-annotation helpers).
4. Dispatch: contributions keeps the default UUID-carrier provenance
   (not `where`), so `wrap_last` behaves as in Circuit mode; only the
   mode-class swap and cache-drop on mode flip need handling in the
   `/api/exec` path.
5. Docs: a Contributions-mode section in `doc/source/user/studio.rst`
   and a row in the compatibility matrix; a screenshot via the
   existing `nb_doc_shots.py` pipeline.
6. Case study: extend **CS2** with a Studio Contributions coda over its
   existing Steps 13–15 (Shapley / Banzhaf on the Exercise→CVD→
   beneficial finding, labelled via `study_mapping`) rather than writing
   a new case study; planned in `case-studies.md`. The mode and this
   section should ship together.

#### Time-travel / Temporal DB mode

- Dedicated chrome for the temporal SRFs `timeslice` / `history` /
  `timetravel` (CS4 §3–5). Sidebar = view picker + date / window /
  column-filter that composes the SRF call; the result table
  renders the SRF output. (The temporal *semiring* evaluation,
  `sr_temporal`, is already in the eval-strip dropdown; this mode is
  about the SRF surface.)
- Motivation: the SRF call shape (`... AS (cols ...)`) plus the
  date / window / filter inputs warrant their own chrome rather
  than a generic eval-strip mini-panel.
- Natural home for a future **"undo last DML"** button (CS4 §7)
  that calls `SELECT undo(...)` server-side. Kept out of the
  main modes for now since `update_provenance` is not yet mature
  enough to expose prominently.

### Larger features

- **Result-table evaluation extension**: run the selected semiring
  across every row of the current result and add a column with the
  per-row value. Today the eval strip evaluates one node at a time;
  this would batch-evaluate all UUIDs in the displayed `provsql`
  column.
- **Multi-user demo deployment**: per-browser-session isolation in
  a single Docker container so a conference audience can each hit
  `localhost:8000` against a hosted instance.

### Notebook-mode polish

Small leftovers from the Notebook-mode plan, deferred from its MVP:

- **Collapse / clear cell output**: per-cell actions to fold a bulky
  output away or drop it (today output is removed only by re-running
  or deleting the cell).
- **Run from here**: run the selected cell and everything below it,
  complementing the existing Run / Run all.
- **Per-cell result-row cap**: the row cap is notebook-level only; a
  per-cell override (recorded in `metadata.provsql`) would let one
  wide cell coexist with a tight default.

## Implementation observations

- **Mode pattern**: each new mode is a sidebar template plus a
  route plus an `/api/...` endpoint, with the existing mode
  switcher gaining a tab. The `wrap_last` flag in `db.exec_batch`
  is the generalisation point: extend the dispatch in `app.py` to
  support the new mode's wrapping (or no wrapping) without
  touching the rest of the request path.
- **Eval-strip generalisation**: the per-node evaluation strip
  already shares `/api/evaluate` with what the future result-table
  extension would call. The extension batch-evaluates the displayed
  UUID column instead of one node, but the back-end dispatch is
  identical.
- **Contributions plumbing already present**: the in-mode target pin
  is the same cell-click Circuit mode uses, not a button. Result-token
  cells are made clickable via `data-circuit-uuid` / `is-clickable`
  gated on `clickableUuid` (`app.js:2928`) and a click runs
  `loadCircuit` (`app.js:2196`); Contributions mode joins that
  `clickableUuid` set and swaps the handler. The per-row `→ Circuit`
  *button* (`data-jump-circuit`, `app.js:3085`) is a Where-mode-only
  cross-mode jump; a `→ Contributions` button would sit beside it in
  Where mode, carried via `sessionStorage` like `ps.preloadCircuit`
  (`app.js:274`). Label resolution reuses `/api/leaf`
  (`circuit.py:resolve_input`) and the mapping picker reuses
  `/api/provenance_mappings`. The only genuinely new code is
  `db.contributions` over `shapley_all_vars` (one C function for both
  measures via its `banzhaf` flag) and the chart renderer.
