# Documentation TODO: case-study coverage

`feature-coverage.md` (in this directory) lists user-guide features that
are not exercised by the tutorial or any of the five existing case
studies. This file plans how to close that gap, by extending CS1-CS5
where the existing scenario fits, and by sketching a new CS6 around
upcoming ProvSQL features.

## Out of scope

The following features are documented elsewhere and do not need
case-study real estate:

- `provsql.active`, `provsql.verbose_level`: covered by
  `user/configuration.rst` and `dev/debugging.rst`.
- `get_infos`, `get_extra`, `get_prob`: covered by the SQL API
  reference.

## Plan

### CS1: Intelligence Agency

- Add the `'independent'` and `'weightmc'` probability methods to the
  benchmark in Step 13. The step already compares possible-worlds,
  monte-carlo, tree-decomposition, and compilation/d4; `'independent'`
  is a useful teaching point because it succeeds or errors depending on
  circuit shape.

### CS2: Open Science Database

- `COUNT(DISTINCT study)` and `string_agg(study, ', ')` per
  (exposure, outcome): a natural addition before or after Step 4
  (single-source claims).
- `FILTER` clause: `COUNT(*) FILTER (WHERE effect = 'beneficial')` per
  exposure to rank exposures by net beneficial evidence.
- Window functions: rank exposures by reliability-weighted study count
  with `RANK() OVER (PARTITION BY outcome ORDER BY ...)`.
- `UNION ALL`: merge "beneficial" and "harmful" findings into a single
  signed-effect view, illustrating that ProvSQL combines provenance via ⊕.
- `aggregation_evaluate`: extend the evidence-grade semiring (Step 6) to
  a `GROUP BY outcome` query with a custom semimodule that aggregates
  per-finding grades into a per-outcome grade.

### CS3: Île-de-France Public Transit

- `LATERAL`: for each route, find the next reachable stop with a
  `LATERAL (SELECT ... LIMIT 1)`. Reads naturally as "what comes after
  Bagneux on this line".
- Window functions: `ROW_NUMBER() OVER (PARTITION BY trip_id ORDER BY
  stop_sequence)` to enumerate Bagneux's position along each trip.

### CS4: Government Ministers

- Add a step that calls `get_valid_time` directly on a single row.
  Currently only the higher-level `union_tstzintervals`, `timeslice`,
  `timetravel`, and `history` are exercised.
- Demonstrate `UPDATE` explicitly: replace the DELETE + INSERT pair in
  Step 6 with an `UPDATE holds SET ...`, then `undo` it. UPDATE is
  documented but not shown in any case study.

### CS5: Wildlife Photo Archive

- Add `'independent'` to Step 4 (before the `repair_key` step): the
  naive conjunctive query is independent-shaped, so the explicit method
  call succeeds and produces the instructive wrong answer that motivates
  Step 5.
- `INSERT ... SELECT` with provenance propagation: materialise a
  `confident_detections` provenance-tracked table from a
  high-confidence filter, showing that inserted rows inherit source
  provenance rather than fresh tokens.

### CS6: Upcoming features (new case study, blocked on implementation)

A new case study to be written once the corresponding ProvSQL features
land. Targets:

- **UDFs**: provenance propagation through user-defined functions.
- **Joining on an aggregate value**: each row joined with the per-group
  aggregate it belongs to, provenance correctly tracked through the
  aggregate.
- **`choose` for UDF uncertainty**: when a UDF returns one of several
  candidate outputs, `choose` models the alternatives as mutually
  exclusive in the provenance circuit (analogous to `repair_key`'s role
  in CS5, but for derived rather than ingested data).

## Priorities

1. **Quick wins on existing case studies** : the single-bullet
   additions to CS1 and CS3, and the small CS5 additions, can land
   independently and immediately close coverage gaps in
   `feature-coverage.md`.
2. **Larger CS2 / CS4 extensions** : CS2 grows by five bullets and CS4
   adds a UPDATE / `undo` round-trip. These are the biggest single-CS
   coverage wins.
3. **CS6** : blocked on the upstream features (UDFs, aggregate joins,
   `choose`) landing in ProvSQL; revisit when those ship.
