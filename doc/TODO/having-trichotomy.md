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

### Gain 3 — FPRAS routing for the apx-safe class

ProvSQL recently gained the **karp-luby** DNF FPRAS (commit `e468f64`) and a
shared `(eps, delta)` argument grammar (`parse_eps_delta`, commit `fddb28b`,
both in `src/probability_evaluate.cpp`). The apx-safe class is precisely where
an FPRAS is the *right, guaranteed* tool rather than a heuristic: exact is
`#P`-hard, but a fully polynomial randomized approximation exists.

- Gain: once the classifier (Gain 2) labels a query apx-safe, route to an FPRAS
  with an actual soundness argument, instead of bare `monte-carlo` (whose
  `~1/p` sample cost is hopeless for rare events).
- Honest gap to record: the paper's apx-safe FPTRAS is a **specialized**
  sampler — it draws a uniformly random possible world *satisfying the
  predicate* via the aggregate structure — which is stronger than ProvSQL's
  generic monotone-DNF karp-luby. So "we have *an* FPRAS" ≠ "we have the
  paper's FPRAS"; karp-luby covers the apx-safe cases whose lineage is
  DNF-shaped, and the paper's construction is the target for the rest.
- For `hazardous` queries the sound action is a `provsql_warning` that no
  efficient approximation exists (today ProvSQL silently grinds the exponential
  enumeration or an uninformative MC).

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
      non-laminar triangle fails. Still open: contributors with
      `gate_plus`/`gate_monus` (UNION/EXCEPT lineage), AVG, and the
      baked-certificate path for BID disjoint-block `⊥` structure the circuit
      cannot self-certify (Phase 1 above).
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
5. **Gain 3, principled apx-safe routing** — mostly classifier wiring on top of
   the existing karp-luby method; the paper's specialized FPTRAS is a later,
   research-grade slice.  **Next open item** — the classifier (3) now produces
   the `apx-safe` verdict, so this is the natural follow-up: have the
   probability path auto-route an `apx-safe` HAVING predicate to karp-luby
   (and warn on `hazardous`) instead of grinding the exponential fallback.
6. **Gain 1, COUNT(DISTINCT) arm** — **closed**: already realised by the
   `COUNT(DISTINCT)` GROUP-BY rewrite + `runCountCmpEvaluator` (see the
   COUNT(DISTINCT) note under Gain 1); the HAVING gap was fixed. No dedicated
   arm needed.

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
