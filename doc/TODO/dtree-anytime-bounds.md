# Anytime interval-bounds probability (Olteanu–Huang d-trees)

## Intro

This note plans the addition of a **deterministic, anytime, interval-bounds**
probability engine to ProvSQL, following Olteanu, Huang & Koch, *"Approximate
Confidence Computation in Probabilistic Databases"* (ICDE 2010), the d-tree /
SPROUT line of work.

The motivation is concrete and measured, not speculative. The paper's baseline
`aconf` is the **Karp-Luby unbiased estimator wrapped in the Dagum–Karp-Luby-Ross
optimal stopping rule** — that is *exactly* what ProvSQL's `karp-luby` and
`stopping-rule` methods are (`src/BooleanCircuit.cpp::karpLuby` /
`karpLubyStopping`). Across tractable and hard TPC-H queries, random graphs and
social networks, the deterministic d-tree **beats `aconf` by several orders of
magnitude**, and on hard small-probability queries `aconf` finished only 4 of 12
runs while the d-tree finished all. ~90% of the d-tree's nodes are independence
nodes: the win is from exploiting independence the samplers ignore.

The d-tree is therefore the highest-value addition to the relative/additive
paths of the method catalog (`doc/TODO/having-trichotomy.md`,
`src/probability_evaluate.cpp`): it competes directly with our weakest pieces
(the samplers), it *generalises machinery we already have but use all-or-nothing*,
and unlike the samplers it returns a **certificate** (a proved interval, no δ).

## What ProvSQL has, and the gap

- `BooleanCircuit::independentEvaluation` is the independent-or / independent-and
  case, but **all-or-nothing**: read-once or it throws. There is no partial
  decomposition, no "peel the independent frontier, recurse on the residual".
- `TreeDecomposition` / `dDNNFTreeDecompositionBuilder` is a **bottom-up, exact**
  compiler: no early stop, no interval.
- The relative/additive paths fall to `stopping-rule` / `karp-luby` / `monte-carlo`
  — sampling, with a δ failure probability and no certificate.

The d-tree unifies all of this: independence + Shannon expansion + a cheap leaf
bound + an early-stop test. Exact is "run to `L = U`"; approximate is "stop when
the interval meets the tolerance". It is the *anytime generalisation* of
`independentEvaluation`.

## The algorithm in ProvSQL terms

A circuit is compiled top-down into a **d-tree** whose inner nodes are three
decompositions and whose leaves are DNFs:

| d-tree node | ProvSQL reading | probability |
|---|---|---|
| independent-or `⊗` | a `plus` gate whose children have **disjoint input cones** | `1 − ∏(1 − P(Φᵢ))` |
| independent-and `⊙` | a `times` gate whose children have **disjoint input cones** | `∏ P(Φᵢ)` |
| Shannon `⊕` on x | restrict an input gate: `Φ ≡ ⊕_a {x=a} ⊙ Φ|_{x=a}` | `Σ P(x=a)·P(Φ|_{x=a})` |

Note ProvSQL's `plus`/`times` circuit *already exposes* `⊗`/`⊙` structurally —
independence a flat DNF must factor to discover is read straight off the gates,
provided we account for **gate sharing** (a shared input in two children breaks
independence; this is the soundness crux, see below).

**Leaf bound** (the one DNF-dependent piece, Fig. 3 of the paper). For a leaf DNF
Φ = ∨ of clauses:

- partition the clauses into buckets of **pairwise-independent** clauses
  (greedy, clauses sorted by descending marginal probability);
- each bucket is computed exactly, `P(Bᵢ) = 1 − ∏_{d∈Bᵢ}(1 − P(d))`,
  `P(d) = ∏` marginals of the clause's literals;
- `L = maxᵢ P(Bᵢ)` (a sub-disjunction is a lower bound);
- `U = min(1, Σᵢ P(Bᵢ))` (union bound).

