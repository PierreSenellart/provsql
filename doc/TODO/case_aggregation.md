# CASE guarded by an aggregate condition

Feasibility study for a *meaningful* searched `CASE` whose guards are
comparisons over aggregates (`agg_token`), e.g.

```sql
SELECT CASE WHEN SUM(x) > 3 THEN SUM(y) ELSE SUM(z) END
FROM r GROUP BY g;
```

Today the RV surface lowers an RV-typed searched `CASE` into a `gate_case`
(`build_rv_case` / `rewrite_probability_event_mutator`, `src/provsql.c`); the
`agg_token` carrier has no counterpart, so the same shape over aggregates hits
the placeholder comparison operator `agg_token_lt_numeric` & friends
(`sql/provsql.common.sql`) and raises *"Comparison agg_token-numeric not
implemented"*. This file records why the gap is worth closing, why exact
evaluation is more tractable than a first look suggests, and the work that
remains. It is a **to-do**: shipped work is not duplicated here.

Anchored on:

- the existing `gate_case` RV surface (see
  [`continuous_distributions.md`](continuous_distributions.md));
- the aggregate-provenance HAVING evaluator `src/having_semantics.hpp`
  (`provsql_having`), whose possible-worlds machinery is the reuse target;
- the conditioning primitive [`conditioning.md`](conditioning.md), into whose
  `agg_cond` / `gate_conditioned` a two-arm `CASE` decomposes.

## Out of scope

- **The RV-typed `CASE`.** Already shipped as `gate_case` with continuous-RV
  semantics; this file is only the `agg_token` carrier. A `CASE` mixing RV and
  aggregate guards is not in scope and should stay rejected.
- **A new gate type.** `gate_case` is carrier-agnostic by construction
  (N-ary wires `[guard_1, value_1, ..., guard_k, value_k, default]`, data only
  in the wires, no `info`/`extra` — `src/provsql_utils.h`), so no enum append
  and no on-disk format change is needed. The enum is append-only; do not add a
  redundant `gate_agg_case`.
- **Semiring evaluation.** A guarded selection is not a semiring operation and
  stays refused by every `sr_*`, exactly as the RV `gate_case` is today.

## Why (the semantics)

For an RV, `CASE WHEN X > 3 THEN A ELSE B END` is uncertain because `X` has a
continuous law; the result is a new RV. For an `agg_token` the uncertainty
source is the **discrete input-provenance distribution**: fix which input
tuples exist, and `SUM(x)` has a definite value, the guard is definitely
true/false, and the arm is chosen. The result is an `agg_token` whose value is
indexed by the possible world — the natural consumers are the polymorphic
`expected` / `variance` / `moment` / `support` dispatchers (which already
accept `agg_token`), the `probability` surface, and `possible_worlds`.

Equivalently, a two-arm `CASE` is just two conditioned aggregates combined:
`(SUM(y) | (SUM(x) > 3))` on the guard's worlds and `(SUM(z) | ¬(SUM(x) > 3))`
on its complement — i.e. two `agg_cond` / `gate_conditioned` tokens
(`cond_predicate_target`, `src/provsql.c`). So the primitive it needs already
exists at the single-arm level.

## What is already in place (the cheap parts)

- **Guard lowering.** `predicate_to_condition_gate` (`src/provsql.c`) already
  routes an aggregate comparison to `having_OpExpr_to_provenance_cmp` and
  builds a `gate_cmp` event token; it is exercised by
  `SUM(x) | (SUM(x) > 3)`. A `CASE` guard is the *same* event.
- **Gate structure.** `gate_case` already carries the exact N-ary
  guard/value/default wire shape.
- **Consumer surface.** `expected` / `variance` / `moment` / `central_moment`
  / `support` already dispatch over `agg_token`; `gate_case` already has arms
  in `MonteCarloSampler` and `Expectation` (for the RV carrier) that the
  aggregate carrier can borrow the traversal shape from.

## Why exact evaluation is tractable (correcting a first impression)

The instinct is "the joint law of the guard aggregate `SUM(x)` and the arm
aggregate `SUM(y)` when they share tuples is #P-hard, so refuse exact and offer
only Monte Carlo." That is wrong on two counts, both settled by how
`provsql_having` already evaluates aggregate comparisons **exactly**:

1. **Correlation is free.** The evaluator enumerates worlds over the *union*
   of contributors, so a guard and an arm that share input tuples are computed
   consistently in each world. Shared tuples are the same base literal; the
   joint is exact by co-enumeration, not an independence approximation.

2. **It is not brute force.** `provsql_having` is structure-aware and has
   polynomial closed forms; the `2^n` mask enumeration
   (`combine_exhaustive_worlds`) is only the *fallback*:
   - **`choose()` / PICKFIRST** (`having_semantics.hpp`): the satisfying
     worlds telescope by first-present index —
     `⊕_{i: vᵢ matches} kᵢ ⊗ (⊗_{j<i}(1⊖kⱼ))` — an O(n) prefix product with
     mutually-exclusive disjuncts.
   - **`bool_or` / `bool_and`**: a two-valued aggregate is characterised
     directly in the m-semiring ("at least one `someE` present" telescoped
     × "none of `noneF` present" as a product of complements), *"rather than
     by a 2^n enumeration"*.
   - **Monotone `MIN` / `MAX`**: the upset shortcut and the absorptive
     `MAX`+`GE/GT` / `MIN`+`LE/LT` arms drop the missing-contributor factors.
   - **Certified path**: mutually-exclusive disjuncts go under a deterministic
     OR / `certified_exclusive_plus` (contributor cap 16).

   The `2^n` fallback is reached only for numeric-ordering comparisons and
   `array_agg` — aggregates with no exploitable value structure.

