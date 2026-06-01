# HAVING Trichotomy: Gains from Ré & Suciu

## Intro

This note assesses what ProvSQL can gain from Ré & Suciu, *"The trichotomy
of HAVING queries on a probabilistic database"* (VLDB Journal 18:1091–1116,
2009; conference version *"Efficient Evaluation of HAVING Queries on a
Probabilistic Database,"* DBPL 2007). The paper is the foundational theory
for computing the **probability** that a HAVING predicate
`agg(y) θ k` holds over a probabilistic (BID / tuple-independent) database,
for `agg ∈ {MIN, MAX, COUNT, SUM, AVG, COUNT(DISTINCT)}` and
`θ ∈ {=, ≠, <, ≤, >, ≥}`.

The central finding of this investigation is that the paper's framework maps
**almost one-to-one** onto machinery ProvSQL already has, and that its
algorithm is precisely the polynomial-time replacement for the exponential
possible-worlds enumeration ProvSQL falls back on today. The four gains below
ground and extend the Tier-1 / Tier-2 HAVING items already sketched in
[`safe-query-followups.md`](safe-query-followups.md); that note carries the
implementation layering (Layers A/B/C), this note carries the theory.

The reference write-up for the chosen HAVING *semantics* (the per-world
construction in `pw_from_cmp_gate`) remains Aryak's `algorithms.tex`; this
note is the complementary *probability-complexity* reference and supersedes
the loose paper pointers scattered in `safe-query-followups.md`.

## The mapping (the heart of it)

ProvSQL's aggregate-provenance circuit already *is* the paper's evaluation
structure. Recall the paper's Fig. 3: each aggregate is computed by annotating
the database with values from a semiring `S_α`, propagating them with a
relational plan over `S_α`, and reading the answer back through a Boolean
*recovery function* `ρ(s) ⟺ s θ k` (Prop. 2, Cor. 1). That triple is exactly
a ProvSQL `gate_cmp(gate_agg(α, [gate_semimod(K_i, m_i)]), gate_value(k))`:

| Ré/Suciu construct | ProvSQL construct |
|---|---|
| Semiring `S_α` + annotation `τ(t)` (Fig. 3) | `gate_semimod(K_i, m_i)` carrying per-row value `m_i` |
| Aggregate `α` over the skeleton `q` | `gate_agg(α, children)` |
| Recovery `ρ(s) ⟺ s θ k` (Fig. 3, Prop. 2) | `gate_cmp(gate_agg(…), gate_value(k))` |
| `S`-random variable `s_{τ,q}` (Def. 6) | the random circuit value of the `gate_agg` subtree |
| **Marginal vector** `m^r[s] = μ(r=s)` (Def. 8) | *missing* — ProvSQL has no marginal-vector carrier; it enumerates worlds |
| Convolution `⊛^+` (independent) / disjoint `⊥` (Def. 8, Prop. 1) | *missing* — the PTIME combine step |
| **Safe plan** computing `ω̂` over marginals (Def. 12, Lemma 1) | the safe-query rewriter `src/safe_query.c`, which today bails on `hasAggs` |
| `src/CountCmpEvaluator.{h,cpp}` Poisson-binomial DP | exactly the COUNT instance of `⊛^+` over `S_k = (Z_{k+1}, +_k, ·_k)` |

**Thesis.** ProvSQL's general probabilistic HAVING path
(`provsql_having` → `enumerate_valid_worlds` in `src/having_semantics.hpp`)
is the naive `O(2^N)` possible-worlds baseline the paper explicitly replaces.
The paper's **marginal-vector + monoid-convolution over a safe plan** is the
PTIME engine. `CountCmpEvaluator` already proves the pattern works for one
aggregate; the gain is to (a) generalize it per Fig. 3, and (b) gate it by a
real tractability classifier instead of a one-off syntactic check.

The paper's trichotomy itself: for self-join-free HAVING queries, every
`(α, θ, skeleton)` is exactly one of

- **`(α,θ)`-safe** — exact probability in PTIME (e.g. `(MAX,≥)`-safe);
- **`(α,θ)`-apx-safe** — exact is `#P`-hard but there is an FPTRAS
  (e.g. `(COUNT,≤)`-apx-safe);
- **`(α,θ)`-hazardous** — no FPRAS, approximation is hard too
  (e.g. `(SUM,=)`-hazardous).

For `MIN, MAX, COUNT` the third class is empty and it collapses to a clean
dichotomy: `α`-safe ⟹ PTIME, otherwise `#P`-hard (Theorem 2). The genuinely
three-way behaviour shows up for `SUM`, `AVG`, and `COUNT(DISTINCT)` (journal
version, §§5–6).

## Plan

### Gain 1 — Exact PTIME evaluators (the marginal-vector engine)

Generalize `CountCmpEvaluator` into a **marginal-vector aggregate evaluator**
keyed by aggregate, using the paper's Fig. 3 semirings and recovery functions.
A marginal vector `m[s]` holds `Pr[agg = s]` over the relevant value domain `S`;
the answer is `Σ_{s : ρ(s)} m[s]` (Cor. 1). Independent children combine by
convolution `⊛^+`, disjoint (same BID block) children by `⊥` (Prop. 1, each
`O(n·|S|²)` / `O(n·|S|)`).

