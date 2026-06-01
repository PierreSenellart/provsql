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

This section records the **theory** of the approximation half of the paper
(`~senellar/s00778-009-0151-4.pdf`, §5–6). The *architecture* that consumes it
(the three user-facing paths, the method catalog, the chooser) is
[Architecture: the probability method catalog and the three-path chooser](#architecture-the-probability-method-catalog-and-the-three-path-chooser)
below.

**Three user-facing paths, each a tolerance the user grants.** The user does not
pick an algorithm (a DBMS does not ask you to pick a join implementation); the
user grants a *tolerance* and the chooser selects the method. Named methods stay
available as an `EXPLAIN`-level escape hatch for debugging / benchmarking.

- *exact* — tolerance `(0,0)`, the true probability.
- *relative* — `(1±ε)p` with confidence `1−δ`.
- *additive* — `|p̂−p| ≤ ε` with confidence `1−δ`; cheaper (Hoeffding `N =
  O(ln(1/δ)/ε²)`, **independent of `p`**, bounded as `p→0`), so it is the
  *robust* choice for rare events where relative is hopeless. A legitimate
  informed choice, not a downgrade.

A relative `(1±ε)p` result has `|p̂−p| ≤ εp ≤ ε`, and an exact result satisfies
every tolerance, so the admissible method sets nest: **exact ⊂ relative ⊂
additive**. The path fixes the admissible set; the chooser returns the cheapest
member — so *every* path can return exact when exact is cheapest, and the
relative path can return an additive-tight relative result, etc.

*Continuous-RV caveat.* The continuous-RV moment / probability functions
(`expected`, `variance`, `rv_sample`, …) are an **approximate-by-nature**
surface: continuous distributions usually have no closed form, so MC is
unavoidable. Their contract is not "never approximate" but "the user can always
tell whether the answer is closed-form or MC." Today the RV path falls back to
`monteCarloScalarSamples` *silently* (`provsql.rv_mc_samples` opt-out); the fix
is **transparency** (signal the fallback), not refusal — see *RV transparency*
in the phased build.

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
5. **The method catalog + three-path chooser** — **the main open item.** The
   theory is *Gain 3* (three paths; relative-not-additive; whole-query via the
   propagation calculus; the shared per-world cmp primitive; the safe-plan
   FPTRAS); the build is *Architecture: the probability method catalog and the
   three-path chooser* below — one cost-aware chooser over a catalog of method
   objects, replacing both the rigid exact `try/catch` ladder and the
   `if/else if` method dispatch, with exact / additive / relative as user-granted
   tolerances. **Phase 1 (catalog skeleton + port the existing exact methods, a
   behavior-preserving refactor) is the natural starting point — it is shared
   infrastructure that improves the exact path before any FPRAS exists.**
   Supersedes the earlier "auto-route to karp-luby" framing, which was wrong on
   two counts: karp-luby cannot consume a threshold DNF, and a static
   trichotomy-class switch is the wrong shape (admissibility from the trichotomy,
   *selection* from a cost heuristic).
6. **Gain 1, COUNT(DISTINCT) arm** — **closed**: already realised by the
   `COUNT(DISTINCT)` GROUP-BY rewrite + `runCountCmpEvaluator` (see the
   COUNT(DISTINCT) note under Gain 1); the HAVING gap was fixed. No dedicated
   arm needed.

## Architecture: the probability method catalog and the three-path chooser

The realization that reorganizes everything below: **the exact default chain and
the relative/additive surfaces are one problem.** Today `probability_evaluate`'s
`method==""` default is a hardcoded `try/catch` ladder (independent →
inversion-free → `makeDD`, with `possible-worlds` / `monte-carlo` reachable only
by name), and the method dispatch is a ~150-line `if/else if` chain on
method-name strings. Both are degenerate, rigid portfolios. The design here
replaces them with **one cost-aware chooser over a catalog of method objects**,
parameterized by the user-granted tolerance.

The portfolio + cost-model principle is ProApproX (Souihli & Senellart,
*"Optimizing Approximations of DNF Query Lineage in Probabilistic XML,"* ICDE
2013): no single algorithm wins; the best one is a function of the formula's
features; different methods can evaluate different parts. **But ProApproX is
DNF-era** — flat formulas, simpler blocks — and several of its specifics are
*wrong* on circuits; the principles survive, the feature vector and cost
expressions are rebuilt circuit-native (below).

### Three paths, one admissibility lattice

The three user-facing paths (*exact* / *relative* / *additive*, see Gain 3) are
tolerance grants, not algorithm picks. They nest: **exact ⊂ relative ⊂
additive** as admissible method sets. The path constructs a
`Tolerance{kind, ε, δ}`; admissibility filtering does the rest. A user on the
exact path therefore *never* receives an estimate (only ε=0 members are
admissible); approximation happens only when the user grants tolerance > 0.
Named-method requests bypass the chooser entirely (`by_name`) — the
`EXPLAIN`-level escape hatch for debugging / benchmarking.

### The method catalog (Strategy + registry)

Mirror the existing tool registry (`tool_registry_sync.h`, the `kind='kcmcp'`
entries): each method is a first-class object that **declares its own** guarantee
/ applicability / cost, instead of that knowledge being smeared across inline
conditionals (the `karp-luby` DNF-shape check, the RV guard, the monotonicity
assumptions) and a string-switch dispatcher.

```cpp
struct Tolerance { enum class Kind { Exact, Relative, Additive };
                   Kind kind; double epsilon, delta; };   // exact ⇒ (0,0)

struct CircuitFeatures {                  // computed once, tiered/lazy
  size_t n_inputs;
  bool decomposable, dnf_shaped, monotone, inv_free_cert,
       aggregate_shaped, safe_skeleton;
  std::optional<int>    treewidth_proxy;      // tier-2: only when amortized
  std::optional<double> prob_lower_bound;     // MulMC vs coverage
};

class ProbabilityMethod {                 // Strategy: one subclass per method
public:
  virtual std::string name() const = 0;
  virtual bool      applicable(const CircuitFeatures&, const Tolerance&) const = 0; // correctness gate
  virtual Guarantee guarantee (const Tolerance&) const = 0;                          // exact ⇒ ε=0
  virtual double    estimated_cost(const CircuitFeatures&, const Tolerance&) const = 0; // selection
  virtual double    evaluate(BooleanCircuit&, gate_t, const Tolerance&,
                             std::optional<Budget> = {}) const = 0;                  // may honour a soft budget
};

class MethodCatalog {                     // registry, mirrors the tool registry
  std::vector<std::unique_ptr<ProbabilityMethod>> methods_;
public:
  static MethodCatalog& instance();
  void register_method(std::unique_ptr<ProbabilityMethod>);
  Result choose_and_run(BooleanCircuit&, gate_t, const Tolerance&) const;  // the chooser
  const ProbabilityMethod* by_name(const std::string&) const;             // debug escape hatch
};
```

SE payoff:

- **Open/closed** — a new method = a new class + one `register_method`; the
  dispatcher never changes again. Kills the `if/else if` chain and `makeDD`'s
  nested sub-ladder.
- **Single source of truth** — each method owns its guarantee / applicability /
  cost; the scattered inline checks move into the one class that cares.
- **Separation** of the three concerns we kept circling — admissibility
  (correctness), cost (selection), execution — as distinct members, not tangled
  `try/catch` control flow.
- **Composition over catalogs** — `CompilationMethod` is parameterized by a
  tool-registry entry, so a `d4` / `c2d` / KCMCP tool *becomes* a method by
  wrapping; the two catalogs compose instead of `wmc` / `weightmc` /
  `compilation` special-cases.
- **Testability** — each method's predicate / cost / evaluate is unit-testable;
  the chooser tests against mock methods; the current ladder becomes the baseline
  assertion (chooser at `(0,0)` must reproduce it).
- Optional, consistent with "a catalog like the tools": expose `provsql.methods`
  as a SQL-visible view (name, guarantee kind, applicability summary) for Studio
  and for users to *see* the portfolio without choosing from it.

### Circuit-native features (ProApproX rebuilt)

ProApproX keyed on `(N vars, m clauses, L size, ℓ = max clause prob)`. On a
*shared circuit DAG* those are the wrong axes — and using them would actively
mislead:

- The `2^N` / `2^m` cost formulas reject the cheapest method on low-**treewidth**
  circuits (1000 vars, treewidth 3 = linear exact). Treewidth (min-fill proxy),
  not var / clause count, is the exact-tractability axis — and ProvSQL *has* it
  (d-DNNF / tree-decomposition) where ProApproX deliberately disallowed Shannon
  expansion.
- ProApproX *recovered* independence structure from a flat DNF (§V). A circuit
  *already is* that tree (plus / times / monus = `⊽ / ⊼ / ⊕`). The hard part
  inverts: not "how to factor" but **"where can I certify a gate is genuinely
  independent"** (children leaf-disjoint across the DAG) — the read-once /
  safe-query / `commonLeaves`-`independenceBlocks` question, where ProvSQL's
  tooling is richer than anything ProApproX had.
- coverage / Karp-Luby assumes **monotonicity**; a circuit with **monus** is
  non-monotone, so monotonicity is an *applicability condition*, not a magnitude.
- `ℓ = max clause prob` presupposes clauses; on an unexpanded circuit MulMC needs
  a **structural** lower bound on `p`, or it is confined to the DNF-shaped track.

| axis | role | ProApproX had it? |
|---|---|---|
| treewidth (min-fill proxy) | exact-compilation gate | no |
| independence-certifiability (read-once / leaf-sharing) | where decomposition + propagation are valid | partially |
| monotonicity (monus present?) | coverage-FPRAS applicability | no (monotone-only) |
| `p` lower bound (structural) | MulMC vs coverage | yes (clause-based) |
| aggregate-shape + skeleton-safe | aggregate-track / FPTRAS gate | no |
| small `N` | naïve fallback | yes |

Features are **tiered**: tier-1 cheap (`n_inputs`, connectivity, dnf-shape,
cert-bit) computed always; tier-2 expensive (treewidth proxy, independence
certificates) only when the circuit is large enough that planning amortizes.

### The chooser, and the planning-cost discipline

`choose_and_run` is generic:

```
features   = compute_tier1(circuit)
admissible = methods_ | filter(.applicable(features, tol))       // correctness ∧ guarantee ≥ tol
ranked     = admissible | sort_by(.estimated_cost(features,tol)) // realize tier-2 features lazily if ranking needs them
return speculative_run(ranked, budget = estimated_cost(cheapest GUARANTEED fallback))
```

Principles that shape it:

- **Minimize planning + execution, not just execution.** The optimizer is not
  free. Hence: tiered features; *below a size threshold, do not plan* — run the
  cheapest exact method directly (a DBMS does not optimize `SELECT 1`); a bounded
  deterministic heuristic, never a plan search (ProApproX itself rejected its own
  sampling-based plan exploration on exactly this ground).
- **Speculative execution as planning.** Rather than *predict* whether exact is
  cheap (pay prediction cost, risk misprediction), spend a bounded slice
  *attempting* it. Finishes within budget ⇒ planning cost = execution cost, zero
  waste, the attempt *was* the plan. Times out ⇒ only the budget is spent. The
  rigid ladder's `try/catch` already half-does this; formalize it with a budget.
- **Speculation budget = cost of the guaranteed fallback.** On the
  relative / additive paths the exact-attempt timeout = the (cheaper, bounded)
  cost of the approximate method you would otherwise run. Then total ≤ ~2× the
  pure-approximate cost worst-case, and you win outright when exact finishes
  inside the window. Cheap tier-1 features *prune* obviously-doomed attempts
  (skip tree-decomposition when the treewidth proxy is clearly huge) so the
  budget is not wasted.
- **Exact is the ε=0 portfolio member.** It satisfies every tolerance, so it is
  admissible on every path and wins purely on cost when the circuit is easy —
  "the relative method returns exact when that is cheaper" is not a special case,
  it is the lattice.
- **Behavior-preserving baseline.** The current ladder is battle-tested; the
  chooser must reproduce it at `(0,0)` (the ladder is a known-good *lower bound*)
  and improve only by *adding* cheaper-when-certified options (small-N
  possible-worlds, treewidth-gated tree-decomposition), with the timeout guard as
  the safety net against a feature heuristic that guesses wrong.

### Guarantee propagation (decompose at certified-independent gates only)

The whole-query `(ε,δ)` decomposes soundly across the circuit using ProApproX's
propagation calculus (§VI) — but **only at independence-certified gates**:

- independent-OR (`⊽`, a `gate_plus` whose children are leaf-disjoint):
  `ε = ε₁+ε₂+ε₁ε₂ ≈ max`; mutex-OR (`⊕`): `max(ε₁,ε₂)`; independent-AND (`⊼`):
  `ε₁`. `δ` composes as `1−δ = (1−δ₁)(1−δ₂)`.
- A `gate_plus` whose children **share leaves** is *not* `⊽`; the rule is invalid
  there. So the unit of guarantee is a **maximal correlated component** (the
  circuit-native "leaf"), and the rules glue components only at certified
  boundaries. This is the precise form of "whole query, not per cmp": correlated
  cmps stay in one component and are sampled jointly; independent structure is
  composed exactly. It reconciles with the earlier worry — a cmp correlated with
  the surrounding lineage is never split off, so it stays consistent.
- **Exact components loosen the budget.** A component cleared exactly contributes
  `ε=0`, so propagation reallocates its budget to approximate siblings (larger ε
  ⇒ fewer samples ⇒ cheaper). This is the per-part payoff and the natural home of
  the *cost-model-later* upgrade (per-node optimization of the ε-split); the
  heuristic-now version applies one method per component.
- **monus propagation is not in the paper.** ProApproX's `⊕` is restricted
  mutex-OR, not general set difference. `Pr(monus(a,b)) = Pr(a)(1−Pr(b))` under
  independence has its own ε-propagation (and an unsound-under-sharing caveat)
  that must be derived — a genuine TODO the DNF-era paper does not cover.

### The portfolio members

Method classes mapped to existing code (✓ exists) and new work (∆):

*Exact (ε=0; admissible on every path):*

- `PossibleWorldsMethod` ✓ (`possibleWorlds`) — applicable when `n_inputs` small.
- `IndependentMethod` ✓ (`independentEvaluation`) — decomposable circuits.
- `InversionFreeMethod` ✓ (`StructuredDNNFBuilder`) — `inv_free_cert`.
- `CompilationMethod` ✓ (`makeDD` / tree-decomposition / compiler tools) —
  parameterized by a tool-registry entry; cost keyed on the treewidth proxy.
- `AggMarginalExactMethod` ✓ (`runAggMarginalEvaluator` et al.) — the HAVING
  marginal-vector engine; the front-running exact arm for aggregate-shaped
  circuits, already self-gating on circuit-level laminarity.

*Relative (admissible on relative + additive paths):*

- `StoppingRuleMcMethod` ∆ — Dagum–KLMR `(ε,δ)`-relative whole-circuit MC; the
  universal relative fallback (when `p ≥ 1/poly`). Reuses
  `BooleanCircuit::karpLubyStopping`, driven by `MonteCarloSampler::evalBool` so
  RV / `gate_cmp` / `gate_agg` are in scope. The correctness oracle the other
  relative members are diffed against.
- `CoverageFprasMethod` ✓-ish (`evaluate_karp_luby`) — monotone DNF only.
- `MinMaxKarpLubyMethod` ∆ (thin) — MIN/MAX easy direction (`MAX≥k` / `MIN≤k`)
  reduces to `P(∃ present tuple with y θ k)` = a monotone UCQ → hand the existing
  coverage FPRAS that small DNF, on **any** skeleton (Thm 8). Cheapest relative
  win.
- `MulMcMethod` ∆ (enrichment) — additive MC made multiplicative via a lower
  bound `ℓ` (`ε_add = ℓε`); cheaper than coverage when `p≈1`. Needs a structural
  `ℓ`.
- `AggFptrasMethod` ∆ (research slice) — SUM / MIN-MAX-hard over a **safe
  skeleton**; see *The SUM-safe FPTRAS* below.

*Additive (admissible only on the additive path):*

- `MonteCarloAdditiveMethod` ✓ (`monteCarlo`) — Hoeffding, `p`-independent.

*Sieve enrichment:*

- `SieveMethod` ∆ — exact inclusion-exclusion, `2^m` cost, best when the clause
  count `m` is tiny; DNF-shaped track only.

### Prerequisite: the shared per-world cmp primitive

Any sampler that touches a `gate_cmp` (`StoppingRuleMc`, `AggFptras`) needs
**one** definition of "value of `agg θ k` in a world," or it drifts from the
exact expansion. Today there are two — `pw_from_cmp_gate` (the canonical
expansion, with the numeric/float scaled-integer domain and the empty-group
exclusion) and `MonteCarloSampler`'s `gate_agg` / `gate_cmp` arms. They can
disagree (`HAVING sum(x) >= -5` on an all-absent group: expansion excludes the
empty group = false; a naive sampler computes `0 ≥ −5` = true). Extract:

```cpp
bool eval_cmp_in_world(cmp_gate, /*present:*/ predicate-over-leaves);
```

reproducing the expansion's edge conventions exactly (empty-group exclusion,
`info2`-typed scaled-integer domain via `parse_decimal_scaled` / `rescale_to`),
and re-express both callers in terms of it (`pw_from_cmp_gate` = over all worlds
= the DNF; the sampler = over the one sampled world). Lives in
`CmpEvaluatorCommon.{h,cpp}` next to `matchAggCmp`. **This is the gating
prerequisite for every sampler member.**

