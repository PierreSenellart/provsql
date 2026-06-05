# Conditioning

Conditioning – restricting a probabilistic object to the worlds where
some event holds – recurs in two ProvSQL settings that look unrelated at
first: discrete tuple-correlation in the style of MarkoViews, and
continuous random variables. This plan argues they are *one* primitive
at two carriers, extracts the conditioning-as-a-gate proposal that
previously lived in
[`continuous_distributions.md`](continuous_distributions.md) §D.1,
folds in what the MarkoViews framework teaches, and grounds the whole
thing in concrete use cases.

Anchored on:

- MarkoViews: Jha & Suciu, *Probabilistic Databases with MarkoViews*,
  PVLDB 5(11), 2012 (`https://vldb.org/pvldb/vol5/p1160_abhayjha_vldb2012.pdf`).
- Koch & Olteanu, *Conditioning Probabilistic Databases*, PVLDB 1(1),
  2008 – the MayBMS assertion operation, the closest existing
  precedent for conditioning a relational PDB on observed evidence.
- The base-independence backbone in
  [`continuous_distributions.md`](continuous_distributions.md)
  (§Theoretical backbone) and its §D.1 conditioning-gate sketch, which
  this document supersedes and expands.

## Out of scope

- Correlation primitives (`gate_copula`, `gate_mvnormal`): see
  [`continuous_distributions.md`](continuous_distributions.md) §D.2,
  §A.5. Conditioning *creates* correlation as a side effect (§4 below);
  it is not a substitute for an explicit joint-distribution primitive.
- Causal interventions (`do`-calculus): a different operator
  (`continuous_distributions.md` §D.4). Conditioning is observation
  (`see`), not intervention (`do`); keeping them distinct is the point
  of that entry.

## 1. One primitive, two carriers

The conditioning *event* is, in both settings, a Boolean provenance
circuit `C` over base variables. What differs is the object being
conditioned:

- **Probability carrier (discrete).** `P(Q | C)`: a real number. The
  answer probability of a query, restricted to worlds where `C` holds.
  This is the MarkoViews / MayBMS setting.
- **Distribution carrier (continuous).** `X | C`: a `random_variable`.
  The distribution of an RV restricted to the worlds where `C` holds,
  which must itself flow into later arithmetic, comparators, and
  aggregates.

Both rest on the same architectural backbone (`continuous_distributions.md`
§Theoretical backbone): base events are independent (tuple-existence
Bernoullis discretely; base `gate_rv` draws continuously), and
correlation enters only through operations that share base variables.
Conditioning is exactly such an operation: it couples everything in the
footprint of `C`. The discrete and continuous cases are then the same
construction reading off two different carriers.

## 2. MarkoViews: the discrete precedent

An MVDB is `(Tup, w, V)`: possible tuples with weights, plus
**MarkoViews** – UCQ views `V(x̄)[w] :- Q` that attach a weight to every
output tuple, read as odds. `w > 1` is positive correlation among the
contributing tuples, `w < 1` negative, `w = 1` independence, `w = 0` a
hard constraint (the view must be empty). It is a Markov Logic Network
whose features are grounded UCQ atoms, with `P(world) = Φ/Z`.

The paper's whole content is **one reduction** (their Theorem 1): query
evaluation on an MVDB collapses to ordinary *tuple-independent*
evaluation plus a conditioning step.

- Each MarkoView `V_i` gets a fresh independent relation `NV_i`
  ("view `i` violated") with tuple probability `1 - w` (weight
  `(1-w)/w`).
- `W = ⋁_i (NV_i ∧ body_i)` is a query-independent "some constraint is
  violated" formula.
- Then

  ```
  P(Q) = ( P₀(Q ∨ W) − P₀(W) ) / ( 1 − P₀(W) )
  ```

  which is precisely the conditional `P₀(Q | ¬W)` on a tuple-independent
  database.

Read in ProvSQL terms, this is **conditioning the query's provenance on
a constraint circuit `¬W`**. Four lessons fall out:

