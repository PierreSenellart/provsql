# Scalar subqueries: m-semiring semantics and implementation

## Goal

Give a scalar (value) subquery `(SELECT x FROM Q WHERE corr)` a correct
m-semiring provenance, rather than rejecting it. Settled design (discussion with
Senellart):

- **value** `:= choose(x)` -- the PICKFIRST aggregate, total over any
  cardinality (`0` matches -> NULL, `1` -> the value, `>=2` -> first by
  occurrence order). Its provenance is the telescoping
  `⊕_i k_i ⊗ (⊗_{j<i} (1 ⊖ k_j)) ⊗ v_i`, which lives entirely in `⊕`/`⊗`/`⊖` +
  the value semimodule, so it evaluates correctly in every m-semiring.
- **existence** `:= outer ⊗ [count(matching Q) ≤ 1]`, the **exclude** semantics:
  worlds with `>=2` matches carry no mass (SQL would error there; an erroring
  world contributes nothing, which is the elegant reading). No renormalisation.
- order never matters: the only worlds where PICKFIRST's order could bite
  (`>=2` present) are exactly the ones the cardinality condition excludes.

`COUNT` (not `SUM`) also dodges the value-`0` additive-identity trap fixed in
`sum_dp`.

### Semiring generality (settled)

`[count ≤ 1] = 1 ⊖ [count ≥ 2]` is a plain `⊕`/`⊗`/`⊖` circuit once the `cmp`
is HAVING-expanded to its threshold polynomial (no opaque `agg` gate remains).
Every semiring ProvSQL carries is **positive and naturally ordered**, and a
naturally ordered semiring has the **canonical, unique** monus
`a ⊖ b := ⊓{c : a ⊑ b ⊕ c}`. So the exclude existence
`e ⊗ (1 ⊖ [count ≥ 2])` has a well-defined, canonical value in **every** ProvSQL
semiring -- there is no "no-monus" boundary (tropical, Viterbi, min-max included;
they are naturally ordered too). Examples: counting `multₑ ∸ multₑ·[≥2]` (the
ambiguous copies subtracted from the bag); Viterbi `e` unless the matches are
jointly certain; how/why/which `e ⊖ e·[≥2]`. Two consequences for the build:

1. **The `cmp` must be expanded to its polynomial on the compiled-semiring path**
   (`sr_*`), the same expansion the Boolean/probability path already does, so
   each evaluator sees `⊕`/`⊗`/`⊖`, not an `agg`/`cmp` gate it would reject.
2. **`[count ≥ 2]` must be the HAVING-expanded recovery on the real `count`**
   (counts rows *with multiplicity*), **not** the pairwise `⊕_{i<j} kᵢkⱼ`. They
   agree in Boolean/probability but diverge in counting (a single
   multiplicity-`≥2` row is ambiguous yet forms no pair). Correctness in
   non-idempotent semirings depends on this.

## The crux: `[count ≤ 1]` must include the empty world

`[count ≤ 1]` has to be **at-most-one** = `1 ⊖ [count ≥ 2]`, i.e. it includes
the **0-match world** (where the scalar is NULL and the row still exists) and
excludes only `>=2`. The standard aggregate provenance cannot express this,
because it conditions a group's existence on `>=1` contributing tuple
(empty-group exclusion). This was validated empirically (q rows `(1,10),(1,20)`
independent at p=0.5; outer `r1.k=1` at p=1; **target P = 0.75**):

| construction | event it actually builds | P | correct? |
|---|---|---|---|
| `choose(x) … HAVING count(*) <= 1` | `[exactly 1]` | 0.50 | no -- drops the 0-match world |
| `choose(x) … HAVING count(*) < 2`  | `[exactly 1]` | 0.50 | no -- same |
| `… LEFT JOIN q … GROUP BY` (no HAVING) | `[≥1]` (at-least-one) | 0.75 | **coincidence only** |
| `… HAVING count(*) >= 2` | `[≥2]` | 0.25 | this is the complement we need |
| `r1 EXCEPT (r1 ⋉ [count≥2])` | `[≤1]` | -- | **rejected** ("non-ALL set op on aggregate results") |