### The SUM-safe FPTRAS (AggFptrasMethod, the research slice)

SUM (and MIN/MAX hard direction) over a **safe skeleton** — gated by
`safe_query_skeleton_is_hierarchical` (the read-only detector built under Gain 4,
currently unused; this is what makes it load-bearing); unsafe skeleton ⇒
hazardous. Thm 9 / Alg 6.3.1:

1. *Rounding.* `τ^R(y) = ⌊(n²/k)·y⌋`, rounded-sum semiring `S_{n²+1}` (size
   `n²+1`, polynomial). Compute the rounded-sum PMF **exactly** with the
   marginal-vector recursion already built (`sumPMF` / `countPMF`, well under
   `kMaxSumSupport`), driven by the **query-level** safe plan from
   `find_hierarchical_root_atoms`.
2. *Random-world generator* (Alg 5.2.1, the one genuinely new subroutine):
   `sampleWorldWithValue(plan_node, target_s)` walks the safe-plan tree
   **top-down**, splitting `target_s` among children ∝ marginal-vector entries
   (`⊕`: `s₁+s₂=s` w.p. `m^φ₁[s₁]·m^φ₂[s₂]/m^φ[s]`; `⊗`: `s₁·s₂=s`; `⊔`: route
   all to one branch). **This is the bottom-up
   `countPMF` / `sumPMF` / `decomposeProduct` recursion run in reverse** — same
   tree, same vectors, the draw descends instead of folding up. Alg 5.2.2 fills
   off-plan tuples.