1. **The discrete carrier needs no new gate.** The exact formula is two
   *unconditional* evaluations of the existing `probability_evaluate`
   dispatcher plus arithmetic: `P(Q | C) = P(Q ∧ C) / P(C)`. The
   conditioning *gate* (§3) earns its keep only where the result must
   flow onward as an object – the continuous carrier, or a materialised
   discrete posterior (§3, materialised conditional tables).

2. **Conditioning is how you get correlation, and where independence
   shortcuts must back off.** MarkoViews' entire purpose is to make
   previously-independent tuples correlated by conditioning on a shared
   constraint. This is the discrete face of the `FootprintCache` caveat
   in §3: `cond(X, A)` has effective footprint
   `footprint(X) ∪ footprint(A)`.

3. **Exact methods only, when weights leave `[0, 1]`.** When `w > 1` the
   synthetic probability `1 - w` is *negative*; the intermediate INDB is
   not a real PDB. Every exact method (Shannon expansion,
   inclusion-exclusion, OBDD, tree decomposition, d-DNNF linear
   evaluation) is correct over negative numbers and the final `P(Q)`
   lands in `[0, 1]`, but sampling and bound-based approximation break.
   If ProvSQL ever admits such inputs, `probability_evaluate`'s
   dispatcher must route them exact-only and refuse `monte_carlo` /
   `independent`. This mirrors the continuous "rare-evidence rejection
   sampling is fragile" failure mode (§4).

4. **Tractability is a property of the conditioned circuit.** A query
   tractable on its own can become intractable once `W` is conjoined;
   MarkoViews is tractable iff *both* `Q ∨ W` and `W` are safe. For
   ProvSQL's inversion-free / `boolean_provenance` safe-query rewriter,
   the safety analysis must run on the constraint-augmented circuit, not
   the bare query. Structurally the augmentation is only a root `plus`
   joining `Q`'s circuit and `W`'s, and the lineage of `Q ∨ W` is no
   larger than the two combined, so the construction is cheap; the
   *analysis* is what must be re-run.

## 3. Conditioning as a gate (continuous carrier)

Extracted and lightly expanded from
[`continuous_distributions.md`](continuous_distributions.md) §D.1.