The `LEFT JOIN` aggregate matches `0.75` here **only** because at two symmetric
candidates `P(both absent) = P(both present) = 0.25`, so `[≥1]` and `[≤1]`
coincide numerically. With `>=3` candidates or asymmetric `p` they diverge:
`[≥1]` keeps the `>=2` worlds and drops the empty world -- the opposite of what we
want. So **no construction available through the SQL surface yields
`[≤1] = 1 ⊖ [count ≥ 2]` in general.**

## What is already available (the pieces all exist)

- **`LEFT JOIN` is handled by the rewriter** (`src/provsql.c:722` accepts
  `JOIN_LEFT`/`FULL`/`RIGHT`; verified: a no-match outer row survives with
  `s = NULL` at the outer row's probability). The `CLAUDE.md` "JOIN (not
  outer/...)" line is stale. So the value side -- including the `0`-match NULL
  row -- is handled by `R LEFT JOIN Q ON corr GROUP BY R.*`.
- **`A EXCEPT B` already lowers to `LEFT JOIN` + `provenance_monus`**
  (`transform_except_into_join`). So the `outer ⊖ X` shape is a built primitive;
  the missing bit is only that `EXCEPT` is *refused on aggregate results*, which
  is why `outer EXCEPT [count≥2]` is rejected at the SQL surface even though the
  gate-level transform it would use exists.
- **`choose`** is supported with the correct PICKFIRST provenance.
- **`[count ≥ 2]`** is expressible (`HAVING count(*) >= 2`, P = 0.25) -- the exact
  complement we need.