3. *Accept-test* (Lemma 7): draw a rounded value `∝ μ(rounded)`, draw a world,
   accept iff it is an *original* solution; `m = O(n·β⁻¹·ε⁻²·log δ⁻¹)` samples →
   relative `(ε,δ)`. This is exactly the regime the exact engine *bails on*
   (`kMaxSumSupport`, Remark-3 pseudo-poly), so the FPTRAS is its principled
   replacement. New code `src/AggFptras.{h,cpp}`, consuming `AggMarginalEvaluator`
   + `safe_query.c`. (The `(α,θ)` × skeleton admissibility map is the table at
   the end of Gain 3.)

### Phased build (behavior-preserving)

Status legend: ✅ done · ◑ partial · ⏳ deferred (with reason).

1. ✅ **Catalog skeleton + port exact methods.** `src/ProbabilityMethod.h` +
   the catalog machinery in `probability_evaluate.cpp`: `ProbabilityMethod`
   (Strategy), `MethodCatalog` (registry), `EvalContext`; every historical
   dispatch branch is now a method class, the empty method runs
   `chooseAndRun` (the independent → inversion-free → compilation ladder) and a
   named method runs `byName`. Behaviour-preserving (188 tests green at the
   refactor). Commit `1f4970a`. *Not yet added: the behaviour-changing small-N
   `PossibleWorlds` / treewidth-gated `Compilation` chain entries (a separate
   commit, since they change the reported method).* `CircuitFeatures`, the cost
   heuristic and the lazy-Boolean-build (so GenericCircuit-level methods fold
   into the catalog) are still TODO.
