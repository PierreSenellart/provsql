# Studio: features and integration beyond v1.0

Plan for Studio work landing after the first public release.
Studio 1.0.0 shipped to PyPI alongside ProvSQL 1.4.0 in May 2026;
the user guide at `doc/source/user/studio.rst` documents the
compatibility matrix, version streams, and CLI flags. This file
now carries only the "Beyond v1.0" backlog (new inspection modes,
notebook integrations, larger features, implementation
observations). The Stage 0–5 bring-up notes that lived in
`studio/TODO.md` and the v1.0-blocking checklist that lived in
this file have both been retired into git history.

## Out of scope

The following are documented elsewhere and do not need a TODO entry:

- Compiled-semiring proposals: covered by `compiled-semirings.md`.
- Tutorial / case-study coverage gaps: covered by `case-studies.md`
  and `feature-coverage.md`.

## Beyond v1.0

### New inspection modes

Two additional modes share the existing chrome (query box,
result-table rendering, mode switcher) and add their own sidebar
plus per-cell click affordances.

#### Contributions mode

- Heat-map of per-input Shapley / Banzhaf contributions for the
  current result. The mode switcher gains a third tab; the sidebar
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
  renders the SRF output.
- Motivation: the SRF call shape (`... AS (cols ...)`) plus the
  date / window / filter inputs warrant their own chrome rather
  than a generic eval-strip mini-panel.
- Natural home for a future **"undo last DML"** button (CS4 §7)
  that calls `SELECT undo(...)` server-side. Kept out of the
  main modes for now since `update_provenance` is not yet mature
  enough to expose prominently.

### Notebooks (small)

- Save / load notebooks: query history is already persisted
  (sessionStorage `ps.sql` carry-over + the History dropdown). The
  remaining work is a "Download .sql" button next to the History
  dropdown that exports the recent buffer, and a file-picker that
  imports back into the textarea. About 30 lines of front-end.

### Larger features

- **Result-table evaluation extension**: run the selected semiring
  across every row of the current result and add a column with the
  per-row value. Today the eval strip evaluates one node at a time;
  this would batch-evaluate all UUIDs in the displayed `provsql`
  column.
- **Knowledge-compilation view**: render the d-DNNF compiled from a
  circuit, not just the raw provenance DAG. Surfaces what
  `provenance_evaluate_compiled` actually consumes and makes
  probability evaluation legible. Could ship as a sub-mode toggled
  from the circuit-mode toolbar (`Π`-shaped circuit ↔ d-DNNF view).
- **Multi-user demo deployment**: per-browser-session isolation in
  a single Docker container so a conference audience can each hit
  `localhost:8000` against a hosted instance.

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
- **Independent versioning**: Studio releases as `studio-vX.Y.Z`
  on PyPI; the extension stays on `vX.Y.Z` on PGXN. Compatibility
  surfaced via a startup check
  (`SELECT extversion FROM pg_extension WHERE extname = 'provsql'`)
  that refuses to start if the installed extension is older than
  Studio's minimum requirement, plus the matrix in
  `doc/source/user/studio.rst`.
