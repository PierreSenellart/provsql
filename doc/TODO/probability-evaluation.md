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

**Soundness of the current monotone-DNF engine audited (correct).** The three
decompositions and their bounds were re-checked against Olteanu-Huang-Koch: the
independent-OR child combination `{1 - prod(1-L_i), 1 - prod(1-U_i)}` brackets
`P` correctly (monotonicity of `1 - prod(1-.)`), the Shannon mixture propagates
bounds linearly so the interval width never exceeds the larger branch's, the
leaf bound's `L = max bucket prob` / `U = min(1, sum bucket probs)` is the
sound greedy disjoint-support partition, the additive (`U - L <= 2*eps`) and
relative (`max_width = 2*eps*L0`) stopping tests are exact, and the memo is
written only on the fully-exact recursion (so a cached value is reusable for any
later width target). Independence is decided by disjoint clause supports
(union-find), never overclaiming. Pinned by `test/sql/probability_bounds.sql`
(brackets the exact value, errors cleanly off-DNF) and `test/sql/dtree.sql`
(equals the exact baselines; within `eps` of exact). No change needed here.

**Dropping the DNF restriction -- DONE** (`dtreeBoundsCircuit`, `src/DTree.cpp`).
The anytime engine now recurses on the Boolean-circuit DAG (`AND` / `OR` / `NOT`
/ `IN`) instead of a flat monotone-DNF clause set, so it handles negation
(`EXCEPT` / `monus`, encoded `A AND NOT B`), nested `AND` / `OR` (CNF), and
arbitrary sharing. All three sub-items below are addressed:

- **`⊗` / `⊙` off the gate structure** -- `genComponents` partitions an `AND` /
  `OR` gate's children by disjoint free-variable footprint (union-find over the
  input cones); independent groups compose exactly (`∏` for `AND`,
  `1-∏(1-·)` for `OR`).  This lands the `⊙` independent-AND factoring.
- **`NOT` / `monus`** -- handled by the exact flip `[1-U, 1-L]` (bound) and
  `Pr(¬g) = 1-Pr(g)` (Shannon), so no monotonicity assumption is needed.
- **General leaf bound** -- the cheap bound generalises `dnfBounds` soundly to
  any gate: independent components compose exactly, and *within* an entangled
  component `AND` uses a Bonferroni lower / `min` upper and `OR` a `max` lower /
  union upper (both valid under arbitrary dependence), with the `NOT` flip.

Exact (Shannon + component decomposition + memo on the canonical
`(op, child-set, restricted-assignment)` subproblem) and anytime (refine until
`U-L <= max_width`) both validated against `possibleWorlds` on a structured
battery and 400 random `AND`/`OR`/`NOT` circuits (0 exact mismatches, 0 bound
violations); pinned by `test/sql/dtree.sql` (CNF + negation == possible-worlds).

Wired into `DTreeBoundsMethod`: applicable to any circuit; a monotone DNF keeps
the optimised clause fast path, everything else takes the general recursion.
**Remaining (deliberately deferred, a cost-model item):** non-DNF *exact*
auto-selection is costed `∞`, so tree-decomposition / d4 / possible-worlds stay
preferred for exact and the general recursion is reached only by name or on the
approximate / `delta=0` paths (where it competes on the delta-independent
anytime cost -- e.g. it now wins the 24-input CNF relative case in
`probability_paths.sql`).  A multivalued (`MULIN` / BID) circuit makes the
general recursion throw, so the chooser falls back -- extending it to
`gate_mulinput` is the other open piece.

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

Cost open question -- **resolved by speculative execution (DONE for the d-tree).**
A calibration sweep over the random-circuit bench (200 circuits; eval-only ms vs
`S`, `N`, depth, for `eps = 0` and `eps > 0`) confirmed empirically that **no
cheap static feature predicts d-tree cost**: depth is the *weakest* predictor
(corr ~0.15, ~0.32 even at fixed `N`, dominated by `S`); the only decent fits are
`S·2^N` (exact, R²≈0.69) and `S·N` (approx, R²≈0.47), and those hold only because
random circuits have treewidth ≈ `N` -- they badly over-predict structured
low-treewidth circuits (the true driver is `2^treewidth`). The sweep also showed
the approximate cost is **nearly `eps`-independent** (median `e(0.01)/e(0.1)` ≈
0.9, not 10), so the old `cost ∝ 1/eps` overstates `eps`-sensitivity by an order
of magnitude. Since the driver (treewidth) is not cheaply predictable, the cost
model is *not* the right lever; instead the d-tree now runs under a **subproblem
budget** (`src/DTree.cpp`: a counter in `recurse` / `genRefine` / `genRefineGroup`
that throws `"d-tree: cost budget exceeded"` on overrun). The chooser
(`chooseAndRun`) sets the budget to the **next-best admissible method's estimated
cost** (`EvalContext::cost_budget`, converted to a subproblem count via
`kCostDTreeMsPerStep ≈ 5e-4` ms/subproblem); on overrun the existing
`catch(CircuitException)` drops the d-tree and escalates -- so wasted work is
bounded at ~the safe fallback's cost (an `O(1)`-competitive "try cheap, escalate
on blow-up" portfolio). Deterministic (subproblem count, not wall-clock), so
method selection stays reproducible. Validated: the 24-input CNF in
`probability_paths.sql`, whose `S/eps` estimate rated the d-tree cheapest but which
actually does ~4·10⁵ subproblems, is now correctly escalated d-tree → tree-
decomposition. Debug GUC `provsql.dtree_max_subproblems` imposes an extra hard cap
(by-name overrun then surfaces as a clean error -- no chooser to escalate to);
pinned by `dtree.sql`.

