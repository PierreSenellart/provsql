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

## 1bis. Agreed surface and model (June 2026)

This section is the decided plan; §2–§6 are the supporting rationale.

### The operator(s)

Conditioning is surfaced as a single overloaded operator `|`
("given"/"conditioned on"), all forms living in the `provsql` schema
(bare when `provsql` is on the search_path, like every other ProvSQL
operator). The right operand is always a Boolean event supplied as a
provenance token (`uuid`); evidence in `[0,1]` only. **Weighted / soft
(MarkoViews-style) conditioning (§2, §5) is explicitly not a priority** –
hard conditioning only; §2 stays as background, not a build target.

**Binary `|` – value-level conditioning** (function alias `cond`,
i.e. `cond(x, c) ≡ x | c`), carrier-parametric in its left operand,
result type following the left operand:

- `uuid | uuid` – condition a provenance token (a Boolean event) on
  another. A terminal `gate_conditioned` (see below); usually consumed
  by `probability_evaluate` to a scalar `P(x∧c)/P(c)`.
- `agg_token | uuid` – condition a discrete aggregate's distribution.
- `rv | uuid` – condition a random variable's distribution.

**Prefix unary `|` – whole-tuple output conditioning** (function alias
`given`, i.e. `| c ≡ given(c)`). Written as a *consumed* term in the
select list, it conditions the **output provenance** of the current
query's rows on `c`:

```sql
SELECT a, b, given((SELECT provenance() FROM tests
                    WHERE patient_id = s.id AND result = 'positive'))
FROM source s;
-- visible columns: a, b   (the given(...) term is stripped)
-- per-row output provenance: provenance() | <that row's evidence>
```