The consequence for `CASE`: its exact evaluability tracks the **aggregate
structure of the guards and arms**, not merely the group size, and the
cleverness transfers to value-carrying selection:

- **PICKFIRST arms** telescope into a *mixture over arm values* by the same
  first-present-index decomposition — each disjunct is a world with a definite
  selected value, i.e. a `gate_mixture` / categorical shape, not an
  enumeration.
- **Monotone guard** (`CASE WHEN MAX(x) > k THEN A ELSE B END`) splits the
  worlds into an upset (value `A`) and its complement (value `B`) along the
  threshold — a closed-form two-value split.
- **`2^n` co-enumeration** remains the backstop, exactly where HAVING already
  falls back (unstructured numeric ordering).

## Plan

### 1. Planner lowering: `agg_token`-typed `CASE` → `gate_case`

Add an `agg_token` branch to `rewrite_probability_event_mutator` mirroring the
RV `casetype == random_variable` branch: an `agg_case(ARRAY[...])` constructor
mirroring `build_rv_case`, reusing `predicate_to_condition_gate` for each guard
(free) and wrapping each arm value `agg_token → uuid`.

The delicacy is **phase ordering**, unlike the RV path. RV comparisons are
self-contained per row, so RV `CASE` lowering runs safely in the target-list
mutator. Aggregate guards/arms only become `provenance_aggregate` / `agg_token`
after `make_aggregation_expression`, and depend on the GROUP BY / HAVING
context, so the `agg_token`-`CASE` rewrite must slot in *after* the
aggregate-provenance setup, not alongside the RV pass. Verify against a nested
case (`CASE` arm that is itself an aggregate `CASE`).

### 2. Value-carrying world combination

HAVING's `combine_exhaustive_worlds` produces a **Boolean predicate**
("does the group survive?") by OR-ing the passing worlds. A `CASE` produces a
**value**: each world contributes `world-provenance ⊗ selected-arm-value`. So
this needs a value-carrying variant — closer to the semimodule combination in
`src/Aggregation.cpp` than to the Boolean HAVING fold — that, per world,
evaluates the guards (first-match), selects the arm, and accumulates the arm's
aggregate value into an `agg_token` distribution (value → provenance).

Implement the tiers in the order they pay off: (a) the structured closed forms
above (PICKFIRST → mixture telescoping; monotone guard → threshold split), and
(b) the `2^n` co-enumeration backstop over the combined guard+arm footprint,
sharing the contributor-collection and certified-OR machinery with
`provsql_having`.

### 3. Consumer wiring and refusals

Route the resulting `agg_token`-carrier `gate_case` through the existing
`expected` / `support` / moment dispatch and `possible_worlds`. Monte Carlo is
the easy first target: `MonteCarloSampler` already has both a `gate_agg` and a
`gate_case` arm; confirm they compose for the aggregate carrier (entropy source
is the input Bernoullis, not continuous leaves) and land it before the exact
tiers, so there is always a working answer.

Where an exact method has no tier for the guard/arm shape, refuse with a clear
message pointing at `monte_carlo` — the same pattern the RV surface uses for
its own intractable corners.

### 4. SQL surface, tests, docs

`agg_case` constructor + `agg_token` cast wrappers so the arms type-check;
the upgrade-script entries and the `extension_upgrade` canary at release time
(not during dev); a `test/sql/` file covering each tier (PICKFIRST arm,
monotone guard, `2^n` numeric fallback, MC path, and the refusal message);
user-manual prose once the surface settles.

## Priorities

1. **Planner lowering + Monte Carlo** (Plan 1, 3-MC): smallest surface that
   makes the query *work* end to end, reusing the free guard lowering and the
   existing sampler arms.
2. **Value-carrying closed forms** (Plan 2a): the PICKFIRST-mixture and
   monotone-threshold tiers, where the payoff over MC is largest and the HAVING
   precedent is most direct.
3. **`2^n` exact backstop** (Plan 2b) and the full consumer/refusal matrix
   (Plan 3): exact everywhere HAVING already is, MC elsewhere.
4. **SQL surface / tests / docs** (Plan 4): tracks the tiers as they land.

## Implementation observations

- The correlation-for-free property and the structure-aware closed forms are
  the reusable core; treat `src/having_semantics.hpp` as the reference and
  factor the contributor-collection + certified-OR out of it rather than
  re-deriving them.
- A two-arm `CASE` equals two `agg_cond` tokens; if that decomposition is
  cheaper to ship first (reusing `gate_conditioned` evaluation directly), it is
  a valid stepping stone to the general N-arm `gate_case`.
- Keep the `gate_case` enum single: the carrier is discriminated by its arm
  types (all `random_variable` vs all `agg_token`), not by a second gate type.
