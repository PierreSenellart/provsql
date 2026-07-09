# Studio: open features and integration work

Backlog for Studio work (new inspection modes, larger features,
notebook polish, implementation observations). The user guide at
`doc/source/user/studio.rst` documents the compatibility matrix,
version streams, and CLI flags.

## Out of scope

- Tutorial / case-study coverage gaps: covered by `case-studies.md`.

## Plan

### New inspection modes

- **"Undo last DML" button** (CS4 §7) that calls `SELECT undo(...)`
  server-side. Kept out of the main modes for now since
  `update_provenance` is not yet mature enough to expose prominently.

### Larger features

- **Result-table evaluation extension**: run the selected semiring
  across every row of the current result and add a column with the
  per-row value. Today the eval strip evaluates one node at a time;
  this would batch-evaluate all UUIDs in the displayed `provsql`
  column.
- **Multi-user demo deployment**: per-browser-session isolation in
  a single Docker container so a conference audience can each hit
  `localhost:8000` against a hosted instance. The Playground already
  gives an audience zero-install isolated instances; this item is
  only worth doing for a demo that needs the *native* extension
  (external knowledge compilers, multi-backend features the WASM
  build lacks).

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
