# Outer-join provenance (root fix), and scalar subqueries as the payoff

## Why this note exists

Goal: give scalar (value) subqueries `(SELECT x FROM Q WHERE corr)` a correct
provenance instead of rejecting them. The investigation showed the real blocker
is a **deeper, independent bug: ProvSQL's outer-join provenance is wrong**, and
the scalar-subquery "empty / 0-match world" problem is just one symptom of it.

So the plan is: **fix outer-join provenance (the root cause)**; scalar subqueries
then decorrelate to a `LEFT JOIN` + `choose` + `HAVING count(...) <= 1` with **no
special-case override**, and user-written outer joins become correct at the same
time.

This supersedes an earlier, more convoluted plan (a scalar-subquery-specific
gate-level "existence override" reconstructing `1 ⊖ [count≥2]` by hand). That was
only ever a hand-reconstruction of what a *correct* `LEFT JOIN` already produces,
so it is dropped in favour of fixing the join.

## The bug

ProvSQL builds provenance by annotating the rows of the **actual (all-present)
instance**. That is sound for monotone SPJU: every tuple that appears in any
possible world also appears in the all-present instance, so annotating the latter
captures all worlds.

**Outer join is non-monotone.** The null-padded row `(r, NULL)` appears precisely
when the right side is *empty* -- a *smaller* world, not the all-present one. So
for a left row `r` that *does* have a match in the actual instance, the
null-padded tuple is never materialised, and ProvSQL has nothing to annotate.

The offending code is the `RTE_JOIN` arm of `process_query` (`src/provsql.c`):

```c
if (jointype == JOIN_INNER || JOIN_LEFT || JOIN_FULL || JOIN_RIGHT) {
  // Nothing to do, there will also be RTE entries for the tables ...
}
```

`LEFT`/`FULL`/`RIGHT` are handled **identically to `INNER`** -- only the matched
branch `R ⊗ S` is emitted; the antijoin / null-padded branch is missing.

**Symptom** (q rows `(1,10),(1,20)` independent at p=0.5; `r1.k=1` at p=1):

| query | ProvSQL | correct |
|---|---|---|
| `r1 LEFT JOIN q ON q.k=r1.k GROUP BY r1.k` | 0.75 | **1.0** (group exists in every world `r1` is present) |
| `… HAVING count(q.k) <= 1` | 0.5 | **0.75** |

The *statically*-unmatched case is already right (a left row with no match in the
actual instance keeps its `(r,NULL)` at the outer probability -- the `k=2` case).
The bug is only the *probabilistically*-unmatched case.

## Correct semantics (validated against the ICDE 2026 ProvSQL paper, §IV-B)

```
R ⟕_θ S  =  (R ⋈_θ S)  ⊎  ( (R − π_R(R ⋈_θ S)) padded with NULL )
```

- **matched** `(r, s)` : `R(r) ⊗ S(s)` -- the inner join (already correct).
- **null-padded** `(r, NULL)` : the antijoin, with provenance
  ```
  R(r) ⊗ (1 ⊖ ⊕_{s : θ(r,s)} S(s))
  ```

built through ProvSQL's **multiset difference `−`**, which is the **`NOT IN`**
semantics

```
⟪q₁ − q₂⟫ = {{ (u, α ⊖ ⊕_{β : (u,β)∈⟪q₂⟫} β)  |  (u,α) ∈ ⟪q₁⟫ }}
```

and is **not** SQL `EXCEPT ALL`. The paper is explicit: the standard bag
difference `⟦q₁−q₂⟧(t) = max(0, m₁(t) − m₂(t))` (what `EXCEPT ALL` means) makes
provenance **intractable** and is deliberately not what ProvSQL implements;
ProvSQL's `−` corresponds to `NOT IN`, and `ε(q₁−q₂)` matches `EXCEPT`. Key
properties of `−` that make it exactly right here:

- it **keeps every left entry**, so it manufactures the "removed-in-actuality,
  present-in-a-sub-world" rows -- the whole point;
- it **preserves `R`'s multiplicity**: `(r,NULL)` appears `mult_R(r)` times when
  `r` is unmatched and `0` when matched (each copy reduces to `xᵢ ∧ ¬match` in
  Boolean), matching SQL `LEFT JOIN`'s bag semantics.

**Semiring-general.** Every semiring ProvSQL carries is positive and naturally
ordered, hence has the *canonical, unique* monus `a ⊖ b := ⊓{c : a ⊑ b ⊕ c}`. So
the antijoin provenance is defined and canonical in **all** of them (no "no-monus"
boundary). `FULL`/`RIGHT` are symmetric -- add the mirror antijoin branch on the
other side.

## The structural transform (in the planner hook)

Replace `R ⟕_θ S` with a subquery computing:

```sql
   ( SELECT R.cols, S.cols  FROM R JOIN S ON θ )              -- matched      (⊗)
   UNION ALL                                                  -- ⊎  (provenance_plus)
   ( SELECT R.cols, NULL, …, NULL
     FROM ( SELECT R.cols FROM R
            EXCEPT ALL                                        -- ProvSQL's −  (NOT-IN semantics:
            SELECT R.cols FROM R JOIN S ON θ ) )              --  R(r)⊗(1⊖⊕matches))
```

Both `UNION ALL` (`process_set_operation_union`) and `EXCEPT ALL` → `−`
(`transform_except_into_join`) are **native**, so the new code is purely
parse-tree construction plus a Var remap. Decision (with Senellart): the `EXCEPT`
rewrite was the *conceptual guide*; the implementation does the equivalent via the
tractable `−`/`NOT IN`, in the planner hook.