A `gate_conditioned(rv_subcircuit, bool_subcircuit)` meaning "the
distribution of `rv` restricted to the event where `bool` holds".
Self-contained: it flows through any subsequent operation. Sampling is
rejection (already the MC fallback's behaviour when `prov` is passed);
analytical evaluation reuses the existing closed-form paths because they
are already conditional internally.

**Pipeline placement.**

- **Simplifier** gets `cond(cond(X, A), B) → cond(X, A ∧ B)`,
  `cond(X, true) → X`, and `cond(X, A) → X` when `A` is independent of
  `X`'s footprint (the `FootprintCache` already gives you this). The
  first rule is the continuous twin of MarkoViews folding another view
  into `W`: evidence accumulates into one event rather than nesting.
- **RangeCheck** treats `cond(X, X ∈ [a, b])` as truncation. The current
  closed-form-truncated path becomes the *specialisation* of a general
  `gate_conditioned` rule rather than a parallel codepath; two
  near-parallel codepaths collapse into one.
- **AnalyticEvaluator** picks up conditional CDFs where they exist;
  conditioning on an independent event factors as
  `P(A) × (unconditional CDF)`, the continuous analog of MarkoViews'
  exact `P(Q ∧ C) / P(C)`.
- **Expectation** semiring: every dispatcher that already takes
  `prov uuid DEFAULT gate_one()` becomes the special case "no explicit
  conditioning gate at the root", unifying the conditioning argument and
  the conditioning gate.
- **FootprintCache** caveat: `cond(X, A)` has effective footprint
  `footprint(X) ∪ footprint(A)`. The structural-independence shortcut on
  `gate_arith TIMES` must back off accordingly. *This is the one
  soundness risk and should land with a regression test that constructs
  `cond(X, A) * cond(Y, A)` and confirms the shortcut does not fire.*
  It is the same coupling MarkoViews exploits deliberately (§2, lesson
  2).

**New directions it opens.**

- **Materialised conditional tables.** Store `cond(rv, evidence)` in a
  regular `random_variable` column and drop the source tuples. Solves
  the "carrying both the distribution and its conditioning" problem, and
  is the continuous counterpart of the MayBMS assertion operation, which
  folds observed evidence back into the stored representation.
- **Sequential Bayesian updates.** Each piece of evidence is another
  `cond(..., new_event)` wrap; the `A ∧ B` fold avoids depth blow-up.
- **Truncation generalises** to the canonical degenerate case of
  same-RV-comparator conditioning.
- **Shapley over evidence** (`continuous_distributions.md` §E.1): with
  conditioning plus the existing Shapley machinery, "which observation
  most shifted my posterior moment?" is mostly connecting code.

**UI.** A `provsql.condition(rv, event_uuid)` function, plus optionally
an infix `|` operator reading as "given":

```sql
-- Bayesian update with materialisation
UPDATE patient_risk
SET risk = provsql.condition(
             risk,
             (SELECT provenance() FROM tests
              WHERE patient_id = 1 AND result = 'positive')
           )
WHERE patient_id = 1;

-- Operator sugar; RangeCheck recognises a same-RV bool as truncation
SELECT expected(measurement | (measurement > 0.5))
FROM sensor_readings;

-- Recursive Bayesian update over an evidence log
WITH RECURSIVE updates(step, dist) AS (
  SELECT 0, provsql.normal(0, 10)
  UNION ALL
  SELECT step + 1, provsql.condition(dist, e.evidence_token)
  FROM updates u, evidence_log e
  WHERE e.confirmed AND e.step_idx = u.step + 1
)
SELECT expected(dist), variance(dist)
FROM updates WHERE step = (SELECT max(step) FROM updates);
```

## 4. Where the two carriers meet

A single, carrier-parametric `gate_conditioned` serves both: its result
type follows its first child. When the first child is a
`random_variable` subcircuit, the result is a distribution (§3); when it
is a Boolean provenance root, the result is a probability and the gate is
usually unnecessary, since `P(Q ∧ C) / P(C)` reuses the existing
dispatcher (§2, lesson 1). The gate is mandatory only when a discrete
posterior must be *materialised* and queried again later.

Two properties are shared across carriers and worth stating once:

- **Conditioning manufactures correlation.** This is a feature
  discretely (it is the whole MarkoViews mechanism) and a hazard for the
  optimiser continuously (the `FootprintCache` back-off). Same
  phenomenon: the conditioning event couples everything in its
  footprint.
- **Conditioning is robust under exact evaluation, fragile under
  sampling.** Discretely, negative synthetic weights force exact-only
  (§2, lesson 3). Continuously, rejection sampling degrades as
  `P(event) → 0`. In both, prefer the closed-form / analytic path; treat
  the sampling path as a fallback that must warn on rare evidence.

## 5. Soft / weighted conditioning

The one direction MarkoViews points to beyond the §D.1 sketch. §3 models
only **hard** conditioning: the event holds, the object is restricted.
A MarkoView with finite weight `w ≠ 0` is **soft** conditioning: it
reweights worlds rather than restricting them. The continuous analog is
`gate_conditioned(X, event, weight)` that *reweights* the distribution by
a likelihood rather than truncating it, which is exactly the move from
rejection sampling to **importance / likelihood weighting**. This makes
rare-evidence conditioning tractable instead of fragile, and gives soft
evidence ("this observation is 90% reliable") a first-class form. Hard
conditioning is then the `weight → ∞` (or indicator-likelihood) limit, as
in the MLN reading where `w = ∞` is a hard constraint. Lower priority
than hard conditioning, but it shares the gate type and the evaluator
hooks, so it lands as a parameter rather than a new mechanism.

## 6. Concrete use cases

Phrased against ProvSQL's actual surface. Conditioning is *half-built*
today, which is the most useful thing to see before scoping the work:
the moment / sample / histogram dispatchers already take a `prov`
conditioning event, and `repair_key` already conditions on a key
constraint, but conditional *probability*, a conditioned *distribution
that flows onward*, and general constraints are missing. §6.A is what
works now; §6.B is the gap the primitive closes.

### 6.A Already expressible today

#### 6.A.1 Conditional moments, samples, and histograms of a probabilistic scalar

The `prov` argument on `expected` / `variance` / `moment` /
`central_moment` / `support` / `rv_sample` / `rv_histogram` *is* a
conditioning event: they compute `E[X^k | prov]` for both a
`random_variable` and an `agg_token`. So conditional expectation,
variance, sampling, and histograms of one scalar already work –
including the canonical truncation, the conditional-Value-at-Risk shape,
and conditional moments of a discrete GROUP BY aggregate
(`E[SUM(x) | event]`). This is the baseline §6.B.2 must lift from
"moment only" to "a distribution that flows onward".

#### 6.A.2 Key / functional-dependency constraints via `repair_key`

A hard key constraint – "at most one tuple per key", MarkoViews' weight-0
denial view `V2` ("a person has one advisor") in its keyed special case –
is `repair_key` today. It turns a table into mutually-exclusive,
renormalised blocks, after which any `probability_evaluate` is implicitly
conditioned on the FD holding. BID blocks (the TID/BID classifier in
`classify_query.c` separates mutual-exclusion from independence) are
exactly the weight-0, single-attribute instance of constraint
conditioning; §6.B.3 is the general denial-constraint case `repair_key`
does not cover.

### 6.B What needs the conditioning primitive

#### 6.B.1 Conditional probability of a discrete answer, `P(Q | C)`

`probability_evaluate(token, method, arguments)` has **no** `prov`
argument – the discrete twin of §6.A.1 is the missing piece. Today it is
a clumsy two-call `P(Q ∧ C) / P(C)`, and nothing stops someone reaching
for `monte_carlo` on a negative-weight (MarkoViews) circuit.

```sql
-- Entity resolution: "P(records 42 and 88 match | 17 and 42 confirmed)".
-- matches(a, b) is a probabilistic table (add_provenance + set_prob),
-- correlated through a transitivity rule.
SELECT probability_evaluate(times(m.provsql, ev.tok))   -- P(Q ∧ C)
       / probability_evaluate(ev.tok)                   -- P(C)
FROM matches m,
     (SELECT provsql AS tok FROM matches WHERE a = 17 AND b = 42) ev
WHERE m.a = 42 AND m.b = 88;
```

The fix is a `probability_evaluate(token, prov => …)` overload lowering
to exactly this, with the exact-only guard (§2, lessons 1 and 3). This is
the canonical MarkoViews / MayBMS scenario: conditioning a PDB on
newly-observed certain facts.

#### 6.B.2 A conditioned distribution that flows onward

§6.A.1 conditions a *moment*; it cannot return a `random_variable` to
store or compose. So **sequential Bayesian update is not expressible
today** – each step needs the posterior as a first-class value. This is
the core motivation for `gate_conditioned` and for materialised
conditional tables.

```sql
-- needs gate_conditioned: the posterior is itself a random_variable
WITH RECURSIVE belief(step, dist) AS (
  SELECT 0, provsql.normal(20.0, 5.0)            -- prior on the quantity
  UNION ALL
  SELECT u.step + 1, provsql.condition(u.dist, o.evidence_token)
  FROM belief u, observation_log o
  WHERE o.confirmed AND o.step_idx = u.step + 1
)
SELECT expected(dist), variance(dist)
FROM belief WHERE step = (SELECT max(step) FROM belief);

-- materialised conditional table: fold a (probabilistic) positive test
-- into a patient's stored risk distribution
UPDATE patient_risk
SET risk = provsql.condition(
             risk, (SELECT provenance() FROM tests
                    WHERE patient_id = 1 AND result = 'positive'))
WHERE patient_id = 1;
```

#### 6.B.3 Arbitrary denial constraints, beyond keys

`repair_key` (§6.A.2) covers key FDs only. A general denial constraint
– "no two overlapping bookings", "an advisor must have been faculty" – is
conditioning on an arbitrary UCQ no-violation event, the full MarkoViews
`¬W`. There is no surface for it today; it wants the same
`condition(token, no_violation_event)` plumbing as §6.B.1 with the
constraint circuit supplied by a helper.

#### 6.B.4 Explainable inference: Shapley over evidence

`shapley(token, var, …)` exists but has no conditioning argument. Once
§6.B.2 lands, "which observation most shifted the posterior expected
risk?" is `shapley` over a `gate_conditioned` root – connecting code over
existing machinery, unique to a provenance-aware system
(`continuous_distributions.md` §E.1).

```sql
-- after gate_conditioned: posterior_risk is a conditioned random_variable
SELECT evidence_id,
       provsql.shapley(posterior_risk, evidence_id, payoff => 'expected')
FROM posteriors, evidence_atoms
WHERE patient_id = 1;
```

#### 6.B.5 Soft evidence / likelihood weighting

Evidence is rarely certain: a test trusted at 90% is soft conditioning,
not hard. Discretely this is a finite-weight MarkoView; continuously it
is the weighted gate of §5, reweighting rather than truncating. Neither
exists today.

```sql
-- update belief with an observation trusted at 90%
SELECT provsql.condition(prior, evidence_token, weight => 0.9);
```

## Priorities

1. **Hard conditioning on the continuous carrier (§3).** The most
   leverage per architectural unit: it collapses the existing RangeCheck
   truncation codepath into one general mechanism, promotes the existing
   `prov` moment-conditioning argument (§6.A.1) from "moment only" to "a
   distribution that flows onward", and unblocks §6.B.2. Lands after the
   §F.1 per-distribution refactor in `continuous_distributions.md`,
   alongside the first architectural batch. The single soundness risk is
   the `FootprintCache` back-off; ship it with the `cond(X,A)*cond(Y,A)`
   regression test.
2. **Discrete event-conditioning as a thin SQL surface (§2, lesson 1).**
   A `probability_evaluate(token, prov => …)` overload lowering to
   `P(Q ∧ C) / P(C)` on the existing dispatcher, with the exact-only
   guard for out-of-range inputs. No new gate. Delivers §6.B.1 and §6.B.3
   (the latter once a no-violation-event helper exists).
3. **Materialised conditional tables (§3).** The MayBMS-style assertion:
   store and re-query a conditioned object. Needed once §6.B.2's running
   posteriors want persistence.
4. **Soft / weighted conditioning (§5).** Parameter on the same gate;
   bridges to importance weighting and soft evidence (§6.B.5). Lower
   priority, no new mechanism.
5. **Shapley over evidence (§6.B.4).** Research track; connecting code
   over (1) plus existing Shapley infrastructure.

## Implementation observations

- The MarkoViews reduction shows the discrete carrier wants *no new
  gate*: `P(Q | C) = P(Q ∧ C) / P(C)` is two existing evaluations and a
  division. Resist adding a discrete conditioning gate; reserve the gate
  for distribution-valued and materialised results.
- Conditioning is the canonical example of a circuit operation that
  *defeats* independence shortcuts. Any optimisation keyed on
  `FootprintCache` disjointness must treat `cond` as widening the
  footprint to the union; the regression test belongs with the feature,
  not after it.
- Negative / out-of-`[0,1]` input probabilities are sound for the exact
  methods and only for them. A conditioned circuit (or a MarkoViews-style
  augmented one) should carry a flag that the dispatcher reads to refuse
  sampling-based methods, paralleling the rare-evidence rejection
  warning on the continuous side.
- Safety / inversion-freedom must be re-evaluated on the conditioned
  circuit, never on the bare query. The augmentation is a cheap root
  `plus`, but the safe-query analysis is not invariant under it.