- **`gate_monus`** gives `1 ⊖ X`.
- the statically-empty case already returns the identity row (`empty_count`,
  issue #60: `count=0` with provenance `𝟙`); the probabilistic-empty case is
  precisely what the `1 ⊖ [count≥2]` existence repairs.

So the only missing link is **building `existence = outer ⊗ (1 ⊖ [count ≥ 2])`
and pairing it with the `choose` value** (NULL on empty).

## The override is validated (semantic core de-risked)

The existence circuit, built **by hand** from existing provenance UDFs, gives the
target and is semiring-general (q rows `(1,10),(1,20)` indep p=0.5; outer p=1):

```sql
-- [count≥2] over the correlated q:
SELECT provenance() AS c2 FROM r1, q WHERE q.k=r1.k GROUP BY r1.k HAVING count(*)>=2;
-- existence = outer ⊗ (𝟙 ⊖ [count≥2]):
SELECT provenance_times(:outer, provenance_monus(gate_one(), :c2));
```

| token | probability | note |
|---|---|---|
| `[count≥2]` | 0.2500 | the HAVING-expanded recovery (real count, not pairwise) |
| `𝟙 ⊖ [count≥2]` | 0.7500 | **empty world included** (true at count 0 and 1) |
| `outer ⊗ (𝟙 ⊖ [count≥2])` | **0.7500** | the target existence |

And it is a plain `⊕/⊗/⊖` circuit -- **no agg gate to reject**:
`sr_counting` returns `0`, `sr_formula` returns `𝟙 ⊖ (10 ⊗ 20)` (the symbolic
m-semiring answer). So the existence override works and evaluates in every
semiring; the remaining work is purely **making the rewriter emit it**.

## Implementation plan

Decorrelate `(SELECT x FROM Q WHERE corr)` to a `LEFT JOIN` + `GROUP BY` over the
outer, computing `choose(Q.x)`, and **override the group's existence provenance**
from the default `δ(⊕ k_i)` (= `[≥1]`) to `outer ⊗ (1 ⊖ [count ≥ 2])`:

1. **Rewriter (`src/provsql.c`):** pull the scalar `SubLink` into the `FROM` as a
   `LEFT JOIN` of the outer with `Q` on `corr` (LATERAL when correlated; a plain
   join when not), `GROUP BY` the outer's columns, and replace the `SubLink`
   node with `choose(Q.x)`. Mechanical parse-tree surgery (rtable + jointree +
   the SubLink->Var swap). Correlated LATERAL is already accepted by the existing
   rewriter, so the lateral case is in reach.
2. **Existence override (the one new circuit shape).** For a scalar-subquery
   group set the output row's provenance to
   `times(outer, monus(one, count_ge_2))`, where `count_ge_2` is the
   **HAVING-expanded recovery on `count(*) >= 2`** over the group members (not the
   pairwise product -- see the semiring note). Everything else is existing gates.
3. **Value:** `choose(Q.x)` unchanged (NULL on empty).

### Why the override has to be at the gate level (not SQL)

ProvSQL **couples a tuple's value and its provenance** -- there is no SQL way to
take `choose`'s *value* while replacing its *provenance*. Any SQL combination
re-multiplies provenances: joining the existence side `outer ⊗ (1 ⊖ [≥2])` with a
value side that carries `choose`'s own `[≥1]` existence yields
`outer ⊗ (1⊖[≥2]) ⊗ [≥1]`, which re-drops the empty world. So the value
(`choose`, `[≥1]`) and the existence (`1 ⊖ [≥2]`, includes empty) cannot be paired
in SQL; the output row must be built directly: **value = `choose`'s value,
provsql = `outer ⊗ (1 ⊖ [count≥2])`**, decoupled. That is the crux that forces a
rewriter/aggregation-provenance construction rather than a pure query rewrite.

**Alternative considered and rejected:** lift the "non-ALL set op on aggregate
results" restriction and do `R EXCEPT (R ⋉ [count≥2])`. It builds the existence
correctly (and `EXCEPT` already lowers to `LEFT JOIN`+`monus`), but `EXCEPT` drops
the value column -- and re-joining the value back re-multiplies the provenance
(same coupling problem). So the gate-level override is the path.

## Decisions to rediscuss with Senellart

1. **Existence override location.** Build `1 ⊖ [count≥2]` inside the
   aggregation-provenance construction for scalar-subquery groups (step 2), or as
   a query-level `EXCEPT` after lifting that restriction? The former is more
   contained and keeps the value pairing.
2. **`[count ≥ 2]` source.** Reuse the `COUNT` cmp evaluator (`count(*) >= 2`
   over the group) so the cardinality logic stays in one place, vs. a direct
   `⊕_{i<j} k_i ⊗ k_j` gate build (O(n^2), but no dependency on the cmp arm).
3. **Empty / NULL value carrier.** Confirm `choose(Q.x)`'s value (an `agg_token`)
   threads to the outer expression as a value (e.g. `a + (scalar)` -> `gate_arith`
   over the choose result), and that its NULL-on-empty reads correctly under the
   `1 ⊖ [count≥2]` existence (the row exists in the 0-match world with a NULL
   value).
4. **Strict variant.** We chose **exclude**; the **error** (SQL-faithful) variant
   is "raise when `[count ≥ 2]` is satisfiable" -- the same event, a triviality
   check on the `COUNT` gate (often settled by `RangeCheck`, no full SAT). Keep
   exclude as the default for an implicit scalar subquery, error as opt-in?

## Validated facts (for the record)

- Correlated `LATERAL (SELECT … FROM q WHERE q.k = r1.k …)` is accepted by the
  rewriter (lowered to `provenance_aggregate(choose(q.x), …)`); no LATERAL error.
- `LEFT JOIN` keeps the no-match outer row with `NULL` at the outer probability;
  inner join drops it.
- `HAVING count(*) <= 1` and `< 2` both compute `[exactly 1]` (drop the empty
  world) -- the empty-group exclusion, the same convention behind the `sum_dp`
  value-`0` bug.
- `EXCEPT` on an aggregate result is rejected
  (`Non-ALL set operations (UNION, EXCEPT) on aggregate results not supported`).
