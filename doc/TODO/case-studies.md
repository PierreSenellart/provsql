# Documentation TODO: case-study coverage

The feature-coverage matrix in the rendered user guide
(`doc/source/user/casestudies.rst`, section *Feature coverage matrix*)
lists user-guide features that are not exercised by the tutorial or any
of the eight existing case studies. This file plans how to close that
gap, by extending CS1-CS5 where the existing scenario fits. CS6 (City
Air-Quality Sensor Network) already covers the continuous-distribution
surface end-to-end, CS7 (Peer-Review Assignment and Knowledge
Compilation) the knowledge-compilation and safe-query surface, and CS8
(ProvSQL as a Probability Calculator) the conditioning surface.

## Out of scope

The following features are documented elsewhere and do not need
case-study real estate:

- `provsql.active`, `provsql.verbose_level`: covered by
  `user/configuration.rst` and `dev/debugging.rst`.
- `get_infos`, `get_extra`, `get_prob`: covered by the SQL API
  reference.

## Plan

### CS1: Intelligence Agency — DONE (independent)

- Added the `'independent'` method as a prose teaching point after the
  Step 13 benchmark: it errors ``Not an independent circuit`` on the
  correlated path query, so it is the method for genuinely
  tuple-independent lineage. (``'weightmc'`` was dropped: the external
  weighted model counter is obsolete and typically not installed.)

### CS2: Open Science Database — DONE (3 aggregate steps)

- Added Step 17 "Richer Aggregates": `COUNT(DISTINCT study)`,
  `string_agg(study, ', ')`, and `COUNT(*) FILTER (WHERE effect =
  'beneficial')` per (exposure, outcome) -- each returned as a tracked
  agg_token.
- Added Step 18 "Signed-Effect View with `UNION ALL`": beneficial and
  harmful Coffee→CVD findings concatenated, each arm keeping its own
  provenance.
- Tested in `test/sql/casestudy2.sql`.
- Window functions (`RANK`): **skipped** -- opaque to provenance
  (warn-and-degrade); the matrix row was removed.
- `aggregation_evaluate`: **dropped** as obsolete (no longer a useful
  surface); its user-doc mentions are being removed separately.

### CS3: Île-de-France Public Transit — DONE (LATERAL)

- `LATERAL`: added Step 6 "The Next Stop on Each Line" -- a
  `LATERAL (SELECT ... ORDER BY stop_sequence LIMIT 1)` giving the next
  stop after Bagneux per line, with `sr_boolean` accessibility of the hop.
  Provenance flows through the lateral even though `stop_times` / `routes`
  are untracked (they contribute certain provenance, as in any join).
  Tested in `test/sql/casestudy3.sql` (DestA accessible, DestB not).
- Window functions (`ROW_NUMBER`): **skipped**. Window functions are
  opaque to provenance (they warn-and-degrade: "window functions are not
  supported; provenance is tracked per input row only"), so this would
  document a limitation rather than a working feature.

### CS4: Government Ministers

- Add a step that calls `get_valid_time` directly on a single row.
  Currently only the higher-level `union_tstzintervals`, `timeslice`,
  `timetravel`, and `history` are exercised.
- Demonstrate `UPDATE` explicitly: replace the DELETE + INSERT pair in
  Step 6 with an `UPDATE holds SET ...`, then `undo` it. UPDATE is
  documented but not shown in any case study.

### CS5: Wildlife Photo Archive — DONE (INSERT … SELECT)

- `INSERT ... SELECT` with provenance propagation: added Step 11
  materialising a `confident_detections` provenance-tracked table from a
  high-confidence filter; the inserted rows inherit the source detection
  tokens (prob == confidence). The target must be provenance-tracked
  first (inserting into an untracked table drops the lineage with a
  warning). Tested in `test/sql/casestudy5.sql`.
- `'independent'` on the naive query: **skipped**. The premise was wrong —
  the naive self-join over photo 5's shared deer/fox candidates is *not*
  independent, so `'independent'` errors ``Not an independent circuit``
  (a guard, not a silent over-estimate). CS1 already documents
  `'independent'` as prose.

### Future case study (UDFs / aggregate joins / `choose`)

A new case study to be written. (The CS8 slot is now taken by *ProvSQL
as a Probability Calculator*, so this would be CS9.) Targets:

- **UDFs**: provenance propagation through user-defined functions
  (blocked: not yet supported).
- **Joining on an aggregate value**: each row joined with the per-group
  aggregate it belongs to, provenance correctly tracked through the
  aggregate (blocked: not yet supported).
- **`choose` for UDF uncertainty**: when a UDF returns one of several
  candidate outputs, `choose` models the alternatives as mutually
  exclusive in the provenance circuit (analogous to `repair_key`'s role
  in CS5, but for derived rather than ingested data). The `choose`
  aggregate itself has shipped; the case study is still unwritten.

## Priorities

1. **Quick wins on existing case studies** : the single-bullet
   additions to CS1 and CS3, and the small CS5 additions, can land
   independently and immediately close coverage gaps in the
   feature-coverage matrix.
2. **Larger CS2 / CS4 extensions** : CS2 grows by five bullets and CS4
   adds a UPDATE / `undo` round-trip. These are the biggest single-CS
   coverage wins.
3. **New UDF / aggregate-join case study** : still blocked on the
   upstream features (UDF provenance, aggregate joins) landing in
   ProvSQL; revisit when those ship.
