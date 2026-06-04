# Empty-group aggregation: SQL-faithful scalar semantics

## Problem (from the audit)

ProvSQL's empty-group handling diverges from SQL, confined to **scalar (no
`GROUP BY`) aggregation over a possibly-empty input**. Grouped aggregation is
correct everywhere (SQL also drops the empty group). The scalar discrepancies:

1. a bare scalar aggregate row gets existence probability `P(non-empty)` instead
   of `1` (SQL: the row always exists -- `count`=0 / `sum`=NULL in the all-absent
   world);
2. a scalar `count` HAVING with a **true-on-empty** predicate (`= 0`, `< k`,
   `<= k`) under-counts by `P(empty)` -- the empty world is dropped;
3. a scalar `IS NULL` HAVING on `min`/`max`/`avg`/`array_agg`/`choose` (true on
   empty) errors (`Unknown structure within Boolean expression`).

Mechanism: the agg row provenance is `δ(⊕ tuples)` and `provenance_aggregate`
returns `gate_zero` for an empty group, so the empty world is `δ(𝟘)=𝟘` --
dropped -- in **every** semiring (not just probability). Only `count`'s empty
value (0) is a real value a comparison can satisfy; `sum`/`min`/`max`/`avg` are
NULL on empty so their comparisons are already false (no #2 for them).

## Unifying rule

The empty world's annotation is the monus complement `𝟙 ⊖ ⊕(tuples)` at the
empty-group value; include it wherever the existence/HAVING predicate holds at
that value. Special cases: predicate ≡ true ⇒ `δ(⊕) ⊕ (𝟙⊖⊕) = 𝟙` (`gate_one`)
[#1]; the probability projection of `𝟙⊖⊕` is `probZero = ∏(1−pᵢ)`. The
already-shipping `expected`/`moment`/`support` over `agg_token` already include
the empty world this way (conditioned by `prov`); these fixes bring the
predicate / existence paths into line with that.

## Mechanism: scalar flag on the agg gate

A scalar-aggregation flag in the **high bit of the `gate_agg`'s `info2`** (whose
low 31 bits hold the result-type OID; type OIDs never use bit 31). Set by the
rewriter when `q->groupClause == NIL && q->groupingSets == NIL`; passed to
`provenance_aggregate` as a separate `is_scalar boolean` argument so the clean
`aggtype` is still available to the agg_token→scalar cast
(`wrap_agg_token_with_cast`). **Folded into the gate's content UUID** (the `'S'`
marker in the `uuid_generate_v5` concat) so a scalar and a grouped aggregate over
identical children stay distinct gates and their `set_infos` calls do not clobber
-- the same discipline `aggfnoid` already follows. Readers mask
`& PROVSQL_AGG_TYPE_MASK` for the type, `& PROVSQL_AGG_SCALAR_FLAG` for the flag
(`src/provsql_utils.h`).

## Status

**Done (committed on `karp-luby`):**

- **Phase 1 -- infrastructure** (`e733bc1`): the flag, the hashed UUID, the
  masking. Behavior-neutral; scalar and grouped agg gates verified distinct.