**`tree-decomposition` budget -- DONE.** `TreeDecompositionMethod::evaluate`
builds the (poly) min-fill decomposition, then -- before the exponential d-DNNF
step -- recomputes the real cost from the *discovered* treewidth
(`td.getTreewidth()`, exact, vs the degeneracy lower bound the estimate used) and
throws if it exceeds `cost_budget`, so the chooser escalates.  The build's
`MAX_TREEWIDTH = 10` cap is still the hard ceiling; this is the competitive
refinement that defers to a genuinely cheaper method when the proxy under-rated
the width.  **`compilation` stays the terminal, un-budgeted fallback** (no
cheaper alternative to escalate to; no multi-compiler trial), per the design
decision.

**Budget calibration is per recursion path.** `kCostDTreeMsPerStepDnf` (~1.4e-3)
vs `kCostDTreeMsPerStepGeneral` (~5e-4): the monotone-DNF clause path pays an
`O(m^2)` subsumption sweep per subproblem, ~3x the general circuit path; a single
constant under-charged the DNF path and let it run well past the fallback's cost
(the `dtree_bench` `big_rare` 160-cycle did ~5e4 approx subproblems / ~70 ms).

**Known follow-up surfaced by the budget:** the d-tree's *approximate* path does
NOT memoise (only the exact path writes the memo, since an early-stopped interval
is width-dependent), so on a high-sharing circuit (a long cycle) approximate
evaluation is *slower* than exact (no DAG sharing).  The budget bails it
correctly; memoising the exact sub-results encountered during an approximate
recursion would remove the blow-up (a noted follow-up).  *(Determinism check:
the whole `dtree_bench` auto-chooser table is now bit-identical across runs --
the min-fill heap and the degeneracy peel both break ties by node id, and the
degeneracy is a graph invariant.  An earlier observed flip on `big_rare` was the
budget calibration, since fixed by the per-path `kCostDTreeMsPerStep` split; the
degeneracy peel's `unordered_set` element pick was also replaced by an ordered
`std::set` so the proxy is provably deterministic, not merely invariant.)*

### 2. The SUM-safe FPTRAS (`AggFptrasMethod`)

**Update (apx-safe corner now handled by direct sampling, all aggregates but
COUNT).** The practical gap this item targeted -- a large-magnitude /
large-support HAVING comparator that bails *both* exact evaluators (the dense
`SumCmpEvaluator`, range > `kMaxSumRange`, and the sparse marginal-vector engine,
distinct values > `kMaxSumSupport`) -- is now resolved soundly for an
`(eps,delta)` request. Such a comparator's exact route is `provsql_having`'s
threshold-lineage expansion, which does **not terminate** in practice;
`probability_evaluate` now detects the surviving comparator
(`circuitHasUnresolvedSampleableAgg`, `src/MonteCarloSampler.cpp`) and routes
`relative` / `additive` / `monte-carlo` straight to the world-sampler, whose
`gate_agg` arm pushes each kept contributor's value into the matching aggregator.
The `relative` path uses the DKLR stopping rule (`monteCarloRVStopping`) -- a
relative FPRAS when `p >= 1/poly` -- giving exactly the apx-safe guarantee.
Pinned by `test/sql/having_agg_fptras.sql` (SUM witness: 25 weights `2^0..2^24`,
`sum >= 2^24` has exact probability `1/2` yet support `2^25`, so both caps are
blown; plus AVG / MIN / MAX faithfulness checks, NULL rows included).