## Implementation plan -- explicit, ordered, each step independently testable

1. **Detect + scope-gate.** A pre-pass `lower_left_outer_joins(q)` (called for
   `CMD_SELECT` before `process_query`, when `provsql_active`) that fires only when
   `jointree.fromlist` contains a `JoinExpr` with `jointype = JOIN_LEFT` whose
   `larg`/`rarg` are base-relation `RangeTblRef`s. Everything else falls through
   unchanged, so nothing else can regress until the arm is correct.
   *Test: triggers on `R LEFT JOIN S`, no-ops elsewhere; the 196-test suite stays
   green.*
2. **Matched arm.** Build the inner-join subquery
   `SELECT R.cols, S.cols FROM R JOIN S ON θ`.
   *Test: matched-row existence equals today's LEFT-treated-as-INNER result.*
3. **Antijoin arm.** Build
   `SELECT R.cols, NULL,… FROM R EXCEPT ALL SELECT R.cols FROM R JOIN S ON θ`,
   padding `S`'s columns with typed `NULL` `Const`s; reuse the `EXCEPT ALL` → `−`
   path so the provenance is `R(r) ⊗ (1 ⊖ ⊕matches)`.
   *Test: on the `r1`/`q` example the antijoin existence is `0.75` and includes the
   0-match world.*
4. **`UNION ALL` + splice.** Combine the two arms under a `SetOperationStmt`;
   replace the `JoinExpr` with a `RangeTblRef` to the new subquery RTE
   (replace-RTE-in-place, line ~4951 pattern, so outer varnos for the *join* index
   stay valid); remap the base-`R`/`S` Vars to the subquery's columns through the
   join's `joinaliasvars` (R.i → subcol p, S.j → subcol q).
   *Test: `r1 LEFT JOIN q GROUP BY r1.k` → `1.0`; `… HAVING count(q.k) <= 1` →
   `0.75`; full suite green.*
5. **`FULL` / `RIGHT`** as the mirror antijoin branch (and both branches for
   `FULL`). Add `test/sql/outer_join.sql` pinning the possible-worlds values
   (matched, 0-match NULL, ≥2, and the `count`-HAVING cases) for `LEFT`/`RIGHT`/
   `FULL`, on tuple-independent and `repair_key` (BID) right sides.

**Then scalar subqueries fall out:** decorrelate
`(SELECT x FROM Q WHERE corr)` to a `LEFT JOIN`:

```sql
SELECT R.*, choose(Q.x)
FROM R LEFT JOIN Q ON corr
GROUP BY R.*
HAVING count(Q.k) <= 1            -- value = choose; existence from the FIXED LEFT JOIN
```

No gate-level override -- the corrected `LEFT JOIN` supplies the `0`-match NULL row
and the `count<=1` HAVING excludes only the `>=2` worlds. (The scalar-`SubLink`
interception itself already exists and currently errors; it would route here.)
This is a separate, smaller follow-up once outer joins are correct.

## Code pointers

- **The bug:** `src/provsql.c`, the `RTE_JOIN` arm (`JOIN_LEFT`/`FULL`/`RIGHT`
  "Nothing to do").
- **Antijoin/monus construction template:** `transform_except_into_join` -- builds
  `LEFT JOIN` + `provenance_monus`, including the `joinaliasvars` /
  `joinleftcols` / `joinrightcols` / `eref` bookkeeping the ruleutils deparser
  needs (NULL eref segfaults `pg_get_querydef`).
- **Replace-RTE-in-place, keep outer varnos:** line ~4951 (the safe-query
  subquery substitution) -- the technique that avoids remapping the *join*-index
  Vars; the base-relation Vars still need remapping via `joinaliasvars`.
- **Set-op handling:** `process_set_operation_union` (UNION ALL),
  `transform_except_into_join` (EXCEPT ALL → `−`).
- **Cached OIDs (already used nearby):** `OID_FUNCTION_PROVENANCE_TIMES`,
  `..._MONUS`, `..._PLUS`, `OID_FUNCTION_GATE_ONE`; the `choose` aggregate;
  `count(*)`.

## Validated facts (for the record)

- LEFT JOIN currently mis-computes: `r1 LEFT JOIN q GROUP BY r1.k` = 0.75 (should
  be 1.0); `… HAVING count(q.k) <= 1` = 0.5 (should be 0.75). `LEFT JOIN … HAVING
  count(q.k) <= 1` does **not** shortcut the fix -- still 0.5.
- The monus existence `outer ⊗ (1 ⊖ [count≥2])`, built by hand, evaluates
  correctly and is semiring-general: probability `0.75`, `sr_counting` → `0`,
  `sr_formula` → `𝟙 ⊖ (10 ⊗ 20)`, with **no** "does not support agg gates" error
  (the `cmp`/HAVING expands to a plain `⊕/⊗/⊖` circuit).
- `EXCEPT ALL` is native (→ `−` via `transform_except_into_join`); set `EXCEPT`
  (which needs the `δ` squash) is refused on aggregate results.
- ProvSQL's `−` ≠ SQL `EXCEPT ALL`: it is `NOT IN` (`α ⊖ ⊕β`); the standard bag
  difference `max(0, m₁−m₂)` is intractable for provenance and deliberately not
  implemented.
- Outer joins are accepted syntactically (`src/provsql.c:722`) but their
  provenance is wrong as above -- so the `CLAUDE.md` "JOIN (not outer/...)" line
  is *correct*, not stale.
