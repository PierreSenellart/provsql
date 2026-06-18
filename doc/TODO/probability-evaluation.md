# Probability evaluation: remaining work (d-trees, HAVING trichotomy, catalog)

Open work only.  The built machinery – the method catalog + three-path chooser,
the d-tree engine (generalised to arbitrary `AND`/`OR`/`NOT`/`IN` circuits, under
a speculative subproblem budget that also covers `tree-decomposition`), the
HAVING marginal-vector evaluators, and the apx-safe `SUM`/`AVG`/`MIN`/`MAX`
sampling route – is described in `doc/source/dev/probability-evaluation.rst`.
Anchor papers (all in `website/_bibliography/references.bib`): Olteanu, Huang &
Koch (d-tree, ICDE 2010); Ré & Suciu (HAVING trichotomy, VLDB J. 2009); Souihli &
Senellart (ProApproX portfolio, ICDE 2013).

The three user-facing paths are tolerance grants – **exact ⊂ relative ⊂
additive** – not algorithm picks; the chooser returns the cheapest *admissible*
method.  Every method declares `guaranteeKind` / `isDeterministic` / `applicable`
/ `estimatedCost`, so new work below must fit that frame.

## 1. The SUM-safe rounding FPTRAS (`AggFptrasMethod`)

The apx-safe `SUM` / `AVG` / `MIN` / `MAX` corner is already covered
*approximately* by direct world-sampling (the DKLR stopping rule over the
surviving `gate_agg`), an FPRAS when `p ≥ 1/poly`.  What remains is the
**rounding-based rejection FPTRAS** (Thm 9 / Alg 6.3.1) proper, whose value over
that stopping rule is **rare-event efficiency** – a sample count that does not
blow up as `p → 0`, via the rounded proposal's bounded acceptance ratio – plus
the **MIN/MAX hard direction** on a safe skeleton.  Deciding skeleton safety
here would re-use the rewriter's hierarchical analysis
(`find_hierarchical_root_atoms`, `src/safe_query.c`); an unsafe skeleton is
hazardous (no FPTRAS, warn and fall back to additive MC).

**ProvSQL is now well-positioned for this.** The marginal-vector engine
(`AggMarginalEvaluator.cpp`) *is* the forward pass of Alg 5.2.1, and the world
generator is literally that recursion run in reverse (piece 2 below); the
hardest descent node – the Cartesian-product `⊗` split of a value-carrying
branch-spanning SUM – needs the per-factor **joint `(sum, count)` distribution**,
which is exactly the `sumCountPMF` built for the branch-spanning work.
So the one prerequisite that used to be missing is now in place.