**Faithful for every aggregate (SUM / AVG / MIN / MAX / COUNT).** The sampler
reproduces SQL semantics for all of them, verified against the exact engine:
each kept contributor's value gate is pushed into the matching aggregator (the
summed term for SUM; the 0/1 indicator for COUNT, 0 for a NULL row so `count(x)`
does not count NULLs; the compared value for AVG / MIN / MAX), so NULL rows are
handled and an empty group finalises as the exact engine does (0 for SUM /
COUNT, NaN -> false for the others). COUNT was made faithful by the one-line fix
in the sampler's `gate_agg` arm -- add `(long)evalScalar(sm[1])` instead of the
hard `1L` -- so it now distinguishes `count(*)` (all contributors value 1) from
`count(x)` over a NULL-bearing column (the NULL rows carry value 0); pinned by
the `count(*)` / `count(x)` cases in `having_agg_fptras.sql`. In practice only
SUM / AVG / MIN / MAX ever reach the sampler: COUNT's value-support is small
(0/1 per row), so the closed-form Poisson-binomial evaluator
(`runCountCmpEvaluator`) always resolves it exactly and it never bails by
magnitude -- but it is sample-faithful too, so `circuitHasUnresolvedSampleableAgg`
admits all five. The exact (`delta = 0`) route is unchanged.

What remains of this item is the **rounding-based rejection FPTRAS** (Thm 9 /
Alg 6.3.1) proper, whose value over the stopping rule above is *rare-event
efficiency* (a sample count that does not blow up as `p -> 0`, via the rounded
proposal's bounded acceptance ratio). It is the research-grade,
statistical-bias-sensitive piece; the sound, magnitude-independent coverage of
the common case now exists without it.

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

**Note (approximate coverage already exists for these residuals).** The
direct-sampling route of item 2 evaluates the actual aggregate over each sampled
world regardless of join laminarity, so the residuals below (branch-spanning
SUM, UNION/EXCEPT re-using a base tuple) already get a sound `(eps,delta)`
estimate on an approximate request: when the laminar engine bails, the surviving
aggregate comparator is sampled. The items here are about the remaining
**exact** (PTIME) coverage.

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
  catalog-driven uniformly. *Partial:* the dispatch now also routes a surviving
  sample-faithful HAVING comparator (any aggregate) to the GenericCircuit
  sampler (item 2) instead of forcing a non-terminating Boolean build -- a step
  toward this, though still outside the catalog.
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
  cost-model-later seam.  *Note:* for methods whose cost is treewidth-driven and
  thus not cheaply predictable (d-tree; next, tree-decomposition), the bench
  showed cost-model refinement is a dead end -- **speculative execution** (a
  per-method work budget set to the next-best method's cost, escalating on
  overrun) is the chosen lever instead; see the d-tree "Cost open question --
  resolved" note in item 1.
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
- MC-fallback NOTICE: **decided -- keep it gated at `provsql.verbose_level >= 5`**
  (`Expectation.cpp` `mc_samples_or_throw`). A user who wants no Monte-Carlo
  estimate for random variables sets `provsql.rv_mc_samples = 0`, which turns the
  fallback into an exception instead of a silent estimate; the verbose-gated
  NOTICE is the right default (no console noise at the normal level, full
  disclosure for those who raise the level). No change.

### 6. d-tree research polish

- **Tractable variable-elimination order** (Olteanu-Huang-Koch Sec. VI-B,
  Lemma 6.8) for a guaranteed poly-size d-tree on hierarchical / `IQ` queries.
  The circuit has lost the query structure, so this means rediscovering a good
  order (the paper's SPROUT-vs-dtree gap); defer until the frequency heuristic is
  shown insufficient.
- **Cheaper memo keys** — *done*: the subproblem memo is now a
  `std::unordered_map<Clauses, double, ClausesHash>` (`src/DTree.cpp`) keyed by an
  order-sensitive hash of the canonical (subsumption-reduced, sorted) clause set,
  with the vector/set `operator==` resolving collisions, so lookups are
  average-`O(clause-set size)` instead of the former `std::map`'s `O(log n)`
  lexicographic compares over `vector<set<gate_t>>`. The remaining
  constant-factor item is the `O(depth)` leaf-closing form (Sec. V-D); the keys
  are no longer the bottleneck behind tree-decomposition on bounded treewidth.
- **Paper benchmark.** `test/bench/dtree_bench.sql` exercises the full portfolio;
  the one shape not yet reproduced is the paper's social-network experiment (the
  **triangle** and **path-of-length-2** queries over a random graph of
  tuple-independent edges, relative ε = 0.01) — d-tree should win by orders of
  magnitude at high edge probabilities and track the sampler at small ones.

### 7. Bibliography

*Done.* `website/_bibliography/references.bib` now carries all three anchor
papers alongside the surrounding canon (`DalviS12`, `JhaS11`, `AmsterdamerDT11`,
`GatterbauerS15`, the 2011 Suciu et al. textbook):

- `DBLP:conf/icde/OlteanuHK10` — Olteanu, Huang & Koch 2010 (ICDE), the d-tree;
- `DBLP:journals/vldb/ReS09` — Ré & Suciu 2009 (VLDB J.), the HAVING trichotomy;
- `DBLP:conf/icde/SouihliS13` — Souihli & Senellart 2013 (ICDE), ProApproX, the
  portfolio + cost-model principle behind the method catalog.

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
