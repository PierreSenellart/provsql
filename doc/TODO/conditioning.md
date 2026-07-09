# Conditioning

Conditioning – restricting a probabilistic object to the worlds where
some event holds – unifies discrete tuple-correlation (in the style of
MarkoViews) and continuous random variables as *one* primitive at two
carriers: a probability `P(Q | C)` and a distribution `X | C`. The
conditioning event is, in both, a Boolean provenance circuit `C` over
base variables; correlation between the conditioned object and the
evidence is handled automatically by content-addressing (a shared base
tuple is the same input gate in both circuits, so `P(X∧C)` over
`times(X, C)` is the true joint).

The design rationale, the MarkoViews reduction it rests on
(`P(Q | C) = P(Q ∧ C) / P(C)`, two unconditional evaluations plus a
division), and the surface that shipped are recorded in the git history,
the user manual (`doc/source/user/conditioning.rst`), and case study 8
(`doc/source/user/casestudy8.rst`). This file keeps only the open work.

Anchored on:

- MarkoViews: Jha & Suciu, *Probabilistic Databases with MarkoViews*,
  PVLDB 5(11), 2012.
- Koch & Olteanu, *Conditioning Probabilistic Databases*, PVLDB 1(1),
  2008 – the MayBMS assertion operation, the closest existing precedent.
- The base-independence backbone in
  [`continuous_distributions.md`](continuous_distributions.md).

## Out of scope

- Correlation primitives (`gate_copula`, `gate_mvnormal`): see
  [`continuous_distributions.md`](continuous_distributions.md) §D.1,
  §A.2. Conditioning *creates* correlation as a side effect; it is not a
  substitute for an explicit joint-distribution primitive.
- Causal interventions (`do`-calculus): a different operator
  (`continuous_distributions.md` §D.3). Conditioning is observation
  (`see`), not intervention (`do`).

## Open work

### Arbitrary denial constraints, beyond keys

`repair_key` covers key FDs only (a table becomes mutually-exclusive
renormalised blocks, after which any `probability_evaluate` is
implicitly conditioned on the FD holding). A general denial constraint –
"no two overlapping bookings", "an advisor must have been faculty" – is
conditioning on an arbitrary no-violation event, the full MarkoViews
`¬W`.

The expressivity itself is covered: `Q | !W` over an ordinary violation
query conditions a query on the constraint holding – the MarkoViews
`P(Q | ¬W)` reduction, demonstrated end to end in case study 8.

The one residual is an *optimisation*, not an expressivity gap: for the
`boolean_provenance` linear path to apply to a conditioned-on-constraint
query, the safety / inversion-freedom analysis has to re-run on the
constraint-augmented circuit (a cheap root `plus` joining `Q`'s circuit
and `W`'s), not the bare query, since safety is not invariant under the
augmentation. Workload-gated; the probability is already computed
correctly by the general methods today.

### Materialised, re-based discrete posterior (MayBMS-style)

A stored conditioned `uuid` token works for the store / re-condition /
evaluate cycle (it is terminal, so re-conditioning folds and
`probability_evaluate` reads it directly). The **one** thing it cannot
do is re-enter relational algebra: joining or unioning a conditioned
tuple raises rather than burying the global `P(C)` normalization under
further algebra. Making a stored posterior re-composable needs a
*re-based* posterior – fold the evidence in and treat the result as a
fresh independent representation, the MayBMS assertion operation.
Deferred until a persisted posterior must actually be composed onward.

### Shapley over evidence (research track)

`shapley` / `banzhaf` on a conditioned token are currently *refused*
with a clear error (`src/shapley.cpp`: Shapley / Banzhaf are linear over
the semiring, conditioning is not). The `observe`-evidence counterpart
(`provsql.shapley_observe`, attribution of observation atoms to a
posterior moment) exists; the open item is the analogue over a
`gate_conditioned` root with *Boolean* evidence, where the conditional
value *definition* is the research question to settle first.

### Soft / weighted conditioning (explicitly not a priority)

Hard conditioning only for discrete evidence, by decision. A
finite-weight MarkoView gate `gate_conditioned(X, event, weight)`
reweights worlds rather than restricting them, giving soft evidence –
"this observation is 90% reliable" – a first-class form. Hard
conditioning is the `weight → ∞` (indicator-likelihood) limit. The
continuous side already has the weighted-evidence machinery (the
`observe` / importance-weighting engine, see
[`latent-variables.md`](latent-variables.md)); the open item is the
finite-weight surface for *Boolean* evidence, landing as a parameter on
the same gate, not a new mechanism. When it lands, weights leaving `[0, 1]` make
the synthetic intermediate probabilities negative, so the
`probability_evaluate` dispatcher must route such inputs **exact-only**
and refuse `monte_carlo` / `independent` (the discrete twin of the
continuous rare-evidence rejection-sampling fragility).

## Priorities

1. **Arbitrary denial constraints beyond keys.** Re-run the safety /
   inversion-freedom analysis on the constraint-augmented circuit so the
   linear Boolean-provenance path applies. Workload-gated.
2. **Materialised re-based discrete posterior (MayBMS-style).** Deferred
   until a stored posterior must re-enter join / union.
3. **Shapley over evidence.** Research track; connecting code over the
   shipped surface once the conditional Shapley value is defined.
4. **Soft / weighted conditioning.** Explicitly not a priority; a
   parameter on the existing gate (with the exact-only dispatcher guard)
   when a use case makes the case.

## Implementation observations

- `gate_conditioned` on the `uuid` carrier is the **terminal** kind,
  non-composable with the semiring gates. The thing to resist is a
  *composable* discrete conditioning gate that would bury the global
  `P(C)` normalization under `plus` / `times`; the re-based posterior
  above is the sound route to composability.
- Conditioning is the canonical circuit operation that *defeats*
  independence shortcuts: any optimisation keyed on `FootprintCache`
  disjointness must treat `cond(X, A)` as widening the footprint to
  `footprint(X) ∪ footprint(A)`. The regression test belongs with the
  feature, not after it. This is the same coupling MarkoViews exploits
  deliberately.
- Negative / out-of-`[0,1]` input probabilities (soft conditioning, or a
  MarkoViews-style augmented circuit) are sound for the exact methods and
  only for them; the dispatcher must refuse sampling-based methods on
  such circuits. Hard conditioning on `[0,1]` evidence needs no such
  guard – every exact method is correct and the result lands in `[0,1]`.
- Safety / inversion-freedom must be re-evaluated on the conditioned (or
  constraint-augmented) circuit, never on the bare query: the
  augmentation is a cheap root `plus`, but the safe-query analysis is not
  invariant under it.