2. ◑ **Shared per-world cmp primitive.** The value-level core
   `agg_cmp_holds_in_world` (empty-group exclusion centralised) is in
   `subset.{hpp,cpp}`, with `enumerate_exhaustive` refactored onto it. Commit
   `f4ee731`. *Deferred:* the gate-level `eval_cmp_in_world` wrapper + wiring
   the `MonteCarloSampler` agg arm through it (the sampler already has a working
   agg/cmp arm, so this is a unification/parity hardening, not a blocker).
3. ◑ **Relative path, core.** ✅ `StoppingRuleMc` shipped as the `stopping-rule`
   method (`monteCarloRVStopping`, Dagum-KLR over `evalBool`) — the universal
   relative FPRAS, handles non-DNF / RV / agg circuits karp-luby cannot. Test
   `stopping_rule.sql`, commit `3af132d`. ⏳ `MinMaxKarpLubyMethod` (MIN/MAX-easy
   → UCQ → karp-luby) deferred: an efficiency optimisation StoppingRuleMc
   already covers functionally.
4. ⏳ **Additive path.** Functionally already served by the existing
   `monte-carlo` method. The substantive piece is the three-path
   AUTO-SELECTION surface (a `relative`/`additive`/`exact` tolerance the chooser
   maps to a method) — design-heavy, changes user-facing surface, overlaps the
   cost-model chooser. Deferred for review.