- **MIN / MAX** — constant-size marginal vectors over `Z_3` / `Z_4` with
  `(max, min)` (Fig. 3). The closed forms are trivial (Theorem 1: `|S|`
  constant), matching Layers A and C of `safe-query-followups.md`. On the
  probability path: `MAX ≥ c` ⟹ `1 − ∏(1−p_i)` over the satisfying children,
  `MIN > c` ⟹ `∏ p̄_i` over the bad children, etc.
  **Landed** as `src/MinMaxCmpEvaluator.{h,cpp}`: a probability-side pre-pass
  (gated on `provsql.cmp_probability_evaluation`, same slot as
  `runCountCmpEvaluator`) covering all twelve `(MIN|MAX, {≥,>,≤,<,=,≠})`
  combinations with the empty-group exclusion, reusing the shared
  independence-certification machinery now factored into
  `src/CmpEvaluatorCommon.{h,cpp}`. Pinned by
  `test/sql/min_max_cmp_optimisation.sql` (off-vs-on parity, exact to four
  decimals) and benchmarked by `test/bench/min_max_cmp_bench.sql` (the off
  path is the `enumerate_exhaustive` `2^N` enumeration, so the speedup is
  unbounded past per-group `N ≈ 15`).
- **COUNT** — `S_k = (Z_{k+1}, +_k, ·_k, 0, 1)`, marginal size `≤ n`; `⊛^+`
  is the Poisson-binomial convolution **already implemented** in
  `CountCmpEvaluator` (`runCountCmpEvaluator`, the pre-pass slotted in
  `src/probability_evaluate.cpp` between `runAnalyticEvaluator` and
  `getBooleanCircuit`).
- **SUM** — same shape, the binomial recurrence replaced by a weighted-sum DP;
  marginal size `O(k)`. This is the Tier-1 "weighted-sum DP, not yet
  implemented" item. **Record the paper's Remark 3 caveat:** with `|S| = O(k)`
  the cost is `O(k·n^{|Q|})`, which is only *pseudo*-polynomial because the
  query-evaluation problem (Def. 4) takes `k` in **binary**. So SUM-safe is not
  "efficient" in the paper's strict sense — this is exactly why SUM bifurcates
  in the trichotomy (Gain 2/3).
  **Landed** as `src/SumCmpEvaluator.{h,cpp}`: the subset-sum DP over the
  reachable-sum range, same pre-pass slot and shared
  `src/CmpEvaluatorCommon.{h,cpp}` independence certification as the COUNT /
  MIN-MAX arms, with a range cap implementing the Remark 3 pseudo-poly
  fallback. Pinned by `test/sql/sum_cmp_optimisation.sql`, benchmarked by
  `test/bench/sum_cmp_bench.sql`.
