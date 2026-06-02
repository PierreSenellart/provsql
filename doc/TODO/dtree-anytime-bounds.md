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

### Phase 0 — the leaf-bound primitive (low risk, immediately useful) — **LANDED**

- `BooleanCircuit::dnfBounds(const vector<set<gate_t>> &clauses, L, U)`, the Fig. 3
  bucket heuristic. O(m²) in the clause count. Takes the per-clause supports
  (a monotone DNF is determined by them), so the `DTree` recursion reuses it on
  cofactor clause sets.
- Exposed at SQL as `probability_bounds(token) -> (lower, upper)` (the `prov`
  conditioning arg is deferred — it needs interval division). Clauses enumerated
  via `dnfShape`.
- Test `test/sql/probability_bounds.sql`: read-once collapses to exact, shared-
  variable interval brackets exact, non-DNF errors.

### Phase 1 — the DNF-restricted d-tree engine — **LANDED**

- `src/DTree.{h,cpp}`: `dtreeBounds(const BooleanCircuit&, vector<set<gate_t>>
  clauses, double max_width) -> {lower, upper}`. Recursion over clause-support
  sets: `⊗` (connected components of the clause graph) then `⊕` (Shannon on the
  most-frequent variable), with subsumption removal and the cheap leaf bound as
  the early-stop test. The width budget propagates soundly: unchanged through
  `⊕`, `max_width/k` to each of `k` `⊗` components.
- **`⊙` (independent-AND factoring) deferred.** On flat clause sets it needs
  Brayton/world-set factoring; `⊗`+`⊕` is already correct and complete without
  it, and the fully-factorable (read-once) case is handled by the existing
  `independent` method. `⊙` is a Phase-3 win (read off `times`-gate structure
  once the engine works on the circuit directly rather than flat clauses).
- `DTreeBoundsMethod` registered with `guaranteeKind() = Exact`,
  `requiredFeatures() = {DnfShape}`, **`inDefaultChain() = false`** (by-name only
  in Phase 1, so it does not perturb the calibrated auto-chooser). By name
  `'d-tree'` is exact; an optional `epsilon=` arg runs it additive with a
  deterministic certificate (`delta = 0`). The relative branch is wired but only
  reachable once Phase 4 puts it in the relative-path portfolio.
- Tests `test/sql/dtree.sql`: exact equals `possible-worlds`/`sieve` on an
  entangled (4-cycle) circuit and on a read-once one; additive `epsilon=0.05`
  within ε of exact; non-DNF declined.

### Phase 2 — memoisation (DAG d-tree) — **LANDED**

The Phase-1 recursion, being a *tree* (no sharing), recomputes overlapping
subproblems. `DTreeContext` now memoises subproblems, keyed on the canonical
(subsumption-reduced, sorted) residual clause set, mapping to the subproblem's
exact probability — written only on an exact request (`max_width == 0`, where the
whole recursion is exact), read always. The recursion is now a shared DAG.