5. ⏳ **`AggFptrasMethod`** — the SUM-safe FPTRAS (Dyer rounding + top-down
   random-world generator + accept-test). Research-grade; high risk of subtle
   statistical bias; needs the `AggMarginalEvaluator` internals exposed and the
   skeleton-safety gate wired. **Deferred for collaborative implementation /
   review rather than autonomous coding.**
6. ✅ **RV transparency.** RV moment MC fallback now emits a `verbose>=5`
   NOTICE (`Expectation.cpp` `mc_samples_or_throw`): closed-form stays quiet, MC
   fallback announces "an approximation, not an exact moment". Test
   `continuous_mc_transparency.sql`, commit `5be02aa`. *Deferred enrichments:*
   route the RV *probability* case `P(X<c)` through `stopping-rule`; decide
   always-on vs the current verbose-gated signal (one-line change).
7. ◑ **Enrichments + cost-model-later.** ✅ `SieveMethod` added (exact
   inclusion-exclusion, `BooleanCircuit::sieve`, demonstrates the catalog's
   open/closed: new method = one class + one `registerMethod`). Test
   `sieve.sql`, commit `0ad8f83`. ⏳ `MulMcMethod` (needs a structural `p`
   lower bound) and the calibrated cost-model / per-node ε-split seam deferred.

**Delivered (autonomous pass):** the method-catalog architecture (1), the shared
per-world primitive core (2), the universal relative FPRAS (3), RV transparency
(6), and the sieve exact method (7) — five commits, all behaviour-preserving or
additive, full regression green throughout (191 tests). **Open for review:** the
three-path auto-selection surface (4), the SUM-safe FPTRAS (5), and the cost
heuristic / treewidth features / lazy-Boolean catalog folding (1).

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
  Also add `Souihli/Senellart 2013 (ICDE)` (ProApproX), the source of the
  portfolio + cost-model principle behind the method catalog, when the chooser
  lands.