Bounds propagate to the root by monotonicity (plug children's `L`s for the node
`L`, children's `U`s for the node `U`).

**Stopping** (Prop. 5.8), checked in one pass:

- additive ε: stop when `U − L ≤ 2ε`; return any point in `[U−ε, L+ε]`;
- relative ε: stop when `(1−ε)·U ≤ (1+ε)·L`; return any point in `[(1−ε)U, (1+ε)L]`;
- exact: run until `L = U`.

**The DNF requirement is confined to the leaf bound** (the sub-disjunction lower
bound and the union upper bound). The three decompositions, the propagation and
the stopping tests are representation-agnostic. So Phase 1 gates on the existing
`DnfShape` feature; the general-circuit version (Phase 3) needs a substitute
bound for non-DNF leaves (one extra Shannon expansion toward DNF leaves, or a
coarser bound).

## Soundness anchors (must hold at every step)

1. **Bound validity**: `L ≤ P(Φ) ≤ U` at every node (Prop. 5.1, 5.4). Property
   test: random circuits, assert `L ≤ exact ≤ U`.
2. **Independence with sharing**: two subcircuits are independent iff their
   **transitive input cones are disjoint**. ProvSQL circuits share gates, so this
   must be computed over the input footprint, not the immediate children. Reuse
   the `seen`-set logic of `independentEvaluationInternal` / a footprint cache;
   a false independence claim is a silent correctness bug.
3. **Stopping soundness**: the two conditions above are exact; never round them.
4. **Monotone-only in Phase 1**: the leaf-bound monotonicity argument assumes a
   monotone DNF (no `NOT`/`monus` inside the leaf). `DnfShape` over positive
   provenance already gives this; Phase 1 refuses circuits with negation in a
   leaf. Phase 3 handles the general case.

## Implementation plan

### Phase 0 — the leaf-bound primitive (low risk, immediately useful)

- `BooleanCircuit::dnfBounds(gate_t root, double &L, double &U)` (new), the Fig. 3
  bucket heuristic. O(m²·clause) in the clause count m.
- Reuse `dnfShapeInfo` to enumerate clauses; pairwise-independence = disjoint
  input sets.
- Expose at SQL as `probability_bounds(token, prov DEFAULT gate_one()) ->
  (lower, upper)`: a cheap certified interval, useful on its own.
- Tests: `test/sql/probability_bounds.sql` (validity `L ≤ exact ≤ U` on the
  read-once and ladder circuits already in `last_eval_method.sql`); a unit check
  that `L = U = exact` on read-once.

### Phase 1 — the DNF-restricted d-tree engine

- `src/DTree.{h,cpp}`: `dtreeApprox(const BooleanCircuit&, gate_t root, Tolerance)
  -> {value, L, U}`. Recursion: try `⊗` (connected components of the clause graph),
  then `⊙` (independent factors), else `⊕` (Shannon on the most-frequent variable,
  the paper's general heuristic). Early-stop via the cheap leaf bound before each
  expansion.
- Straightforward recursion first (no leaf-closing); correctness over memory.
- Register `DTreeBoundsMethod : ProbabilityMethod` in `probability_evaluate.cpp`:
  - `guaranteeKind() = Exact` (run-to-closure is exact, so it is admitted on every
    tolerance path; its **cost** drops with looser ε — see cost model);
  - `requiredFeatures() = {Feature::DnfShape}`; `applicable` = `ctx.dnf_ok_` and
    monotone;
  - name `"d-tree"` so it is explicitly requestable.
- Tests: parity vs `possible-worlds` on small circuits; result within ε of exact
  for additive and relative; bound validity; a non-DNF circuit is correctly
  declined.

### Phase 2 — incremental / memory-efficient form

Only if Phase 1 shows memory pressure on large DNFs. Implement Sec. V-D / Thm.
5.12: depth-first construction, **close** leaves into aggregated bounds, keep only
the current root-to-leaf path → O(depth) memory. Lemma 5.11 bound-space check to
decide a leaf can be closed while preserving the ε-guarantee.

### Phase 3 — generalise to arbitrary circuits

Drop the `DnfShape` requirement. Read `⊗`/`⊙` off the gate structure directly
(disjoint input cones); handle `NOT`/`monus`; for a non-DNF leaf, either Shannon-
expand one more variable toward DNF leaves, or use a coarser fallback bound. This
makes the engine apply to the full Boolean-circuit surface, not just SPJU lineage.

### Phase 4 — cost model + chooser integration

- `estimatedCost(ctx, tol)`: anytime cost is convergence-dependent and hard to
  predict statically. Model heuristically from features already acquired:
  cheap when `DnfShape` ∧ low `TreewidthProxy` (independence-rich → fast
  convergence), pessimistic otherwise; **decreasing in ε** (looser tolerance
  stops sooner) and benefiting from `p` near 0/1 where bounds converge fast.
  Calibrate the constant on this machine via the existing
  `provsql.verbose_level >= 50` instrumentation, as done for the other `kCost*`.
- Wire into `chooseAndRun` so the chooser prefers `d-tree` over `stopping-rule` /
  `karp-luby` / `possible-worlds` when its estimate wins. The exact-when-cheaper
  behaviour falls out: on a tractable circuit the d-tree closes to `L=U` cheaply.

### Phase 5 — optional / research

- Tractable variable-elimination order (Sec. VI-B, Lemma 6.8) for a guaranteed
  poly-size d-tree on hierarchical / `IQ` queries. The circuit has lost the query
  structure, so this means rediscovering a good order (the paper's SPROUT-vs-dtree
  gap); defer until the frequency heuristic is shown insufficient.
- Studio: surface the certified interval `[L,U]` next to the point estimate (the
  eval strip already shows the resolved method and the sampler guarantee; a
  deterministic interval is a stronger artefact).

## Benchmark to reproduce (validates the whole effort)

Rebuild the paper's social-network experiment in `test/bench/dtree_bench.sql`:
the **triangle** and **path-of-length-2** queries over a random graph of
tuple-independent edges, evaluated with `stopping-rule` vs `d-tree` at relative
ε = 0.01. Expected: d-tree wins by orders of magnitude for high edge
probabilities and tracks the sampler even at small probabilities. This both
proves the win against ProvSQL's own samplers and feeds the Phase-4 calibration.

## Open questions / risks

- **Independence detection cost**: computing disjoint input cones per node can be
  expensive; needs a footprint cache shared across the recursion (cf. the
  `FootprintCache` in the continuous-RV evaluator).
- **Shannon blow-up on the hard core**: a monolithic high-treewidth, small-`p`,
  tight-ε circuit forces many expansions and wide bounds; the d-tree degrades to
  the inherent #P-hardness. It must be a *costed* method, chosen only when it
  wins, never a default wrapper.
- **Negation / `monus`**: deferred to Phase 3; the leaf-bound monotonicity does
  not hold through negation.
- **Relation to `makeDD` / tree-decomposition**: complementary, not a
  replacement. Bounded-treewidth exact → tree-decomposition; independence-rich /
  anytime / loose-ε → d-tree. The chooser arbitrates on cost.
