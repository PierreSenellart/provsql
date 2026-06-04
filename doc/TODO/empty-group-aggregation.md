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
- **Phase 2 -- generic-semiring + `cmp`-off path** (`count_enum`): the world
  enumeration adds the empty subset `mask=0` when `is_scalar` and `0 op C`; the
  caller (`having_semantics`) annotates it as `one ⊗ (𝟙 ⊖ ⊕(tuples))` -- the
  correct empty-world term in **every** m-semiring, including the absorptive
  Boolean expansion of the `cmp`-off probability path (no `!absorptive` gate
  needed: a true-on-empty predicate is a downset, `upset=false`, so the empty
  subset gets the monus complement). Verified `cmp`-on == `cmp`-off == MC ==
  `0.0625`; the scalar `count(*) < 4` why-provenance in `having_on_aggregation`
  now correctly carries the empty witness `{}` (a grouped count would not).

**Deferred (need care / human judgment):**
- **Phase 3 -- `IS NULL` HAVING** on `min`/`max`/`avg`/`array_agg`/`choose`:
  needs the HAVING lift (`needs_having_lift` / `having_Expr_to_provenance_cmp`)
  to accept a `NullTest` over an aggregate first (currently errors), then the
  empty-world term for the scalar case.
- **Phase 4 -- scalar existence `= gate_one`** ("no δ without grouping",
  `src/provsql.c:1843`): low value (the `p=1` corner is the less-useful number),
  but high churn -- every scalar aggregation's `provenance()` flips from
  `P(non-empty)` to `1`, needing many re-pins. Also resolve whether
  `GROUP BY <constant>` (e.g. positional `GROUP BY 1`) collapses to an empty
  `groupClause` (would wrongly trip the scalar test). Worth doing with a human in
  the loop on the re-pins.
- **Phase 5 -- retire `rewrite_uncorrelated_antijoin`**: once the cmp path is
  empty-world-correct on all routes (Phase 2 generic + the antijoin's monus is
  the same `𝟙 ⊖ ⊕` term), route uncorrelated `(SELECT count(*) …) < k` / `= 0` /
  `NOT EXISTS` through the simple cross-join HAVING-gate instead of the
  `EXCEPT ALL` construction. Verify per-semiring coverage before deleting.

## Cross-cutting

- Any new discriminating bit on a gate must enter its `uuid_generate_v5` content
  (the `aggfnoid` lesson).
- `expected`/`moment`/`support` over `agg_token` are the reference for the
  empty-world value semantics: `count→0`, `sum→0`, `min→+∞`, `max→−∞` (the
  monoid identities); HAVING uses the SQL values (`count→0`, the rest NULL).
- Release discipline: the new 5-argument `provenance_aggregate` must be added to
  the upgrade script before a release (the in-database function is replaced on a
  fresh `CREATE EXTENSION`, which is why the regression suite picks it up).
