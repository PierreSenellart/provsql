# Documentation TODO: case-study coverage

The feature-coverage matrix in the rendered user guide
(`doc/source/user/casestudies.rst`, section *Feature coverage matrix*)
lists user-guide features that are not exercised by the tutorial or any
of the eight existing case studies. This file tracks the remaining
gaps. CS6 (City Air-Quality Sensor Network) covers the
continuous-distribution surface end-to-end, CS7 (Peer-Review Assignment
and Knowledge Compilation) the knowledge-compilation and safe-query
surface, and CS8 (ProvSQL as a Probability Calculator) the conditioning
surface.

## Out of scope

The following features are documented elsewhere and do not need
case-study real estate:

- `provsql.active`, `provsql.verbose_level`: covered by
  `user/configuration.rst` and `dev/debugging.rst`.
- `get_infos`, `get_extra`, `get_prob`: covered by the SQL API
  reference.

## Plan

### CS4: Government Ministers

- Add a step that calls `get_valid_time` directly on a single row.
  Currently only the higher-level `union_tstzintervals`, `timeslice`,
  `timetravel`, and `history` are exercised.
- Demonstrate `UPDATE` explicitly: replace the DELETE + INSERT pair
  that retires and re-adds a `holds` row with an `UPDATE holds SET ...`.
  UPDATE is documented but not shown in any case study. (The `undo`
  round-trip is already shown in the rebuilt CS4.)

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

1. **CS4 extensions** : the direct `get_valid_time` call and the
   `UPDATE`-for-DELETE+INSERT swap close the remaining temporal /
   data-modification coverage gaps in the feature-coverage matrix.
2. **New UDF / aggregate-join case study** : still blocked on the
   upstream features (UDF provenance, aggregate joins) landing in
   ProvSQL; revisit when those ship.
