# Probability evaluation: d-trees, the HAVING trichotomy, the method catalog

## Scope

This note tracks the **remaining** work on ProvSQL's probability-evaluation
method selection. It consolidates two earlier plans now that most of both has
landed:

- the **anytime interval-bounds d-tree** (Olteanu, Huang & Koch, *"Approximate
  Confidence Computation in Probabilistic Databases,"* ICDE 2010);
- the **HAVING trichotomy** (Ré & Suciu, *"The trichotomy of HAVING queries on
  a probabilistic database,"* VLDB J. 18:1091–1116, 2009; conference version
  DBPL 2007),

both feeding the **method catalog + three-path chooser** (the portfolio +
cost-model idea of ProApproX, Souihli & Senellart, ICDE 2013).

## What has landed (context, not work)

The framework and most of its members are built; this section is pointers so the
open items below are self-locating. The authoritative description of the built
machinery is `doc/source/dev/probability-evaluation.rst`.

- **Method catalog + three-path chooser.** `src/ProbabilityMethod.h` + the
  catalog in `src/probability_evaluate.cpp`: a `ProbabilityMethod` Strategy
  registry, the `exact` / `relative` / `additive` tolerance grants with the
  nested admissibility lattice **exact ⊂ relative ⊂ additive**
  (`toleranceAdmits`), and `chooseAndRun` picking the cheapest admissible member
  by `estimatedCost`. The approximate methods (`monte-carlo`, `karp-luby`,
  `stopping-rule`) are first-class portfolio members; a `δ=0` deterministic
  request filters to `isDeterministic()` members. Sampler cost constants are
  benchmarked (`kCostMonteCarlo`, `kCostStoppingRule`, `kCostKarpLuby`).
- **d-tree engine.** `src/DTree.{h,cpp}` (`dtreeBounds`) + the `dnfBounds` leaf
  bound (Fig. 3) + `probability_bounds(token)` SQL; `DTreeBoundsMethod` in the
  catalog (exact by name, an optional `epsilon=` arg giving an additive
  *certified* interval with `δ=0`, in the default chain via a `δ`-independent
  cost). Memoised over the shared DAG. Studio exposes it with an optional ε
  control rendering the certified value interval. Its niche: high-treewidth
  exact (where tree-decomposition bails) plus the deterministic / low-`δ`
  approximate paths.
- **HAVING marginal-vector evaluators.** MIN / MAX / SUM / COUNT / AVG closed
  forms (`src/{MinMax,Sum,Count}CmpEvaluator`, `src/AggMarginalEvaluator`) over
  arbitrary-depth hierarchical joins plus the safe cross-product node, all
  self-gating on circuit laminarity; the `safe` / `apx-safe` / `#P-hard`
  classifier (`src/classify_having.c`, GUC `provsql.classify_having`);
  COUNT(DISTINCT) via the GROUP-BY rewrite + `runCountCmpEvaluator`. Independent
  read-once UNION/EXCEPT contributors already resolve.
- Shared per-world cmp value core (`agg_cmp_holds_in_world` in
  `subset.{hpp,cpp}`); RV MC-fallback transparency NOTICE; `SieveMethod`.

The rest of this note is **open work only**.

## The admissibility frame (the one invariant the open items share)

The three user-facing paths are tolerance grants, not algorithm picks; the
chooser returns the cheapest *admissible* member. A method is admissible iff its
guarantee is at least as tight as the request, and the sets nest:

- *exact* — `(0,0)`, the true probability;
- *relative* — `(1±ε)p`, confidence `1−δ`;
- *additive* — `|p̂−p| ≤ ε`, confidence `1−δ`; Hoeffding `N = O(ln(1/δ)/ε²)`,
  **independent of `p`**, so the robust choice for rare events.

Exact satisfies every tolerance (so it is admissible on every path and wins on
cost when the circuit is easy: "exact when cheaper" is the lattice, not a special
case). Every new method below must declare its `guaranteeKind` / `isDeterministic`
/ `applicable` / `estimatedCost` so the chooser places it correctly.

## Open work

### 1. d-tree on arbitrary circuits (drop the DNF restriction)

The engine recurses only on **monotone-DNF** clause-support sets today
(`requiredFeatures() = {DnfShape}`). Generalise it to the full Boolean-circuit
surface:

- **Read `⊗` / `⊙` off the gate structure directly** rather than from flat
  clauses: a `plus` (resp. `times`) gate whose children have **disjoint
  transitive input cones** is an independent-OR (resp. independent-AND). This
  also lands the **`⊙` independent-AND factoring** deferred in Phase 1 (on flat
  clause sets it needed Brayton/world-set factoring; on the gate DAG it is read
  off `times` structure). The read-once case stays handled by the existing
  `independent` method; `⊙` is the partial-factoring generalisation.
- **Handle `NOT` / `monus`.** The leaf-bound monotonicity argument (sub-disjunction
  lower bound, union upper bound) does not survive negation, so a non-monotone
  leaf needs either one extra Shannon expansion toward monotone leaves, or a
  coarser fallback bound.
- **Non-DNF leaf bound.** The three decompositions, the propagation and the
  stopping tests are representation-agnostic; only the leaf bound is DNF-specific.
  A general leaf needs a substitute bound (Shannon-expand one more variable, or a
  coarser interval).

Decompositions and probabilities, for reference:

| d-tree node | ProvSQL reading | probability |
|---|---|---|
| independent-or `⊗` | `plus` gate, children with **disjoint input cones** | `1 − ∏(1 − P(Φᵢ))` |
| independent-and `⊙` | `times` gate, children with **disjoint input cones** | `∏ P(Φᵢ)` |
| Shannon `⊕` on x | restrict input `x`: `Φ ≡ ⊕_a {x=a} ⊙ Φ|_{x=a}` | `Σ P(x=a)·P(Φ|_{x=a})` |

Soundness anchors that must hold at every step (the general case re-imposes
exactly these):

1. **Bound validity** `L ≤ P(Φ) ≤ U` at every node (Prop. 5.1, 5.4). Property
   test: random circuits, assert `L ≤ exact ≤ U`.
2. **Independence with sharing** — two subcircuits are independent iff their
   **transitive input cones are disjoint** (ProvSQL shares gates, so this is over
   the input footprint, not immediate children). Reuse the `seen`-set logic of
   `independentEvaluationInternal`, or the `FootprintCache` / `getJointCircuit`
   oracle from the continuous-RV path. A false independence claim is a silent
   correctness bug.
3. **Stopping soundness** — the additive (`U−L ≤ 2ε`) and relative
   (`(1−ε)U ≤ (1+ε)L`) tests are exact; never round them.

Cost open question: the treewidth proxy **mispredicts** d-tree cost (high-`w`
cliques collapse under Shannon + subsumption and run fast; low-`w` cycles do
not), so the current `δ`-independent cost deliberately avoids it. No cheap static
feature obviously predicts whether the DNF collapses, which is what running it
tells you; speculative execution (a bounded attempt) may be the only honest plan.

### 2. The SUM-safe FPTRAS (`AggFptrasMethod`)

The apx-safe class of the trichotomy: SUM (and the MIN/MAX hard direction) over a
**safe skeleton** has an FPTRAS where the exact engine bails (`kMaxSumSupport`,
the Remark-3 pseudo-poly cap). This is the principled replacement for that bail.
Gated by `safe_query_skeleton_is_hierarchical` (`src/safe_query.c`, the read-only
detector built but so far **unused** — this is what makes it load-bearing); an
unsafe skeleton is hazardous (no FPTRAS, warn and fall back to additive MC).

Thm 9 / Alg 6.3.1, three pieces:

1. *Rounding.* `τ^R(y) = ⌊(n²/k)·y⌋`, rounded-sum semiring `S_{n²+1}` (size `n²+1`,
   polynomial). Compute the rounded-sum PMF **exactly** with the marginal-vector
   recursion already built (`sumPMF` / `countPMF`, well under `kMaxSumSupport`),
   driven by the query-level safe plan from `find_hierarchical_root_atoms`.
2. *Random-world generator* (Alg 5.2.1, the one genuinely new subroutine):
   `sampleWorldWithValue(plan_node, target_s)` walks the safe-plan tree
   **top-down**, splitting `target_s` among children ∝ marginal-vector entries
   (`⊕`: pick `s₁+s₂=s` w.p. `m^φ₁[s₁]·m^φ₂[s₂]/m^φ[s]`; `⊗`: `s₁·s₂=s`; `⊔`:
   route all to one branch). **This is the bottom-up `countPMF` / `sumPMF` /
   `decomposeProduct` recursion run in reverse** (same tree, same vectors, the
   draw descends instead of folding up). Alg 5.2.2 fills off-plan tuples.
3. *Accept-test* (Lemma 7): draw a rounded value `∝ μ(rounded)`, draw a world,
   accept iff it is an *original* solution; `m = O(n·β⁻¹·ε⁻²·log δ⁻¹)` samples →
   relative `(ε,δ)`.

New code `src/AggFptras.{h,cpp}`, consuming `AggMarginalEvaluator` internals
(needs them exposed) + `safe_query.c`. The `(α,θ)` × skeleton admissibility map:

| `(α, θ)` | safe skeleton | unsafe skeleton |
|---|---|---|
| `MIN ≤/<`, `MAX ≥/>` | FPTRAS (karp-luby) | **FPTRAS (karp-luby)** — gate-independent (`MAX≥k ⟺ ∃ present tuple, y≥k`, a UCQ; Thm 8) |
| `MIN ≥/>`, `MAX ≤/<` | FPTRAS (safe-plan) | hazardous |
| `SUM <,≤,≥,>` | FPTRAS (rounding+sampling) | hazardous |
| `SUM =,≠`, `AVG =,≠` | hazardous | hazardous |
| `COUNT` (any) | exact (dichotomy) | #P-hard; relative-MC if `p ≥ 1/poly` |

**Research-grade; high risk of subtle statistical bias.** Deferred for
collaborative implementation / review rather than autonomous coding.

### 3. HAVING marginal-vector engine: the residual shapes

The laminar / cross-product engine covers COUNT / SUM / MIN / MAX / AVG at
arbitrary hierarchical depth. The genuine residuals:

- **Branch-spanning SUM** — a value depending on more than one product factor
  (the current product node bails, since it factors `S_f · M` only when the
  weight is constant within each factor part). Needs per-factor joint
  `(sum, count)` distributions; a branch-spanning value may be `#P`-hard, so this
  must self-gate.
- **BID disjoint-block `⊥`** — mutual exclusion from a key constraint is a
  *semantic* fact that need not surface as circuit leaf-sharing, so it is the one
  genuinely **certificate-only** structure (the circuit footprint oracle cannot
  see it). Open spike: how BID-`⊥` block membership surfaces in the loaded
  circuit (repair_key / mutually-exclusive inputs). If needed, bake a
  `CERT_SAFE_AGG_PLAN` blob (the `find_hierarchical_root_atoms` block structure,
  currently discarded) onto the `gate_agg` at the HAVING-lift site
  (`having_Expr_to_provenance_cmp` in `src/provsql.c`, append-only via
  `src/safe_query_cert.{c,h}`), carried through `CircuitFromMMap`.
- **UNION/EXCEPT over a join that re-uses a base tuple** — `(R⋈S) UNION (R⋈T)` →
  `(r∧s)⊕(r∧t)`, non-read-once on the shared `r`. Needs per-contributor
  read-once factoring (`r∧(s∨t)`), the safe-query / read-once-rewriter problem,
  `#P`-hard in general.

### 4. Method-catalog follow-ups

- **Lazy Boolean build.** RV / HAVING circuits with no Boolean view currently
  fall to a small top-level estimator outside the catalog. A true lazy Boolean
  build would fold even those into `chooseAndRun` so all three paths are
  catalog-driven uniformly.
- **`MinMaxKarpLubyMethod`** (thin) — MIN/MAX easy direction (`MAX≥k` / `MIN≤k`)
  reduces to `P(∃ present tuple with y θ k)`, a monotone UCQ; hand the existing
  coverage FPRAS that small DNF, on **any** skeleton (Thm 8). The cheapest
  relative win; functionally already covered by `stopping-rule`, so this is an
  efficiency optimisation.
- **`MulMcMethod`** — additive MC made multiplicative via a structural lower
  bound `ℓ` (`ε_add = ℓ·ε`), cheaper than coverage when `p ≈ 1`. Needs a
  structural `ℓ` on the unexpanded circuit (the missing feature).
- **Gate-level `eval_cmp_in_world` — resolved as a non-issue (dropped).** This
  was proposed as parity hardening against drift between the `MonteCarloSampler`
  agg/cmp arm and the `pw_from_cmp_gate` expansion. Investigation showed there is
  no live drift to harden against: discrete `gate_cmp`s are **expanded into their
  threshold lineage by `provsql_having` inside `getBooleanCircuit` before any
  sampler runs**, so the Boolean-path methods never see a symbolic cmp, and the
  one sampler that does (`run_stopping_rule` on the `GenericCircuit`) agrees with
  the expansion empirically. The decisive check (per Senellart): encode the same
  distribution as a categorical RV and as a `choose()` aggregate over a
  mutually-exclusive `repair_key` block — they match on every operator, arity and
  non-uniform mass. So the two cmp paths are consistent and the shared primitive
  is unnecessary.
  *The investigation did surface a real, unrelated bug, now fixed:* the SUM
  world-enumeration (`sum_dp`) dropped the value-`0` worlds along with the empty
  world for `<` / `<=`, so `HAVING sum(val) < k` over a BID block containing a
  value-`0` outcome returned the wrong probability (e.g. `0` instead of `0.5`).
  Fixed by keeping `dp[0]` and removing only the empty mask; pinned by
  `test/sql/having_sum_zero.sql`. COUNT is structurally immune (no value-`0`
  contributor).
- **Cost-model refinements.** The `karp-luby` `S·m` cost is pessimistic for large
  `m`; the calibrated per-node ε-split (guarantee propagation, below) is the
  cost-model-later seam.
- **Guarantee propagation** (decompose the whole-query `(ε,δ)` at
  **independence-certified gates only**): independent-OR `ε ≈ max(ε₁,ε₂)`,
  mutex-OR `max`, independent-AND `ε₁`; `1−δ = (1−δ₁)(1−δ₂)`. A `plus` whose
  children **share leaves** is not independent-OR, so the unit is a *maximal
  correlated component* (correlated cmps stay together and are sampled jointly;
  independent structure composes exactly). An exactly-cleared component
  contributes `ε=0`, loosening the budget for approximate siblings (the per-part
  payoff). **`monus` propagation is not in the paper** — derive the
  `Pr(monus(a,b)) = Pr(a)(1−Pr(b))` ε-propagation and its unsound-under-sharing
  caveat.
- **CircuitFeatures tier-2** — the treewidth proxy exists (`tw_proxy_`);
  independence certificates remain to be cached lazily. Optional: expose a
  `provsql.methods` SQL view (name, guarantee kind, applicability) for Studio.

### 5. RV probability transparency enrichments

- Route the RV *probability* case `P(X<c)` through `stopping-rule`.
- Decide always-on vs the current verbose-gated MC-fallback NOTICE (a one-line
  change in `Expectation.cpp` `mc_samples_or_throw`).

### 6. d-tree research polish

- **Tractable variable-elimination order** (Olteanu-Huang-Koch Sec. VI-B,
  Lemma 6.8) for a guaranteed poly-size d-tree on hierarchical / `IQ` queries.
  The circuit has lost the query structure, so this means rediscovering a good
  order (the paper's SPROUT-vs-dtree gap); defer until the frequency heuristic is
  shown insufficient.
- **Cheaper memo keys** (a hash / fingerprint instead of `map<vector<set>>`) and
  the `O(depth)` leaf-closing form (Sec. V-D). Constant-factor polish; the
  current keys are why the memoised d-tree is still ~2× behind tree-decomposition
  on bounded treewidth.
- **Paper benchmark.** `test/bench/dtree_bench.sql` exercises the full portfolio;
  the one shape not yet reproduced is the paper's social-network experiment (the
  **triangle** and **path-of-length-2** queries over a random graph of
  tuple-independent edges, relative ε = 0.01) — d-tree should win by orders of
  magnitude at high edge probabilities and track the sampler at small ones.

### 7. Bibliography

`website/_bibliography/references.bib` has the surrounding probabilistic-DB canon
(`DalviS12`, `JhaS11`, `AmsterdamerDT11`, `GatterbauerS15`, the 2011 Suciu et al.
textbook) but not the three papers this note is anchored on. Add, when the
relevant item lands:

- Olteanu, Huang & Koch 2010 (ICDE) — the d-tree;
- Ré & Suciu 2009 (VLDB J.), optionally the DBPL 2007 conference version — the
  HAVING trichotomy;
- Souihli & Senellart 2013 (ICDE) — ProApproX, the portfolio + cost-model
  principle behind the method catalog.

## Implementation observations

- The pre-pass slot, sound-only contract (numeric Bernoulli `gate_input`,
  meaningless to symbolic semirings), and `ref_count`-style privacy gating are
  established by `runAnalyticEvaluator` and `runCountCmpEvaluator`; new
  per-aggregate evaluators should reuse that contract and the
  `provsql.cmp_probability_evaluation` umbrella GUC, not invent a new path.
- The absorptive-vs-non-absorptive split in `pw_from_cmp_gate` (`S.absorptive()`)
  is the correct gate for the *symbolic* closed forms; the marginal-vector
  probability forms are a parallel probability-only track and must not disturb the
  semiring evaluation that non-probability semirings rely on.
- Reference write-ups: the chosen HAVING *semantics* (`pw_from_cmp_gate`) is
  Aryak's `algorithms.tex`; this note is the *probability-complexity* companion.
  The built machinery is documented in
  `doc/source/dev/probability-evaluation.rst`; the Boolean-only and safe-query
  follow-ups it shares a border with are in
  [`safe-query-followups.md`](safe-query-followups.md).