- **COUNT(DISTINCT)** — two-stage: per-value `EXISTS` marginal bottom-up, then
  count the distinct present values (Def. 14, Theorem 3). The marginals are
  *lossy* (only the count's distribution survives, not which values), which
  shrinks the safe class — only plans whose final operation projects away `y`
  with no proper ancestor doing the same are `COUNT(DISTINCT)`-safe.
  **Already realised by the existing machinery, no dedicated arm needed.**
  ProvSQL's `COUNT(DISTINCT)` rewrite (`rewrite_agg_distinct` in `src/provsql.c`)
  *is* this two-stage construction: an inner `GROUP BY (key, group)` computes
  the per-value `EXISTS` provenance, and the outer plain `COUNT` over those
  deduped rows is then resolved by `runCountCmpEvaluator` (the per-value
  `EXISTS` events are leaf-disjoint, so the independence certification fires).
  The HAVING case was extended to `q->havingQual` (commit "Fix COUNT(DISTINCT)
  in HAVING"); `test/sql/having_count_distinct.sql` pins it. So Priority 6
  below is effectively closed.

Implementation home and shape match `CountCmpEvaluator`: a probability-only
pre-pass that replaces the `gate_cmp` with a Bernoulli `gate_input` carrying
the computed probability, bypassing DNF/possible-worlds construction entirely.
Net effect: the `2^N` exhaustive path (MIN/MAX/EQ/NE in `enumerate_valid_worlds`)
and the generic `sum_dp` are superseded by poly-size closed forms for the
probability path.

### Gain 2 — A HAVING trichotomy classifier

The paper's payoff beyond the closed forms is the **classification** that says
*when* each path is sound and *which* method to use. Today ProvSQL has no such
gate: every HAVING cmp is routed through `provsql_having`'s enumeration
regardless of tractability.

Propose a **HAVING classifier** analogous to `src/classify_query.c`
(`classify_top_level`, which already labels each top-level SELECT TID/BID/OPAQUE
and emits a `NOTICE`). Given `(α, θ, skeleton)` it would emit
`safe` / `#P-hard` / `apx-safe` and drive method selection:

- `safe` → the exact marginal-vector evaluator (Gain 1);
- `apx-safe` → the FPRAS path (Gain 3);
- `hazardous` → warn and fall back to additive Monte-Carlo.

For `MIN/MAX/COUNT` the verdict is just the skeleton's safe/unsafe status
(Theorem 2 dichotomy; the safe-plan test of Dalvi & Suciu, already encoded in
`src/safe_query.c`). The three-valued verdict is needed only for
`SUM/AVG/COUNT(DISTINCT)`. Like `classify_top_level`, this is cheap, read-only,
and decidable in PTIME in the size of the query (paper: "It can be decided in
polynomial time in the size of `Q` if `Q` is `α`-safe").

### Gain 3 — Relative approximation for the apx-safe class

This section records the **design conclusions** reached while studying the
approximation half of the paper (`~senellar/s00778-009-0151-4.pdf`, §5–6).
The concrete build is in *Implementation plan: the relative-approximation
surface* below.

**The user-facing contract = two surfaces.**
- *Exact surface* (`possible-worlds`, `tree-decomposition`, `independent`,
  `d4`/`c2d`/…, default chain): promises the true probability; may be slow or
  give up, but **must never silently return an estimate**. *Caveat — this
  applies to the discrete probability surface.* The continuous-RV moment /
  probability functions are a separate, **approximate-by-nature** surface
  (continuous distributions usually have no closed form, so MC is unavoidable):
  their contract is not "never approximate" but "the user can always tell
  whether the answer is closed-form or MC." Today the RV path falls back to
  `monteCarloScalarSamples` *silently* (`provsql.rv_mc_samples` opt-out); the
  fix is **transparency** (signal the fallback), not refusal — see Phase E.
- *Approximate surface*: promises a **relative** `(1±ε)` result with
  confidence `1−δ`. The user states `(ε,δ)` and that is the whole contract;
  **which mechanism delivers it is internal** and invisible (the method names
  `karp-luby` / `monte-carlo` are implementation detail leaking into the API).

**Additive is not an FPRAS — relative is the promise.** Fixed-sample MC gives
*additive* `|p̂−p| ≤ ε` (Hoeffding, `N = O(ln(1/δ)/ε²)`, independent of `p`),
which does **not** discharge the apx-safe guarantee and degrades to ≈ 0 on
rare events. A relative FPRAS needs the **`(ε,δ)`-relative stopping rule**
(Dagum–Karp-Luby-Mihail-Ross): sample until enough satisfying worlds accrue to
bound the *relative* error, `N ∝ 1/p`, polynomial iff `p ≥ 1/poly`. ProvSQL
already has this loop as `BooleanCircuit::karpLubyStopping`. (A reverted commit
shipped the *additive* version under `monte-carlo` — the lesson: never swap an
exact computation for an estimate, and never call additive MC an FPRAS.)

**Guarantee at the whole query, not per cmp gate.** Per-cmp guarantees do not
compose: relative error survives products but blows up under OR / monus /
inclusion-exclusion, and resolving a cmp to a Bernoulli *estimate* and plugging
it in breaks the correlation between that cmp and the rest of the lineage. The
`(ε,δ)` must be on `P(whole query result)` — sample the **entire circuit root**
(`evalBool(root)` under the stopping rule), so one sampled world fixes every
tuple once and all cmps + the surrounding Boolean structure are evaluated
jointly and consistently. The closed-form pre-passes then play a precise role:
they resolve the **independent** cmps **exactly** (verified by
`aggSubtreePrivate`), which is sound Rao-Blackwellised variance reduction in
front of the whole-circuit sampler — if everything resolves, no sampling.

**cmp gates: expand or keep symbolic.** Two options for a cmp during
evaluation: (a) *expand* it to its Boolean lineage (the threshold DNF, via
`pw_from_cmp_gate`) — then any Boolean method applies, but the DNF is
`binom(N,k)` clauses; (b) *keep it symbolic* and let the evaluator compute the
aggregate over the (sampled) world and apply the comparison in place — no
blowup, but only an evaluator that understands `gate_agg`/`gate_cmp` can do it
(the tuple sampler can; karp-luby / d-DNNF cannot). For the hard cases
(where approximation is needed) expansion is exponential, so **the approximate
surface keeps cmp gates symbolic and samples them** — which makes the *sampler*
the source of the cmp's semantics. **Risk: drift from the canonical
(expansion) semantics.** Today there are two independent implementations of
"value of `agg θ k` in a world": `pw_from_cmp_gate` (the expansion, which has
the numeric/float scaled-integer domains and the empty-group exclusion) and
`MonteCarloSampler`'s `gate_agg`/`gate_cmp` arms (the sampler). They can
diverge — e.g. `HAVING sum(x) >= -5` on an all-absent group: the expansion
excludes the empty group (false), a naive sampler computes `0 ≥ −5` (true).
**Required fix: one shared per-world cmp primitive** used by both the sampler
(one world) and the expansion (all worlds = the DNF), so sample and expand are
the same semantics by construction and the relative FPRAS approximates exactly
what the exact method would have returned.

**The paper's FPTRAS is built on safe plans, and is implementable.** Reading
§5–6: "use safe plans as a guide to sample." Three pieces of very different
difficulty:
- *MIN/MAX, easy direction* (`MIN ≤/<`, `MAX ≥/>`) — equivalent to a
  conjunctive query with an inequality selection (`MAX(y) ≥ k ⟺ ∃ present tuple
  with y ≥ k`), so it is just `P(a UCQ)` → the **existing karp-luby DNF
  FPRAS**, on **any** skeleton (Thm 8). Nearly free.
- *SUM, and MIN/MAX hard direction, over a SAFE skeleton* — Dyer-style rounding
  + guided sampling (Thm 9, Alg 6.3.1): round values down
  `τ^R(y)=⌊(n²/k)·y⌋` (bound `n²`) so the rounded-sum semiring has size `n²+1`
  (polynomial); compute the rounded-sum distribution **exactly** with the
  safe-plan semiring algorithm (= `sumPMF`/`countPMF` over the small domain,
  well under `kMaxSumSupport`); **sample** worlds satisfying the *rounded*
  predicate; accept-test the fraction that are *original* solutions; multiply
  by `μ(rounded)`. Lemma 7: originals are a `≥ n⁻¹β` fraction, so it converges
  in `m = O(n·β⁻¹·ε⁻²·log δ⁻¹)` samples — **polynomial, relative error**. This
  is exactly the regime the exact engine currently *bails on* (`kMaxSumSupport`,
  Remark-3 pseudo-poly), so the SUM-FPTRAS is the principled replacement.
- *The one new subroutine — the random-world generator* (Alg 5.2.1): walk the
  safe-plan parse tree **top-down**, splitting the target value `s` among
  children proportional to the marginal-vector entries (`⊕`: pick `s₁+s₂=s`
  w.p. `m^φ₁[s₁]·m^φ₂[s₂]/m^φ[s]`; `⊗`: `s₁·s₂=s`; `⊔`: `(s,0)`/`(0,s)`), then
  Alg 5.2.2 fills the off-plan tuples. **This is the bottom-up marginal-vector
  recursion run in reverse** — the hard prerequisite (the parse tree with
  marginal vectors) is already `countPMF`/`sumPMF`/`decomposeProduct`.
- *Unsafe skeleton + SUM/AVG/COUNT(DISTINCT)* → **hazardous**, no FPTRAS. And
  `SUM =`/`≠` → hazardous (a thin set has no relative approximation).

**Safe plan = the safe-query machinery (the detection half, not the rewrite
half).** The FPTRAS prerequisite "safe plan" is exactly what
`find_hierarchical_root_atoms` builds and `safe_query_skeleton_is_hierarchical`
certifies — the variable hierarchy = the parse tree `φ(P,J,τ)`. Earlier this
line was dismissed by conflating it with the *read-once rewrite*
(`try_safe_query_rewrite`, the `SELECT DISTINCT` wraps) — that rewrite collapses
the multiset and is genuinely wrong for aggregates, but the *plan detection* it
is built around is the **backbone of the FPTRAS**. Consequences:
- The skeleton-safety bit (`safe_query_skeleton_is_hierarchical`, built under
  Gain 4 and so far unused) is the **load-bearing gate** for approximation:
  safe skeleton → safe-plan FPTRAS; unsafe → hazardous (warn); MIN/MAX-easy →
  karp-luby, gate-independent. The Gain-2 classifier's skeleton axis finally
  earns its keep.
- My circuit-level laminar recursion re-derives the same plan per instance for
  the cases it handles, but the **query-level** safe plan is the authoritative
  source and the natural input to the FPTRAS — it *certifies* the skeleton is
  safe (so the relative guarantee can be honestly claimed) and exposes the plan
  even where a materialised circuit might not self-expose it.

**The `(α,θ)` map for approximation (gate × direction):**

| `(α, θ)` | safe skeleton | unsafe skeleton |
|---|---|---|
| `MIN ≤/<`, `MAX ≥/>` | FPTRAS (KL) | **FPTRAS (KL)** — gate-independent |
| `MIN ≥/>`, `MAX ≤/<` | FPTRAS (safe-plan) | hazardous |
| `SUM <,≤,≥,>` | FPTRAS (rounding+sampling) | hazardous |
| `SUM =,≠`, `AVG =,≠` | hazardous | hazardous |
| `COUNT` (any) | exact (dichotomy) | #P-hard; relative-MC if `p≥1/poly` |

### Gain 4 — Independence certification via the safe-query rewriter

`CountCmpEvaluator` is sound only when the `gate_agg` children share no input
leaves; it enforces this with a **syntactic** check — `ref_count == 1` on every
gate of the `K_i → semimod_i → gate_agg` chain
(`src/CountCmpEvaluator.cpp`), which guarantees the cmp's leaves are private to
its subtree. That is conservative: any join that shares a leaf is rejected, so
the closed form fires only on flat single-table COUNT.

The paper supplies the **semantic** condition instead: a *safe plan* combines
random variables that are independent (`⊛^+`) or disjoint (`⊥`) by
construction (Def. 12, Lemma 1). The gain is to reuse the **safe-query
hierarchical detector** (`src/safe_query.c`) on the HAVING *body* to certify
that independence — the Tier-2 "HAVING independence certification" item — which
would extend all four closed-form evaluators (Gain 1) to **joins**, not just
flat tables. Prerequisite (already noted in `safe-query-followups.md`): the
rewriter currently bails on `hasAggs` and needs a one-level descent into the
HAVING subquery to certify "the FROM/WHERE/GROUP-BY feeding this aggregate is
hierarchical."

**This is a new engine, not a `ref_count` relaxation.** The flat check does
triple duty: read-once *within* a contributor, leaf-disjointness *across*
contributors, and no reuse outside the cmp. A join breaks the middle one (two
aggregate-input rows sharing a base tuple — fan-out, e.g. `R(k,a),S(a,b)` with
two `b` per `a`). The tempting fix — run the `boolean_provenance` read-once
rewriter (`try_safe_query_rewrite`) on the skeleton so the lineage becomes
read-once — **does not work**: that rewrite makes *Boolean existence*
read-once via `SELECT DISTINCT` wraps, which is exactly why
`is_safe_query_candidate` bails on `hasAggs` (the wraps collapse the multiset
the aggregate must count). Existence-read-once is the wrong semantics for
COUNT/SUM. What the paper actually requires is a **recursive marginal-vector
evaluator**: carry `m[s] = Pr[agg = s]` and combine sub-blocks by convolution
`⊛^+` (independent root-variable values) or disjoint sum `⊥` (same BID block),
descending the hierarchy. The flat Poisson-binomial in `CountCmpEvaluator` is
the degenerate single-level case (all contributors independent). The skeleton
certificate gates this engine; it does not produce it.

**Architecture: bake at planner time, evaluate at probability time.** The
CmpEvaluators run on a circuit reloaded from the mmap store with no access to
the original `Query`, so they cannot call
`safe_query_skeleton_is_hierarchical` (which needs the parse tree at
planner-hook time). Follow the inversion-free precedent: analyse `sk(Q)` at
planner time and bake a `CERT_SAFE_AGG_PLAN` blob — the root-variable nesting /
block structure from `find_hierarchical_root_atoms`, currently discarded —
onto the `gate_agg` via `src/safe_query_cert.{c,h}` (append-only); the
probability-time engine consumes it. For the *independent-blocks* case the
circuit-level footprint oracle below substitutes for the certificate, so the
common star-schema shape may need no planner baking at all.

**Reusing the RV distribution algebra.** The continuous-RV path already has the
two combinators this engine needs, but not in reusable exact form, and the
distinction matters:

- Its "convolution" is **family-specific closed form** (Normal sum → Normal,
  i.i.d. Exponential sum → Erlang, in `runHybridSimplifier`) plus a bounded
  **2^k joint table** (`runHybridDecomposer`, `JOINT_TABLE_K_MAX = 8`, else
  MC); `Expectation` carries **moments, not distributions**. So modelling an
  aggregate as `gate_mixture` + `gate_arith PLUS` and routing it through the RV
  evaluator yields **Monte Carlo, not exact PTIME** — the very fallback this
  gain replaces. The exact discrete-convolution primitive is missing on *both*
  sides; P4 must build it.
- What *is* reusable: (a) the **independence oracle** `FootprintCache` /
  `getJointCircuit`, which decides leaf-set disjointness on the *circuit* (the
  probability-time view), giving the `⊛^+` independence test without a
  certificate; (b) **`gate_mixture` as the `⊥` primitive** — a BID block of
  size n is one categorical factor, not 2^n enumerated outcomes.
- So build the engine as a small **discrete distribution-factor algebra**: a
  marginal vector combined by `⊛^+` (convolution) and `⊥` (categorical
  mixture). Factor the two combinators into a shared util so a later RV lift is
  mechanical — the same `⊥` combinator dropped into `runHybridDecomposer` would
  let its dependent-comparator case scale past `JOINT_TABLE_K_MAX` when the
  correlation graph is tree-structured (a real but secondary RV win). Keep two
  carriers: discrete + bounded for HAVING (exact), the existing closed-form / MC
  machinery for continuous RVs (which has no exact general convolution).

## Priorities

1. **Gain 1, MIN/MAX arm** — constant-size, closed forms already specified in
   `safe-query-followups.md` Layer A/C; smallest, highest-confidence win, and
   it kills the `2^N` path for the most common non-COUNT aggregates.
   **Done** — see `src/MinMaxCmpEvaluator.{h,cpp}` and the note under Gain 1.
2. **Gain 1, SUM arm** — weighted-sum DP; ship with the explicit pseudo-poly
   caveat (Remark 3) and a sane `k` bound.
   **Done** — see `src/SumCmpEvaluator.{h,cpp}` and the note under Gain 1.
3. **Gain 2, classifier** — needed to make Gains 1/3 self-selecting and to stop
   the silent exponential fallback; mirrors `classify_top_level` closely.
   **Done** — `src/classify_having.c`, GUC `provsql.classify_having`: a
   read-only `NOTICE` per HAVING aggregate comparison giving the
   safe / apx-safe / #P-hard / open verdict from the `(α, θ)` overlay × the
   skeleton-safety bit. Pinned by `test/sql/classify_having.sql`.
4. **Gain 4, safe-join marginal-vector engine** — unlocks joins for all of
   Gain 1. **Read-only half done** — `safe_query_skeleton_is_hierarchical`
   (`src/safe_query.c`) certifies skeleton safety read-only (the classifier's
   missing axis); `test/sql/skeleton_safety.sql` pins it. The recursive
   marginal-vector engine described under Gain 4 above (the riskier half: it
   touches evaluation soundness) has **landed for COUNT / SUM / MIN / MAX at
   arbitrary hierarchical depth** (Phases 2-3 below); UNION/EXCEPT
   contributors and the BID disjoint-block certificate path remain. Phased:
   1. *Certificate plumbing* — `safe_query_extract_aggregate_plan` exposes the
      `find_hierarchical_root_atoms` block structure; bake `CERT_SAFE_AGG_PLAN`
      onto the `gate_agg` at the HAVING-lift site in `src/provsql.c`
      (`having_Expr_to_provenance_cmp`), carry it through `CircuitFromMMap`.
   2-3. *Engine, COUNT / SUM / MIN / MAX* — **Done (arbitrary hierarchical
      depth)** — `src/AggMarginalEvaluator.cpp` (`runAggMarginalEvaluator`):
      one hierarchical recursion factoring each independence block's shared
      root event (`⊥` mixture) and combining independent blocks (`⊛^+`).
      COUNT/SUM carry the count/weighted-sum distribution (`countPMF`,
      `sumPMF`, the latter with the Remark-3 support cap); MIN/MAX reduce to a
      few `pAllAbsent` calls over value-thresholded subsets (the hierarchical
      generalisation of `MinMaxCmpEvaluator`'s independent-only `qprod`). A
      new pre-pass arm after the flat `runCountCmpEvaluator` /
      `runMinMaxCmpEvaluator` / `runSumCmpEvaluator` in
      `probability_evaluate.cpp`, same `provsql.cmp_probability_evaluation`
      GUC and Bernoulli-`gate_input` contract. Pinned by
      `test/sql/having_safe_join_count.sql` (COUNT) and
      `test/sql/having_safe_join_agg.sql` (SUM/MIN/MAX): off-vs-on parity to 4
      decimals vs exact enumeration on the fan-out `R(k,a),S(a,b)` over all
      operators, 3-way star, depth-2 nesting `acct(u),ord(u,o),item(u,o,i)`,
      the non-hierarchical triangle declining, and the flat degenerate no-op.
      Benchmarked by `test/bench/having_safe_join_count_bench.sql` (≈276× at
      per-group N=12; off-path TIMEOUT past N=15 while the engine stays
      ~10 ms to N=100). **No planner certificate is needed for this whole
      class** — the engine gates *circuit-only* and self-gates: at every
      recursion level each multi-member block must have a leaf common to all
      its members (and every leaf private to the cmp subtree), which the
      non-laminar triangle fails. Detection is **join-order- and
      subquery-invariant**: the lineage is content-addressed and `times` is
      AND on the probability path, so `parseProductContributor` flattens
      nested `gate_times` (SPJ subqueries / views, which build
      `times(times(r,s),t)`) to the same leaf set as the flat join, and the
      privacy check (`aggSubtreePrivate`) walks the whole product-DAG so a
      shared subquery-tuple gate is handled. This is why the circuit-level
      recogniser is preferable to a query-level rewrite, which *would* be
      order-sensitive. The **cross-product / product-join** node (safe-but-non-laminar COUNT,
      e.g. `R(a),S(a,b),T(a,c)` whose count is `N_S·N_T`) is **done for
      COUNT** — `tryProductFactors` + `productConvolve` in `countPMF`. It is
      circuit-detectable, no certificate: a multi-member block with no common
      leaf is a genuine product iff it is a *complete leaf-disjoint*
      multipartite set (factors = non-co-occurrence components; verify
      `|contributors| = ∏|factor|`), which the safe cross-product passes and
      the #P-hard `h0`/triangle fails (its private *middle* leaf — the
      `S(x,y)` tuple — collapses the factor partition / breaks completeness),
      as does any branch-linking predicate (the incomplete bipartite case).
      Pinned by the `xprod` / `3factor` / `xprod filtered` rows in
      `having_safe_join_count.sql`. The product node also covers
      **SUM/MIN/MAX** (`having_safe_join_agg.sql` `xprod …` rows): MIN/MAX
      need only the value-agnostic `pAllAbsent` product node
      (`1 - ∏(1-pAllAbsent(factor))`), since `minMaxProb`'s value-thresholded
      subsets are sub-products when the value is single-branch (and bail when
      genuinely non-rectangular); SUM factors as `S_f · M` only when the
      value depends on a single factor (detected: weight constant within each
      `f`-part group), else it bails (a branch-spanning value may be #P-hard).
      **AVG** is done too (`having_safe_join_agg.sql` `AVG …` rows): it
      reduces to `SUM(v_i − C) θ 0` with the empty group excluded, so it
      inherits the entire laminar / product machinery — and is the *first*
      closed-form AVG path at all, firing even on a flat single table (no
      prior pre-pass covered AVG; only integer thresholds reach here, a
      fractional HAVING-AVG constant being rejected upstream). On `gate_plus`/`gate_monus` (UNION/EXCEPT) contributors:
      **independent read-once unions already work** — the flat
      `CountCmp`/`SumCmp`/`MinMaxCmp` evaluators resolve them via
      `contributorProb` (which handles plus/monus), pinned by
      `test/sql/having_union.sql` (off-vs-on parity, UNION DISTINCT + EXCEPT
      over COUNT/SUM/MIN/MAX). The genuine residual is **UNION/EXCEPT over a
      join that re-uses a base tuple** (`(R⋈S) UNION (R⋈T)` → `(r∧s)⊕(r∧t)`,
      non-read-once on the shared `r`), which needs per-contributor read-once
      factoring (`r∧(s∨t)`) — the safe-query/read-once-rewriter problem,
      #P-hard in general. Still open:
      branch-spanning SUM via per-factor joint (sum,count) distributions, and
      the genuinely certificate-only **BID disjoint-block `⊥`** structure
      (mutual exclusion from a key constraint is a semantic fact that need not
      surface as circuit leaf-sharing).
   4. *Validation* — `test/sql/having_safe_join_{count,minmax,sum}.sql`
      off-vs-on parity against `possible-worlds` on the `R(k,a),S(a,b)`
      fan-out, star-schema, and BID-`⊥` shapes; a **negative** test that the
      `R(x,y),S(y,z),T(z,x)` triangle does *not* fire the fast path;
      degenerate-equivalence guard (flat skeleton ⇒ bitwise-identical to
      today's `runCountCmpEvaluator`). Benchmark + `references.bib`
      (Ré-Suciu 2009, Dalvi-Suciu J.ACM 2012) + `probability-evaluation.rst`.

   Open spikes before Phase 2: how BID-`⊥` block membership surfaces in the
   loaded circuit (repair_key / mutually-exclusive inputs), and confirming
   `FootprintCache` / `getJointCircuit` are callable from the CmpEvaluator
   pre-pass slot. First shippable slice: Phases 1–2 for COUNT on single-level
   fan-out, validated against `possible-worlds`.
5. **Gain 3, the relative-approximation surface** — the conceptual conclusions
   are recorded under *Gain 3* above (two-surface contract; relative-not-
   additive; whole-query-not-per-cmp; the shared per-world cmp primitive; the
   paper's safe-plan FPTRAS; safe-plan = the safe-query detection half). The
   concrete build is the *Implementation plan* section below. **This is now the
   main open item.** Supersedes the earlier "auto-route to karp-luby" framing,
   which was wrong: karp-luby cannot consume a threshold DNF.
6. **Gain 1, COUNT(DISTINCT) arm** — **closed**: already realised by the
   `COUNT(DISTINCT)` GROUP-BY rewrite + `runCountCmpEvaluator` (see the
   COUNT(DISTINCT) note under Gain 1); the HAVING gap was fixed. No dedicated
   arm needed.

## Implementation plan: the relative-approximation surface

Concrete, phased build of the *approximate surface* described under *Gain 3*.
The contract is: the user asks for `(ε, δ)`; ProvSQL returns a value within a
**relative** `(1±ε)` factor of the true probability with confidence `1−δ`,
choosing the internal mechanism itself. "All bells and whistles" = the
stopping-rule estimator, the safe-plan FPTRAS, the MIN/MAX karp-luby shortcut,
the closed-form pre-pass as variance reduction, and the skeleton-safety gate —
all behind one surface.

**Design invariants (do not violate):**
- *Exact never approximates.* Approximation only ever happens because the user
  selected the approximate surface (an explicit `(ε,δ)` request). No *discrete*
  exact method silently estimates. (The continuous-RV surface is approximate by
  nature — see the *Gain 3* caveat and Phase E — so the rule there is "never
  estimate *silently*," not "never estimate.")
- *Relative, not additive.* Every mechanism on this surface must deliver a
  relative bound. Fixed-sample additive MC is **not** admissible here; it stays
  available only under an explicit `monte-carlo` method request, which is a
  *different*, additive contract the user opts into by name.
- *Whole query, not per cmp.* The `(ε,δ)` is on `P(root)`. We sample / bound the
  whole circuit root; cmp gates are resolved exactly by the pre-pass where
  independent, and sampled jointly with the rest otherwise.

### Phase A — the shared per-world cmp primitive (prerequisite, no new surface)

The blocker for any sampler-based cmp handling: today the "value of `agg θ k` in
one world" lives in two places that can disagree (`pw_from_cmp_gate`'s
expansion vs `MonteCarloSampler`'s `gate_agg`/`gate_cmp` arms). Unify them.

- Extract a single function `eval_cmp_in_world(cmp_gate, present: predicate over
  leaves) -> bool` that: gathers the `gate_agg`'s contributor tuples, keeps the
  ones `present` selects, applies the aggregate (COUNT/SUM/MIN/MAX/AVG), applies
  the comparison from `info1`, and reproduces **exactly** the expansion's edge
  conventions — the **empty-group exclusion** (no present tuple ⇒ predicate
  false, even for `sum(x) ≥ −5` or `min(x) ≤ k`) and the **numeric/float
  scaled-integer domain** (reuse `parse_decimal_scaled` / `rescale_to`, dispatch
  on `info2`).
- Re-express both callers in terms of it: `pw_from_cmp_gate` = `eval_cmp_in_world`
  applied over the enumerated worlds (the DNF); `MonteCarloSampler` = the same
  applied to the one sampled world. Lives in `CmpEvaluatorCommon.{h,cpp}`
  alongside `matchAggCmp` (which already centralised the typed parsing).
- Tests (parity, exact, both surfaces irrelevant — this is semantics): empty
  group under every `(agg,θ)`; `sum(x) ≥ negative`; `min/max` boundary `=`;
  numeric fractional threshold; a world where sampler and full expansion must
  return the identical bit. This is the regression that pins "sample = expand".

### Phase B — relative stopping-rule whole-query MC (the universal fallback)

The always-correct (when `p ≥ 1/poly`) relative estimator, behind the
approximate surface.

- Add a `Sampler`-driven whole-circuit Boolean estimator that runs the
  **Dagum–Karp-Luby-Mihail-Ross `(ε,δ)`-relative stopping rule** — reuse the
  existing `BooleanCircuit::karpLubyStopping` loop logic, but driven by
  `MonteCarloSampler::evalBool(root)` (so RV leaves, `gate_cmp`, `gate_agg` are
  all in scope via Phase A) rather than DNF clauses. One sampled world fixes
  every `gate_input`/`gate_rv` once; `eval_cmp_in_world` decides each cmp on
  that world; the surrounding plus/times/monus compose as usual.
- Wire it as the **default mechanism of the approximate surface**: when the user
  supplies `(ε,δ)` and no more specific FPTRAS applies, this runs. It also is the
  *correctness oracle* the later phases are diffed against.
- Honest limit, logged not hidden: cost `∝ 1/p`. If the stopping rule has not
  converged within a budget, emit a NOTICE that `p` is too small for a relative
  guarantee at this `(ε,δ)` (do **not** silently return an additive-ish value).
- Reuse `parse_eps_delta` / `parse_sample_spec` for the grammar; surface the
  request as a method name on `probability_evaluate` (e.g. `approximate` /
  `relative`, taking `(ε,δ)`), distinct from the additive `monte-carlo`.

### Phase C — MIN/MAX easy direction → karp-luby (nearly free, any skeleton)

`MAX(y) ≥ k` / `MIN(y) ≤ k` reduce to `P(∃ present tuple with y θ k)` = a
monotone UCQ over the qualifying tuples — exactly what `evaluate_karp_luby`
already approximates with a relative guarantee, on **any** skeleton (Thm 8).

- Detect this `(α,θ)` shape on the matched `gate_cmp` (reuse `matchAggCmp`;
  the direction test is `info1` ∈ {≥,>} for MAX, {≤,<} for MIN).
- Build the threshold UCQ (the disjunction over contributors with `y θ k`,
  filtered by the scaled-integer domain) and hand it to the **existing**
  karp-luby DNF FPRAS. No new estimator. This is the cheapest win and covers the
  gate-independent column of the `(α,θ)` table.

### Phase D — SUM-safe FPTRAS (rounding + guided sampling, gated on safe skeleton)

The research-grade slice: SUM (and MIN/MAX hard direction) over a **safe
skeleton**, Thm 9 / Alg 6.3.1. Gated by
`safe_query_skeleton_is_hierarchical` — *only* fires when the skeleton is
certified safe; otherwise the predicate is **hazardous** → Phase F warns.

1. *Rounding.* `τ^R(y) = ⌊(n²/k)·y⌋`, rounded-sum semiring `S_{n²+1}` (domain
   size `n²+1`, polynomial). Compute the rounded-sum PMF **exactly** with the
   safe-plan recursion already built — `sumPMF` / `countPMF` over the small
   rounded domain (well under `kMaxSumSupport`), driven by the **query-level**
   safe plan from `find_hierarchical_root_atoms`, not the per-instance circuit
   sniffing.
2. *Random-world generator* (Alg 5.2.1, the one genuinely new subroutine):
   `sampleWorldWithValue(plan_node, target_s)` walks the safe-plan parse tree
   **top-down**, splitting `target_s` among children proportional to the
   marginal-vector entries (`⊕`: choose `s₁+s₂=s` w.p.
   `m^φ₁[s₁]·m^φ₂[s₂]/m^φ[s]`; `⊗`: `s₁·s₂=s`; `⊔`: route all to one branch).
   This is the existing bottom-up `countPMF`/`sumPMF`/`decomposeProduct`
   recursion **run in reverse** — same parse tree, same marginal vectors, the
   draw just descends instead of folding up. Then Alg 5.2.2 fills off-plan
   tuples consistent with the chosen value.
3. *Accept-test* (Lemma 7): sample a rounded value `∝ μ(rounded)`, draw a world
   with `sampleWorldWithValue`, accept iff it is an *original* (un-rounded)
   solution; the accepted fraction estimates the correction. `m = O(n·β⁻¹·
   ε⁻²·log δ⁻¹)` samples → relative `(ε,δ)`. Diff against Phase B on small
   instances where both run (Phase B is the oracle).
- New code: `src/AggFptras.{h,cpp}` — the rounding semiring, the top-down world
  generator over the safe plan, the accept loop. Consumes the marginal-vector
  helpers from `AggMarginalEvaluator` and the safe plan from `safe_query.c`.

### Phase E — continuous-RV transparency (NOT a GUC flip)

Independent of HAVING, and the subtlest of the phases because the obvious move
is wrong. The continuous-RV moment / probability functions (`expected`,
`variance`, `moment`, cmp-event probabilities, `rv_sample`, `rv_histogram`)
fall back to `monteCarloScalarSamples` by default (`provsql.rv_mc_samples`
opt-out). It is tempting to "fix the exact-surface leak" by flipping the
default to opt-in (error when no closed form) — **do not**. Continuous
distributions are intrinsically approximate: for an arbitrary arithmetic
composite or conditioned expression there *is* no closed form, so refusing to
sample does not make the surface exact, it makes it useless. These functions
are **an approximate-by-nature surface**, not part of the discrete exact
surface; the *Gain 3* contract for them is "analytic when a closed form exists,
MC otherwise, and the user can always tell which." The real defect is
**silence**, not sampling.

- *Transparency, not refusal.* When the MC fallback fires, signal it — a
  verbose-gated NOTICE and/or a distinguishable result — so an estimate is
  never mistaken for a closed-form value. MC stays the default; usefulness is
  preserved. `rv_mc_samples` stays an opt-out budget, just no longer silent.
- *Upgrade the probability case to a real guarantee.* Where the quantity is a
  **probability** (a `gate_cmp` event such as `P(X < c)`), route the MC
  fallback through Phase B's relative stopping rule so it inherits a relative
  `(ε,δ)` bound instead of a fixed, opaque sample budget. This is the genuine
  improvement and the natural reuse of B on the continuous surface.
- *Leave moments best-effort.* A moment can be ≈ 0 or negative, where a
  *relative* bound is ill-posed; keep best-effort fixed-budget MC there, but
  labeled (per the first bullet) and with a user-tunable budget. Do not pretend
  a relative guarantee the estimator cannot honour.

### Phase F — routing + hazardous warning (ties it together)

The dispatcher that reads the Gain-2 classifier and the skeleton-safety bit and
picks the mechanism — the user never names it.

- On the approximate surface, for each HAVING cmp / query verdict:
  `MIN/MAX-easy` → Phase C; `SUM/MIN-MAX-hard` **and** safe skeleton → Phase D;
  everything else with `p ≥ 1/poly` → Phase B; `hazardous` (unsafe skeleton +
  SUM/AVG/COUNT(DISTINCT), or `SUM =`/`≠`) → `provsql_warning` "no FPRAS exists;
  returning a best-effort estimate / refuse" (decide refuse-vs-best-effort —
  leaning refuse, to keep the surface honest).
- The skeleton-safety gate (`safe_query_skeleton_is_hierarchical`, built under
  Gain 4, currently unused) becomes load-bearing here. The closed-form pre-pass
  (`runAggMarginalEvaluator` et al.) runs **first** on every surface as exact
  variance reduction: independent cmps resolve exactly and never reach the
  sampler.

**Dependency order:** A → B (B needs the shared primitive) → {C, F} → D → E.
A + B + C + F is a complete, shippable relative surface for everything except
SUM-hard-but-safe; D is the research slice that closes that cell; E is a small
independent transparency change on the continuous surface (and reuses B for the
probability case).

## Implementation observations

- The pre-pass slot, sound-only contract (numeric Bernoulli `gate_input`,
  meaningless to symbolic semirings), and `ref_count`-style privacy gating are
  all established by `runAnalyticEvaluator` and `runCountCmpEvaluator`; new
  per-aggregate evaluators should reuse that contract and the
  `provsql.cmp_probability_evaluation` umbrella GUC, not invent a new path.
- The absorptive-vs-non-absorptive split already in `pw_from_cmp_gate`
  (`S.absorptive()`) is the correct gate for the *symbolic* closed forms; the
  marginal-vector probability forms here are a parallel, probability-only track
  and should not disturb the semiring evaluation that non-probability semirings
  rely on.
- Bibliography: `website/_bibliography/references.bib` has the surrounding
  probabilistic-DB canon (`DalviS12`, `JhaS11`, `AmsterdamerDT11`,
  `GatterbauerS15`, the 2011 Suciu et al. textbook) but **not** this HAVING
  paper. Add `Re/Suciu 2009 (VLDB J.)` — and optionally the DBPL 2007
  conference version — when any of the above lands, so the docs can cite it.