**But the gap it closes is narrow.** The rounding FPTRAS only beats the existing
DKLR sampler on *safe-skeleton SUM with large-magnitude incommensurate values*
(so the exact pseudo-poly path bails) **and** a *small target probability* (so
DKLR's `Θ(1/p)` sample count is prohibitive) – the intersection of two corners
each already handled. It is the sole place ProvSQL is provably weaker than the
paper, but rarely the bottleneck in practice. Recommendation: keep deferred;
implement only when that rare-event SUM corner actually bites.

Thm 9 / Alg 6.3.1, three pieces:

1. *Rounding.* `τ^R(y) = ⌊(n²/k)·y⌋`, rounded-sum semiring `S_{n²+1}` (size `n²+1`,
   polynomial). Compute the rounded-sum PMF **exactly** with the marginal-vector
   recursion already built (`sumPMF` / `countPMF`, well under `kMaxSumSupport`),
   driven by the query-level safe plan from `find_hierarchical_root_atoms`.
2. *Random-world generator* (Alg 5.2.1, the one genuinely new subroutine):
   `sampleWorldWithValue(node, target_s)` walks the laminar recursion tree
   **top-down**, splitting `target_s` among children ∝ marginal-vector entries
   (`⊕`: pick `s₁+s₂=s` w.p. `m^φ₁[s₁]·m^φ₂[s₂]/m^φ[s]`; `⊗`: `s₁·s₂=s`; `⊔`:
   route all to one branch). **This is the bottom-up `countPMF` / `sumPMF` /
   `sumCountPMF` / `decomposeProduct` recursion run in reverse** (same tree, same
   vectors, the draw descends instead of folding up). Two concrete hooks: (i) the
   forward pass currently *discards* the per-node marginals – they must be
   retained as a lightweight `(combinator, child-PMFs, leaves)` tree (`O(circuit)`)
   or recomputed on the way down; (ii) the `⊗` split of a branch-spanning value
   draws against the per-factor joint from `sumCountPMF`. Alg 5.2.2 fills
   off-plan tuples (maps onto the existing BID / `mulinput` block handling).
3. *Accept-test* (Lemma 7): draw a rounded value `∝ μ(rounded)`, draw a world,
   accept iff it is an *original* solution; `m = O(n·β⁻¹·ε⁻²·log δ⁻¹)` samples →
   relative `(ε,δ)`.

New code `src/AggFptras.{h,cpp}`, consuming `AggMarginalEvaluator` internals
(needs them exposed) + `safe_query.c`. The `(α,θ)` × skeleton admissibility map:

| `(α, θ)` | safe skeleton | unsafe skeleton |
|---|---|---|
| `MIN ≤/<`, `MAX ≥/>` | FPTRAS (karp-luby) | **FPTRAS (karp-luby)** – gate-independent (`MAX≥k ⟺ ∃ present tuple, y≥k`, a UCQ; Thm 8) |
| `MIN ≥/>`, `MAX ≤/<` | FPTRAS (safe-plan) | hazardous |
| `SUM <,≤,≥,>` | FPTRAS (rounding+sampling) | hazardous |
| `SUM =,≠`, `AVG =,≠` | hazardous | hazardous |
| `COUNT` (any) | exact (dichotomy) | #P-hard; relative-MC if `p ≥ 1/poly` |

**Research-grade; high risk of subtle statistical bias.** Deferred for
collaborative implementation / review rather than autonomous coding.

## 2. HAVING marginal-vector engine: exact residual shapes

Approximate coverage of these exists (item 1's sampling); the open work is the
remaining **exact** (PTIME) coverage.  The laminar / cross-product engine covers
COUNT / SUM / MIN / MAX / AVG at arbitrary hierarchical depth; the residuals:

- **Branch-spanning SUM** – the separable shapes (additively separable
  `sum(b+c)` / `sum(2b-c+1)`, multiplicatively separable `sum(b*c)`) are now
  exact in `src/AggMarginalEvaluator.cpp`. Remaining: genuinely coupled values
  that are neither (`sum(b*c+b+c)`, a rank-≥2 weight tensor; may be `#P`-hard,
  self-gates back to enumeration today).
- **UNION/EXCEPT over a join that re-uses a base tuple** – `(R⋈S) UNION (R⋈T)` →
  `(r∧s)⊕(r∧t)`, non-read-once on the shared `r`. The *independent* case (each
  contributor's footprint private) is now exact (`contributorExactMarginal`,
  pinned by `test/sql/having_union.sql`). Remaining: a base tuple shared *across*
  a group's contributors, which couples them – the safe-query / read-once-rewriter
  problem, `#P`-hard in general.

## 3. Method-catalog follow-ups

- **Lazy Boolean build.** RV / HAVING circuits with no Boolean view fall to a
  small top-level estimator outside the catalog (the surviving-aggregate sampler
  routing of item 1 is a partial step). A true lazy Boolean build would fold even
  those into `chooseAndRun` so all three paths are catalog-driven uniformly.
- **Guarantee propagation** (decompose the whole-query `(ε,δ)` at
  **independence-certified gates only**): independent-OR `ε ≈ max(ε₁,ε₂)`,
  mutex-OR `max`, independent-AND `ε₁`; `1−δ = (1−δ₁)(1−δ₂)`. A `plus` whose
  children **share leaves** is not independent-OR, so the unit is a *maximal
  correlated component* (correlated cmps stay together and are sampled jointly;
  independent structure composes exactly). An exactly-cleared component
  contributes `ε=0`, loosening the budget for approximate siblings (the per-part
  payoff). **`monus` propagation is not in the paper** – derive the
  `Pr(monus(a,b)) = Pr(a)(1−Pr(b))` ε-propagation and its unsound-under-sharing
  caveat.  This is also the principled fix for the `karp-luby` `S·m` cost being
  pessimistic for large `m` (the calibrated per-node ε-split).
- **CircuitFeatures tier-2** – the treewidth proxy exists (`tw_proxy_`);
  independence certificates remain to be cached lazily. Optional: expose a
  `provsql.methods` SQL view (name, guarantee kind, applicability) for Studio.

## 4. RV probability transparency

- Route the RV *probability* case `P(X<c)` through `stopping-rule` (today it goes
  through fixed-sample / analytic paths only).

## 5. d-tree research polish

- **Tractable variable-elimination order** (Olteanu-Huang-Koch Sec. VI-B,
  Lemma 6.8) for a guaranteed poly-size d-tree on hierarchical / `IQ` queries.
  The circuit has lost the query structure, so this means rediscovering a good
  order (the paper's SPROUT-vs-dtree gap); defer until the frequency heuristic is
  shown insufficient.
- **`O(depth)` leaf-closing form** (Sec. V-D) – a constant-factor refinement of
  the memoised recursion (the memo keys themselves are no longer the bottleneck).
- **Paper benchmark.** `test/bench/dtree_bench.sql` exercises the full portfolio;
  the one shape not yet reproduced is the paper's social-network experiment (the
  **triangle** and **path-of-length-2** queries over a random graph of
  tuple-independent edges, relative ε = 0.01) – d-tree should win by orders of
  magnitude at high edge probabilities and track the sampler at small ones.

## Implementation observations

- The pre-pass slot, sound-only contract (numeric Bernoulli `gate_input`,
  meaningless to symbolic semirings), and `ref_count`-style privacy gating are
  established by `runAnalyticEvaluator` / `runCountCmpEvaluator`; new per-aggregate
  evaluators should reuse that contract and the
  `provsql.cmp_probability_evaluation` umbrella GUC, not invent a new path.
- The absorptive-vs-non-absorptive split in `pw_from_cmp_gate` (`S.absorptive()`)
  is the correct gate for the *symbolic* closed forms; the marginal-vector
  probability forms are a parallel probability-only track and must not disturb the
  semiring evaluation that non-probability semirings rely on.
- The Boolean-only and safe-query follow-ups this note borders on are in
  [`safe-query-followups.md`](safe-query-followups.md).
- **d-tree is closed** (anytime engine over arbitrary Boolean circuits, exact +
  certified bounds, BID / multivalued via the `handlesMultivalued()` rewrite).
  Two polish ideas were evaluated and **dropped** -- recorded so they are not
  re-attempted: (1) *non-DNF exact auto-selection* -- the d-tree's cost has no
  usable proxy ("the treewidth proxy is NOT used -- it mispredicts this
  engine"), so it stays by-name-only for exact; (2) *memoising the approximate
  path* -- tried, speeds d-tree ~20-30% but makes the auto `relative` path 11x
  slower on `big_rare` (681 ms -> 7.9 s) by suppressing the beneficial
  escalation to `compilation:d4`; net regression, reverted.