This is the *derive*-not-*mutate* answer to "condition an entire tuple
output": there is **no `set_provenance`** – the rewriter derives a new
relation's provenance, exactly as every query already derives output
provenance, just with one more operator. The literal `(a,b,c) | evidence`
form is *not* used (ProvSQL is a post-parse rewriter and cannot add
syntax; a record-typed `|` would collapse the columns and still not
reach the tuple's hidden `provsql`). The prefix term is the realizable
shape, and it is a directive (`given c`), never a naming of the `provsql`
column.

Mechanics and conventions:

- **Prefix, not postfix.** PostgreSQL removed postfix operators in PG14
  but keeps prefix operators on every supported version, so `| c` is
  safe across the CI matrix; `c |` would not be.
- **Coexistence.** Operators key on `(name, leftarg, rightarg)`, so the
  binary `|`(uuid,uuid)/(rv,uuid)/(agg_token,uuid) and the prefix
  `|`(none,uuid) all coexist, and none collide with core PG's binary `|`
  (integer bitwise-OR, different arg types). Prefix vs binary is
  disambiguated by the presence of a left operand (`a, | c` parses `| c`
  as prefix).
- **Per-row evidence is free.** Because `given(c)` is a select-list term,
  `c` is evaluated per output row and may correlate with the row's
  columns – each tuple is conditioned on its *own* evidence.
- **No-provenance → 1.** A row whose provenance defaults to `gate_one()`
  conditioned by `given(c)` is `1 | c = gate_conditioned(one, c)`,
  well-defined (a certain row, given `c`), so `SELECT given(c)` with no
  payload columns is harmless.
- **Rewriter novelty.** Both `| c` and `given(c)` resolve to one backing
  function the rewriter recognizes; the only new pattern is that the
  prefix term is *stripped* from the visible projection (a consumed
  marker) rather than *substituted* like `provenance()`.

### Why this stays consistent with semiring annotations

The default `provsql` column is a *semiring* annotation, not a
probability or a Boolean event. Conditioning is **not a semiring
operation**: `P(·|C) = P(·∧C)/P(C)` needs a normalizing division, which
no general semiring has (m-semirings have monus, not a multiplicative
inverse). So `|` does not act on the annotation *qua* semiring element;
it is meaningful only in the *measure* interpretation (probability, and
the RV / `agg_token` distribution evaluators).

Concretely, `|` builds a **`gate_conditioned`** marker that is

- transparent to the measure-flavoured evaluators (`probability_evaluate`
  → `P(·∧C)/P(C)`; the RV / `agg_token` moment / sample / histogram
  dispatchers → the restricted distribution), and
- **refused by every general `sr_*` semiring** (counting, why, formula,
  tropical, …),

reusing the `gate_assumed` "transparent-or-refuse" marker pattern (the
`'boolean'` / `'absorptive'` assumption markers). The semiring-safe
*shadow* of conditioning already exists: restriction without
normalization is plain `times(token, C)`. Conditioning = restriction +
a measure-only normalization; only the normalization leaves the semiring
world, and the marker is what fences it off so a conditioned token
cannot be silently fed to `sr_counting` et al.

### Prerequisite (A): `provenance()` is scope-local and inert, in every case

`provenance()` must resolve to the current (innermost) subquery scope in
**all** contexts, and the token it yields must be **inert**: obtaining it
– as a target-list column, a sublink result, or conditioning evidence –
reads the tuple's *identifier* without coupling that relation into the
surrounding row's lineage. Lineage coupling comes only from actual data
flow (joins, and the decorrelation of *non*-provenance values) and from
explicit conditioning. This separates **identity** (the token, inert)
from **existence** (the probabilistic lineage); naming a token never
asserts the tuple's existence into your row.

Today FROM-subqueries already resolve `provenance()` scope-locally and
expose it as a plain value, but a `provenance()` inside an
expression-context SubLink (scalar / `IN` / `EXISTS`) is *rejected* by a
defensive guard (`provsql.c` ~11757 / ~11777), because `has_provenance()`
and the provenance mutator do not descend into SubLinks, so the call
would otherwise reach the executor unprocessed. The fix:

- drop the guard;
- make `has_provenance()` and the provenance mutator descend into
  SubLinks, resolving `provenance()` to the SubLink's own scope;
- treat a `provenance()`-returning SubLink as an **inert scalar token
  fetch** – evaluate it to the uuid, *not* through value-subquery
  decorrelation (which would couple the relation into the outer lineage).

This is an independently-sensible model fix (it removes a current
limitation and makes `provenance()` uniform across FROM and expression
contexts), and it is the piece the canonical use case hinges on:

```sql
-- bare row token  |  inert evidence token
SELECT probability_evaluate(
         provenance() | (SELECT provenance() FROM tests
                         WHERE patient_id = p.id AND result = 'positive'))
FROM patients p;
```

The correlation between the two tokens is handled automatically by
content-addressing: a base tuple shared by `X` and `C` is the *same*
input gate in both circuits, so `P(X∧C)` over `times(X, C)` is the true
joint, not an independent product – no special joint-load path is needed.

### Carrier asymmetry, and the terminal discrete gate

Normalization by `P(C)` is **global** (a ratio of two whole-circuit
evaluations), so it does not distribute through `×` / `+`. The two
carriers answer this differently, and the discrete one is settled by an
explicit design choice:

- `rv | C` and `agg_token | C` yield a self-contained conditioned
  *distribution* that composes onward cleanly – `gate_conditioned` as a
  stored, composable gate is sound here (this is the §3 continuous gate).
- `uuid | C` builds a **terminal** `gate_conditioned(target, evidence)`
  gate. It is **not composable with the semiring gates**: it may not be
  a child of `plus` / `times` / `monus` / `semimod` / `agg` / `project`
  / etc., and the constructors refuse to build such a parent (so
  joining or unioning a conditioned tuple raises, rather than silently
  burying a posterior under further algebra where the global
  normalization would be meaningless). This makes the
  "discrete conditioning is root/answer-level" property *structural*
  rather than a convention.

The single operation a conditioned `uuid` token admits is **more
conditioning**: `cond(cond(X, A), B)` is allowed and **folds** to
`cond(X, A ∧ B)` – the evidence accumulates via `times` into the gate's
*evidence* child, so the gate never actually nests; it stays the binary
`gate_conditioned(target, evidence)` flattened to one level. This is
sequential Bayesian update (the §3 simplifier rule), and it is sound for
hard conditioning because `P((X|A)|B) = P(X | A∧B)`. Evaluation:
`probability_evaluate(gate_conditioned(X, E)) = P(X∧E)/P(E)`; every
general `sr_*` semiring refuses it.

This shapes **materialization** (creating a tuple whose `provsql` is a
conditioned token):

- `rv` / `agg_token`: trivial – ordinary column values that flow onward.
- `uuid`: **also fine now** – store the `gate_conditioned` token; it can
  be re-conditioned and evaluated as above with no special machinery,
  precisely because it is terminal. The **only** thing it cannot do is
  re-enter relational algebra (join / union). Making a conditioned tuple
  *re-composable* would need a **re-based posterior** (MayBMS-style: fold
  the evidence in and treat the result as a fresh independent
  representation); that is the one piece left **deferred**, and it is
  needed only when a stored posterior must be joined onward, not for the
  store / re-condition / evaluate cycle.

### Delivery target: a "ProvSQL as a probability calculator" case study

A dedicated case study (a sibling of the existing `doc/.../casestudy*`
narratives) that uses ProvSQL as an **exact, correlation-aware
probability calculator queried in SQL**, across **both carriers** –
discrete events *and* continuous random variables. It is a demonstrator
and an acceptance target for the whole conditioning surface, discrete and
continuous together; the implementation targets both, and any blocker on
the continuous side (e.g. the §F.1 per-distribution refactor in
`continuous_distributions.md`) is fixed as it is hit, not treated as a
gate.

The pitch: classic probability problems, expressed as queries over a
probabilistic database, with ProvSQL doing the (conditional) probability
arithmetic – *exactly* (not by sampling) and *correlation-aware* (the
provenance circuit tracks shared events, so joint and conditional
probabilities are right without independence assumptions or hand-rolled
inclusion–exclusion). That correlation-awareness is the differentiator a
spreadsheet or an independence-assuming tool cannot match, and it is
what makes "calculator in SQL" non-trivial.

**The database substrate is the other half of the pitch**, and the case
study should make it tangible rather than incidental – this is a
probability calculator that inherits everything a DBMS already provides:

- **The query language *is* the event algebra.** Events, evidence and
  hypotheses are arbitrary SQL – joins, `GROUP BY`, set operations,
  `WITH RECURSIVE`, window functions, subqueries – so a complex event is
  *specified*, declaratively and compactly, never enumerated by hand. A
  standalone calculator works over a small, explicitly listed sample
  space; here the sample space is whatever the tables hold, and the
  event is a predicate over it. (Recursion is the sharpest example:
  gambler's ruin / a Markov chain is a `WITH RECURSIVE`, not a
  hand-unrolled tree.)
- **Real data, at scale, with indexing and the planner.** The
  probabilistic model is an ordinary (large) dataset, not a toy: the
  evidence `(SELECT … WHERE patient_id = p.id AND result = 'positive')`
  is found by an index, the joins are planned and executed by Postgres,
  and only the *relevant* lineage reaches the circuit. The calculator
  scales with the database, not with a hand-built model.
- **Integration into ordinary analytics.** Conditional probabilities and
  posterior moments are columns in normal queries – they join with
  deterministic tables, feed views / CTAS / BI tools, and compose with
  the rest of a SQL pipeline. The probability lives *next to* the data it
  is about.
- **The model and its posteriors persist.** `add_provenance` + `set_prob`
  + `repair_key` define a stored, declarative probabilistic model that is
  queried (and updated) across sessions; materialised conditional tables
  persist posteriors for re-query. There is no per-session model rebuild.

So the case study argues two things at once: ProvSQL is *correct* where
naive tools are wrong (correlation-aware, exact), and it is *practical*
where standalone calculators are not (a real query language over real,
indexed, persistent data).

The translation dictionary it teaches (both carriers):

| probability | ProvSQL / SQL |
|---|---|
| event | tuple(s) with `set_prob` |
| `A ∧ B` | join (`provenance_times`) |
| `A ∨ B` | `UNION` (`provenance_plus`) |
| `¬A` | `EXCEPT` (`monus`) |
| mutually exclusive outcomes | `repair_key` |
| `P(A)` | `probability_evaluate(A)` |
| `P(A \| B)` | `probability_evaluate(A \| B)` |
| a continuous quantity | a `random_variable` (`normal`, `uniform`, …) |
| `E[X]`, `Var[X]` | `expected(X)`, `variance(X)` |
| `X \| C` (a posterior distribution) | `X \| C` (an `rv`, flows onward) |
| `E[X \| C]`, `Var[X \| C]` | `expected(X \| C)`, `variance(X \| C)` |
| sequential Bayesian update | `cond(cond(X,A),B)` / `given(...)` materialisation |

Candidate worked problems (a spread across both carriers):

Discrete:

- **Base-rate / medical test (Bayes).** Prevalence + sensitivity +
  specificity ⇒ `P(disease | positive)`; the canonical conditioning
  demo, showing `P(D|+) ≠ sensitivity`.
- **Correlation that matters.** `P(A ∪ B)` where `A`, `B` share a
  sub-event: ProvSQL does inclusion–exclusion automatically where a
  naive independent computation is wrong – the showcase example.
- **Monty Hall / two-child.** Conditioning on an observation; the
  textbook "surprising" answers fall out of `|`.
- **Mutual exclusion + conditioning.** A die via `repair_key`;
  `P(even | > 3)`.
- **Independence as a computed fact.** Check `P(A∧B) = P(A)·P(B)`; show
  conditioning breaks it when events are correlated.

Continuous (the same calculator, distribution-valued):

- **Bayesian inference on a quantity.** A `normal` prior, updated by
  observations into a posterior `random_variable`; `expected` /
  `variance` of the posterior. The continuous twin of the medical-test
  Bayes example, and the motivating sequential-update story (the
  posterior must be a *value that flows onward*, not just a moment).
- **Truncation as conditioning.** `X | (X > k)` – a truncated /
  conditional-Value-at-Risk distribution; `expected(X | X > k)`. Shows
  the RangeCheck truncation path as a *special case* of the general
  `|`.
- **Conditional moments of an aggregate.** `E[SUM(x) | event]` over a
  probabilistic `GROUP BY` (the `agg_token` carrier).
- **Mixed discrete/continuous.** A continuous posterior conditioned on a
  discrete event (e.g. a measurement distribution given that a
  correlated sensor fired).

Cross-cutting / tie-in:

- **Recursive / random walk.** Gambler's ruin or a conditional
  reachability probability as a `WITH RECURSIVE`, linking to the
  bounded-treewidth recursive route – the substrate argument's sharpest
  case (the event *is* a recursive query, not a hand-unrolled tree).
- **Grounded at scale (the substrate argument made concrete).** One
  problem over a real, indexed table rather than a toy sample space –
  entity resolution / record linkage (`P(records 42, 88 match |
  17, 42 confirmed)` over correlated `matches`), or per-component network
  reliability – where the event and the evidence are non-trivial queries
  resolved through indexes and the planner, and the *same* `|` machinery
  that cracks the textbook puzzles runs over thousands of correlated
  tuples. This is the example that proves the calculator is not a toy.

How it shapes the implementation:

- The CS is the acceptance target for the **whole operator across both
  carriers** – discrete `cond` / `probability_evaluate(A | B)` *and*
  continuous `rv | C` / `agg_token | C` with `expected` / `variance` of
  the conditioned object. The continuous carrier is in scope from the
  start, not deferred; if the §F.1 per-distribution refactor (or any
  other continuous-side blocker) gets in the way, it is fixed as part of
  this work rather than gating it.
- It forces the `P(C)=0` (impossible evidence) decision early (lean:
  NULL), and on the continuous side the rare-evidence behaviour (prefer
  the analytic / closed-form path; the rejection-sampling fallback must
  warn as `P(C) → 0`).
- It pins the **`FootprintCache` soundness risk** as a first-class test:
  the `cond(X,A) * cond(Y,A)` shared-evidence case must defeat the
  `gate_arith TIMES` structural-independence shortcut – ship the
  regression test with the feature.
- It exercises the interplay with `repair_key` mutual exclusion and the
  existing `probability_evaluate` method portfolio (all exact methods
  correct on `[0,1]` evidence; no exact-only guard needed, weighted /
  MarkoViews inputs being out of scope).
- It may motivate a thin `conditional_probability(A, B)` convenience, or
  confirm `probability_evaluate(A | B)` is enough.

### Build order

1. **(A)** `provenance()` scope-local + inert in SubLinks (drop the
   guard, descend, inert fetch). Standalone prerequisite, testable in
   isolation: the currently-rejected scalar-subquery cases now return
   inert tokens.
2. **(B)** the whole `|` conditioning surface, **both carriers**, on top
   of (A) – built as one deliverable, not staged discrete-then-continuous:
   - binary `|` / `cond` value operator for `uuid` / `rv` / `agg_token`;
   - prefix `|` / `given` whole-tuple output directive;
   - the `gate_conditioned` gate – terminal for `uuid`, composable
     distribution for `rv` / `agg_token` – transparent to the measure
     evaluators, refused by general semirings;
   - `probability_evaluate(A | B)` → scalar; `expected` / `variance` of a
     conditioned `rv` / `agg_token`.
   The continuous carrier is in scope from the start; the §F.1
   per-distribution refactor in `continuous_distributions.md` (or any
   other continuous-side blocker) is **fixed inline as part of this
   work**, not a gate. Ship the `cond(X,A) * cond(Y,A)` `FootprintCache`
   regression test with it.
3. **(C)** the "ProvSQL as a probability calculator" case study
   (discrete + continuous) – the acceptance target for (B).
4. Materialised *re-based* discrete posterior (MayBMS-style, for
   re-composition into join / union) – deferred.
5. Soft / weighted conditioning – not a priority.

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

Superseding the original ordering below: the agreed plan (§1bis) leads
with the model prerequisite, then the unified operator.

0. **(A) `provenance()` scope-local + inert in SubLinks (§1bis).** The
   enabling model fix, independently sensible and testable in isolation;
   the `|` operator's ergonomics hinge on it. Drop the sublink guard,
   descend into SubLinks, inert token fetch.
1. **(B) the whole `|` conditioning surface, both carriers (§1bis).**
   Binary `|` / `cond` for `uuid` / `rv` / `agg_token`, prefix `|` /
   `given` whole-tuple directive, the `gate_conditioned` gate (terminal
   for `uuid`, composable distribution for `rv` / `agg_token`, refusing
   non-measure semirings), `probability_evaluate(A|B)` and
   `expected`/`variance` of conditioned distributions. **One deliverable,
   discrete and continuous together** – the §F.1 continuous refactor (or
   any continuous blocker) is fixed inline, not a gate. Ships with the
   `cond(X,A)*cond(Y,A)` `FootprintCache` regression test.
2. **(C) Delivery target: the "ProvSQL as a probability calculator" case
   study (§1bis).** Demonstrator and acceptance target for (1), spanning
   both carriers.
3. **Materialised *re-based* discrete posterior (MayBMS-style, §3 /
   §1bis).** Store a conditioned `uuid` as a fresh independent
   representation so it can re-enter relational algebra (join/union).
   Deferred until a persisted posterior must be composed onward.
4. **Soft / weighted conditioning (§5).** *Not a priority* (explicit
   decision): hard conditioning only. Kept as background; later a
   parameter on the same gate, no new mechanism.
5. **Shapley over evidence (§6.B.4).** Research track; connecting code
   over (1) plus existing Shapley infrastructure.

## Implementation observations

- The MarkoViews reduction shows the discrete *evaluation* needs no new
  machinery: `P(Q | C) = P(Q ∧ C) / P(C)` is two existing evaluations and
  a division, and that is exactly what `probability_evaluate` does for a
  `gate_conditioned(Q, C)` internally. The gate itself is still added for
  the `uuid` carrier (§1bis) – not to evaluate a one-shot `P(Q|C)`, but
  so the operator's result is a storable, re-conditionable token. It is
  the **terminal** kind (§1bis "terminal discrete gate"): non-composable
  with the semiring gates, foldable only under further conditioning. The
  thing to resist is a *composable* discrete conditioning gate that would
  bury the global `P(C)` normalization under `plus` / `times`.
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