**Measured outcome** (cycle-160, the worst Phase-1 case): exact 38 ms → **17 ms**,
matching tree-decomposition to 10 decimals (pinned by `dtree.sql`'s `cyc_eq_td`).
So memoisation gives correctness-preserving ~2× and, more importantly, guards
against the exponential blow-up a non-memoised tree suffers on structures with
repeated subproblems.

**But it does not make the d-tree beat tree-decomposition on bounded treewidth**
— still ~2× behind (17 vs 8 ms), a constant-factor gap from the expensive
clause-set keys; tree-decomposition is purpose-built for that regime. The
re-confirmed conclusion: the d-tree's *exact* niche is **high treewidth (`w >
MAX_TREEWIDTH`), where tree-decomposition bails entirely** (e.g. clique-16:
d-tree 1.3 ms vs compilation ~40 ms / possible-worlds 2^16), plus the approximate
paths. Cheaper memo keys (a hash/fingerprint instead of `map<vector<set>>`) and
the O(depth) leaf-closing form (Sec. V-D) are deferred polish.

### Phase 3 — generalise to arbitrary circuits

Drop the `DnfShape` requirement. Read `⊗`/`⊙` off the gate structure directly
(disjoint input cones); handle `NOT`/`monus`; for a non-DNF leaf, either Shannon-
expand one more variable toward DNF leaves, or use a coarser fallback bound. This
makes the engine apply to the full Boolean-circuit surface, not just SPJU lineage.

### Phase 4 — cost model + chooser integration — **LANDED (via deterministic admissibility)**

The settled design was: keep `guaranteeKind() = Exact` (admissibility) and govern
selection by a **tolerance-aware cost** — pessimistic on exact (~`S·2^w` via the
treewidth proxy), decreasing in ε on the approximate paths. The Phase-4
measurement (`d-tree` by name vs the competitors, this machine, `p = 0.3`)
**overturned the cost half of it**:

| circuit | w | exact d-tree | exact best other | ε=0.1 d-tree | ε=0.1 other |
|---|---|---|---|---|---|
| cycle-160 (pairwise) | 2 | 38 ms | tree-decomp **8 ms** | **1.2 ms** | stop-rule 1.6, karp-luby **52** |
| clique-16 (all pairs) | 15 | **1.3 ms** | poss-worlds 2^16 / compile ~40 | 1.3 ms | — |

1. **The treewidth proxy mispredicts the cost.** High-`w` cliques *collapse*
   under Shannon + subsumption and run fast; low-`w` cycles do not and run slow.
   So `S·2^w` is backwards; degeneracy is not a usable predictor, and no other
   *cheap static* feature obviously is (the driver is whether the DNF collapses,
   which is what running it tells you).
2. **Exact is dominated** by tree-decomposition — the no-memoisation limitation,
   hence Phase 2 is promoted ahead of this phase.
3. **Approximate is only marginally ahead** of the stopping rule (1.2 vs 1.6 ms),
   though it *does* crush `karp-luby` (52 ms) on a high-clause DNF. But
   `karp-luby`'s own cost model predicts ~1 ms for that 52 ms run — it is badly
   miscalibrated too, so the approximate-path chooser is already unreliable
   independently of the d-tree.

**The integration key was not cost at all — it was admissibility on `delta`.**
The samplers are `(eps,delta)` with a sample count ∝ `ln(1/delta)`, so:

- **`delta == 0` (deterministic request)**: the samplers are *infeasible* and the
  d-tree (a certified interval) is the only non-exact method that can serve it.
  Modelled as a new `ProbabilityMethod::isDeterministic()` (the samplers override
  it to `false`); `chooseAndRun` drops non-deterministic methods when
  `tol.delta == 0`. Parsing now accepts `delta = 0` on the `relative`/`additive`
  paths (a by-name sampler with `delta = 0` errors). The RV/HAVING fallback, which
  has only samplers, errors on `delta = 0`.
- **`delta` small but > 0**: the samplers stay admissible but their *cost* grows
  as `ln(1/delta)` (already in their `estimatedCost` — and the stale
  `: 0.5` floor that masked this was removed), while the d-tree's cost is
  **`delta`-independent**. So the chooser migrates to the d-tree as confidence
  tightens, on cost, with no need to predict collapse.

So the d-tree is now `inDefaultChain() == true` with a `delta`-independent cost:
approximate `≈ kCostDTreeApprox · S / eps` (the anytime stop caps the work, and
the treewidth proxy is deliberately NOT used — it mispredicts), exact
`≈ kCostDTreeExact · S · m` (pessimistic, so tree-decomposition wins low treewidth
and the d-tree is picked for exact only where tree-decomposition's cap is
exceeded). Net selection: deterministic / low-`delta` approximation → d-tree;
high-treewidth exact → d-tree; everything the cheap exact methods resolve →
exact-when-cheaper, unchanged. Pinned by `dtree.sql` (`det_within_eps`,
`det_not_sampler`, and the by-name-sampler-`delta=0` error).

Still open: the `karp-luby` / sampler cost *constants* remain miscalibrated
(karp-luby ~14× under, stopping-rule ~6× over on the measured cases), so the
`delta > 0` competition among the approximate methods is only roughly right — a
dedicated sampler-cost calibration pass is the remaining work, independent of the
d-tree.

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
