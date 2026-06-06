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

- Heat-map of per-input Shapley / Banzhaf contributions for the
  current result. The mode switcher gains a new tab; the sidebar
  lists input gates with mapping-resolved labels and a per-input
  contribution bar; result-table rows get a "→ Contributions"
  affordance similar to the existing "→ Circuit".
- Backed by `shapley_all_vars` / `banzhaf_all_vars`. The eval-strip
  variants (per-node `shapley` / `banzhaf` with a variable-token
  picker) likely fold into this mode rather than living on the
  circuit canvas.
- Closes the CS2 §13–15 gap.

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