- **Phase 2 -- scalar `count` true-on-empty HAVING, probability paths**
  (`ee7e93e`): `CountCmpEvaluator` (independent fast path) and
  `AggMarginalEvaluator` (hierarchical path) add `probZero` when the predicate
  holds at `count = 0`; Monte Carlo already samples the empty world. Verified
  `count(*)=0 → 0.0625`, `<2 → 0.3125`, etc., matching brute force; grouped
  unchanged. (Also corrected `scalar_subquery` Part uc2: an uncorrelated value
  body's `count(*) <= 1` gate now correctly counts the empty world, `0.375 →
  0.5` -- the row exists whenever the scalar subquery is well-defined.)
- **Phase 2 -- generic-semiring + `cmp`-off path** (`count_enum`): the empty
  world is folded into each branch's bound via `lo = is_scalar ? 0 : 1` (rather
  than appended), keeping it consistent with the upset / minimal-witness
  structure -- for the GE upset, `count >= 0` then has minimal witness `{}` and is
  a tautology. `having_semantics` annotates the empty subset as
  `one ⊗ (𝟙 ⊖ ⊕(tuples))` -- the correct term in **every** m-semiring, incl. the
  absorptive Boolean expansion of `cmp`-off. Verified `cmp`-on == `cmp`-off == MC
  across the family (`=0 → 0.0625`, `<2 → 0.3125`, `<>2 → 0.625`, …); the scalar
  `count(*) < 4` why-provenance in `having_on_aggregation` carries the empty
  witness `{}` (a grouped count would not).
- **Phase 2 -- tautology fix** (`runHavingAlwaysTrueRewriter`, `RangeCheck.cpp`):
  a true-on-empty predicate is a downset *except* `count >= 0` / `count > -K`
  (upsets) -- these are tautologies resolved by the always-true rewriter, which
  runs unconditionally in the probability path (so it intercepts these before
  `count_enum`). It used to rewrite an always-true count cmp to "the group is
  non-empty" (OR of the per-row K-gates) -- the grouped reading; for a **scalar**
  agg it now resolves to `gate_one` (probability 1), since the empty-input world
  is a real row. Grouped unchanged. Verified scalar `count(*) >= 0 == 1.0` on all
  routes; grouped stays group-existence (`0.75 / 0.5 / 0.5`). (`count(*)` is never
  rerouted to `sum_dp` -- it is detected as COUNT via the unit-weight remap; the
  tautology was intercepted by `RangeCheck`, not `sum_dp`.)

- **Phase 3 -- `IS NULL` HAVING** on `sum`/`avg`/`min`/`max`/`array_agg`/`choose`
  (`having_NullTest_to_provenance`): the HAVING lift now accepts a `NullTest` over
  an aggregate. These aggregates are NULL exactly when the aggregate has no
  contributor, so writing `⊕` for the OR of the per-row provenance tokens K (the
  2nd argument of each `provenance_semimod`, *not* the semimods -- those carry a
  value gate the evaluators cannot walk): `IS NOT NULL → δ(⊕)` (the group
  existence, scalar and grouped); scalar `IS NULL → 𝟙 ⊖ ⊕` (probZero); grouped
  `IS NULL → gate_zero`. Structural (monus/delta), hence route-independent.
  Verified scalar `sum/max/avg/array_agg IS NULL → 0.0625`, `IS NOT NULL →
  0.9375` (cmp-on == cmp-off == MC); grouped `IS NOT NULL` = group existence.
  (Known edge: a grouped group whose aggregated values are *all* NULL -- non-empty
  yet NULL-valued -- is treated as `gate_zero`; the row tokens differ from the
  value tokens there, which this rewrite does not separate.)

- **Phase 4 -- scalar existence `= gate_one`** ("no δ without grouping", the
  `aggregation && !lift_having` arm in `make_provenance_expression`): emit
  `gate_one` instead of `δ(⊕)` when `q->groupClause == NIL && q->groupingSets ==
  NIL`.  A scalar aggregate always returns one row (count 0 / sum-min-max NULL
  over the empty input), so its existence is certain.  This required decoupling a
  *second* meaning that was riding on the same `δ(⊕)` token -- "the aggregate is
  non-empty" -- which the **agg_token moment / support surface** used as its
  conditioning event.  The fix (option (b), in `agg_raw_moment` and `support`):
  for the NULL-on-empty aggregates `min`/`max`, the empty input world carries no
  value (SQL NULL, not the `±∞` monoid identity), so the moment/support is
  **conditional on the aggregate being defined** -- the empty world is excluded
  and the result renormalised by `P(prov AND non-empty)`.  So `expected(min(v))`
  is finite (= `E[min | non-empty]`) *by default*, and passing `provenance()` is
  no longer needed (it is now `gate_one` anyway).  `count` (empty = 0) and `sum`
  (empty = 0, the "expected total") keep the empty world.  The nested-aggregate
  refusal (`nested_agg_refuse` Case A) becomes a well-defined query rather than an
  error -- the scalar outer `provenance()` is now the constant `gate_one`, so
  `sum(probability_evaluate(provenance()))` is `sum(1.0)` over the inner rows (no
  nested Aggref).  Verified: scalar `count(*)` existence `0.79 → 1.0`;
  `expected(min)` / `variance(min)` / `support(min)` finite & conditional; grouped
  `min`/`max` moments `±∞ → finite`; all 201 tests green.  (`GROUP BY <constant>`
  is fine: positional `GROUP BY 1` keeps a non-NIL `groupClause`, so it is
  correctly grouped, not scalar.)

**Resolved -- Phase 5 will NOT retire `rewrite_uncorrelated_antijoin`** (kept on
purpose). The hypothesis was that, now the scalar `cmp` path is empty-world-correct
(Phase 2), uncorrelated `(SELECT count(*) …) < k` / `= 0` / `NOT EXISTS` could route
through the simple cross-join HAVING-gate (`move_uncorrelated_where_predicates`)
instead of the `EXCEPT ALL` construction. An A/B probe (antijoin call disabled,
fallback observed) shows it cannot:

- **`count(col)` with NULLs regresses.** The antijoin reformulates a true-on-empty
  predicate as `R ⊗ (𝟙 ⊖ ⟦P'⟧)` for the *false*-on-empty `P'` (e.g. `count(x) >= 1`),
  so the empty world never needs the scalar `cmp` annotation -- `⟦count(x) >= 1⟧` is
  evaluated directly from the value-correct PMF, which counts only non-NULL
  contributors. The cross-join HAVING-gate instead emits the true-on-empty
  `count(x) = 0` directly, whose scalar empty-world term is `𝟙 ⊖ ⊕(all row tokens)`
  = `probZero` over *every* row -- conflating "row absent" with "row present but the
  counted column is NULL". On `cc_q = {(10),(20),(NULL)}` @ p=0.5, `count(x) < 1`
  gives the antijoin's correct **0.25** (both non-NULL rows absent) but the fallback's
  wrong **0.125** (all three absent). This is the same latent edge as the direct
  scalar `… HAVING count(col) = 0` (which also returns 0.125), i.e. the Phase 3
  "all-NULL-values group" known edge -- the scalar `cmp` annotation is over row
  *existence* tokens, not value-contributing tokens. `count(*)` (every present row
  contributes 1) has no such gap and the fallback matches exactly (`= 0 → 0.125`,
  `< 2 → 0.5`, `= 2 → 0.375`).
- **No `NOT EXISTS` arm.** `move_uncorrelated_where_predicates` handles
  `EXISTS` / `(agg) OP v` but not a `NOT`-wrapped `EXISTS`; with the antijoin off,
  uncorrelated `NOT EXISTS` / `NOT IN` raises "Subqueries … not supported".
- **Circuit vs evaluator robustness.** The antijoin bakes a `monus` *gate* into the
  circuit (correct in every consumer / semiring, Shapley included, with no per-route
  flag handling), whereas the HAVING-gate emits a `cmp` gate whose empty-world
  correctness is re-derived in each evaluator from the `is_scalar` flag -- strictly
  less robust.

The one fully-equivalent sub-case is uncorrelated `count(*)` true-on-empty, but
retiring only that arm shrinks `rewrite_uncorrelated_antijoin` without removing it,
splits `count(*)` from `count(col)` handling, and buys negligible simplification. So
the antijoin stays whole; this phase is closed.

## Cross-cutting

- Any new discriminating bit on a gate must enter its `uuid_generate_v5` content
  (the `aggfnoid` lesson).
- `expected`/`moment`/`support` over `agg_token` are the reference for the
  empty-world value semantics: `count→0`, `sum→0`, `min→+∞`, `max→−∞` (the
  monoid identities); HAVING uses the SQL values (`count→0`, the rest NULL).
- Release discipline: the new 5-argument `provenance_aggregate` must be added to
  the upgrade script before a release (the in-database function is replaced on a
  fresh `CREATE EXTENSION`, which is why the regression suite picks it up).
