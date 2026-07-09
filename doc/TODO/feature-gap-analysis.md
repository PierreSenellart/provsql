# Feature-gap analysis: ProvSQL vs. the probabilistic- and provenance-database literature

A literature review of probabilistic-database systems and provenance-enabled
databases (implemented systems and theory papers), read against ProvSQL's
current surface, to identify features that are **genuinely missing** and would
deserve implementation. For each candidate: the gap, the use cases it unlocks,
implementation complexity in ProvSQL's circuit architecture, and a demand
signal. A prioritised roadmap closes the document.

This is a **to-do / design-rationale** document, not a record of shipped work.
Where a candidate is already tracked in a sibling plan, it is cross-referenced
rather than re-argued.

## How this was derived

Fan-out web search over five angles (probabilistic-DB systems; provenance
systems and provenance querying; approximation/enumeration/expressiveness
theory; recursion/factorization/richer semirings; recent surveys and demand
evidence), ~24 primary sources fetched, ~120 falsifiable claims extracted, of
which a subset were adversarially cross-checked (3-vote) before a model-quota
interruption; the rest are recorded as sourced-but-unverified and are treated
here as **corroborating**, not load-bearing. Every candidate below is anchored
on at least one primary source (see *Sources*).

---

## The feature-gap table

Complexity is **relative to ProvSQL's existing machinery** (circuit DAG,
knowledge compilation, safe-query rewriter, Shapley, RV surface): *Low* = fits
an existing evaluator/registry; *Med* = new evaluator or planner arm reusing
current data structures; *High* = new algebraic object, ABI, or algorithm class.
Rows are ordered by tier.

| # | Candidate feature | Present elsewhere | Fit to ProvSQL circuit model | Complexity | Demand | Tier |
|---|---|---|---|---|---|---|
| 1 | **Tractable responsibility measures (WSMS)** + **approximate Shapley with guarantees** | Bienvenu et al. (PODS'25); Deutch et al. | High — extends existing Shapley/Banzhaf + KC | Low–Med | High (survey explicitly asks) | **P1** |
| 2 | **Why-not / missing-answer provenance** | PUG, GProM, summarisation systems | Medium — dual-indeterminate polynomials `S∞[X,X̄]`; monus already present | High | High | **P2** |
| 3 | **Attribute-level / correlated-attribute uncertainty** (joint pdfs, multivariate Normal, copulas) | Orion 2.0, MCDB | Medium — new `rv`-family gates; partly planned | High | Med–High | **P2** |
| 4 | **FPRAS for bounded-hypertree-width sj-free CQ** (multiplicative, combined complexity) | Meel et al. (PODS'23) | Medium–High — new approximation over lineage | High | Med | **P2** |
| 5 | **Open-world PDB semantics** (interval bounds for absent facts) | Ceylan–Darwiche–Van den Broeck (KR'16) | High — `LiftO^R` is linear-time atop safe-query path | Low–Med | Med (NELL/YAGO/KB workloads) | **P3** |
| 6 | **Provenance minimal factorisation / compression** | Makhija–Gatterbauer; factorised DBs | Medium — ILP/max-flow over circuit; NP-c but bounded | Med | Med | **P3** |
| 7 | **Dissociation as a complementary bound** (non-branching PTIME leaf/fallback for the shipped d-tree engine) | Gatterbauer–Suciu | High — alternative leaf bound in `dtreeBoundsCircuit` | Med | Low (refines a shipped feature) | **P3** |
| 8 | **Top-k / ranking under uncertainty** (U-Topk, U-kRanks) | Soliman–Ilyas; UA-DB | Low — new operator, reuses probability eval | Med | Med | **P4** |
| 9 | **Deletion propagation / GDPR erasure** (min source-side-effect) | Buneman–Cheney–Tan; Kimelfeld et al. | Medium — uses provenance + update provenance | Med | Med (compliance) | **P4** |
| 10 | **Incremental probability maintenance under updates** | Berkholz et al.; dynamic lifted inference | Low for safe UCQs; recompute today | Med–High | Med | **P4** |
| 11 | **Transaction-level provenance / reenactment** (function-exposed, no new syntax) | GProM MV-semirings | Medium — a function + MV-semiring history | High | Med | **P4** |

---

## Detailed analysis

### Tier P1 — best architecture fit, high value, near-term

#### 1. Tractable responsibility measures and approximate Shapley

**Gap.** ProvSQL computes Shapley and Banzhaf **exactly** via knowledge
compilation — #P-hard in data complexity even for simple CQs, which caps
scalability. Two complementary additions are well-charted:

- **WSMS** (weighted sums of minimal supports; Bienvenu, Figueira & Lafourcade,
  PODS'25): a family of responsibility measures, each expressible as the Shapley
  value of a suitable game (so the game-theoretic justification survives), with
  **PTIME data complexity for all unions of conjunctive queries** — precisely
  where exact Shapley is intractable.
- **Approximate Shapley with guarantees**: any PTIME-answerable query admits an
  additive FPRAS by permutation sampling; all UCQs satisfy the "gap property"
  enabling a multiplicative FPRAS. A recent Shapley-in-databases survey
  *explicitly calls for* wiring the knowledge-compilation Shapley algorithm and
  its approximations into "PostgreSQL extensions such as ProvSQL", noting a
  bibliometrics application that recomputed Shapley from the definition for want
  of exactly this.

**Use cases.** Fact attribution / responsibility at interactive latency on
workloads where exact Shapley times out; explanation UIs (Studio) that rank
contributing tuples; the bibliometrics / credit-assignment use case named in the
survey.

**Complexity — Low–Med.** The exact path and the compiled circuit already exist;
WSMS is a different weighting over minimal supports, and the FPRAS is a sampling
loop over the same circuit with a Chernoff–Hoeffding stopping rule mirroring the
MC probability path. Largest new piece is the WSMS tractable evaluator for UCQs.

### Tier P2 — high value, larger scope

#### 2. Why-not / missing-answer provenance

**Gap.** ProvSQL answers "why is this tuple in the output" (how/why/where
provenance, monus for difference) but not "**why is this tuple absent**". PUG
(GProM extension) is billed as the first practical system for both why and
why-not over first-order queries with negation, via a graph model equivalent to
provenance games, implemented purely by Datalog→RA→SQL rewriting on a stock
backend — the same architectural style as ProvSQL's planner hook. The algebra is
compatible: the most general provenance semiring for negation is the
dual-indeterminate `N∞[X,X̄]`, and PUG extracts `N[X]` polynomials with dual
positive/negative indeterminates.

**Two hard truths to design around.** (a) Full why-not is combinatorially
infeasible to materialise — a single Airbnb-scale question has ~10¹⁵ failed
derivations, and summarisation work scales to 10⁶⁰–10⁸⁰ — so any ProvSQL feature
must compute only the **explanation relevant to the user's question** (PUG's
pruning over the active domain) and/or a **sampled/pattern summary**, never the
full negative provenance. (b) Why-not is intractable in general (minimal
successful reparameterisations are NP-hard).

**Use cases.** Debugging "my row didn't show up"; data-cleaning / integration
("which source fact must change for X to appear"); explanation panels.

**Complexity — High.** Needs a negation-aware provenance object (dual
indeterminates over the circuit), a firing-rule / provenance-game rewriting for
the negative side, question-directed pruning, and a summarisation layer. Best
scoped first to UCQ¬ over TID/BID inputs. This is a research + engineering
project, plausibly publishable.

#### 3. Attribute-level and correlated-attribute uncertainty

**Gap.** ProvSQL attaches uncertainty at **tuple granularity** (one UUID token
per tuple) plus per-value `random_variable` columns whose correlation arises
only through *shared circuit structure*. It has no schema-level way to declare a
**joint distribution over several uncertain columns** of one tuple. The
circuit-side machinery (multivariate Normal, copulas, stochastic processes) is
already the [`continuous_distributions.md`](continuous_distributions.md)
§A.2 / §D.1 / §D.2 roadmap; the *systems-side* pieces unique to this candidate
are:

- a schema-level declaration in the Orion 2.0 style (`DEPENDENT (xloc, yloc)` →
  a single stored joint pdf over the columns, feeding a shared RV root);
- Orion's "partial pdfs" unifying tuple-existence with value uncertainty (a
  distribution integrating to x<1 encodes existence probability 1−x);
- Orion's *physical-design* apparatus (uncertain indexes, uncertainty-aware
  selectivity/costing) — a separate, large, and lower-priority sub-gap.

**Use cases.** Sensor fusion (a 2-D Gaussian over (x,y) location), correlated
measurement errors, any model where two uncertain fields of one record are not
independent — currently only expressible by hand-building shared gates.

#### 4. FPRAS for bounded-hypertree-width self-join-free CQ

**Not a general approximation gap — a *combined-complexity* one.** ProvSQL
already offers guaranteed approximation on the #P-hard tail: alongside exact
knowledge compilation, the d-tree's deterministic *additive* bounds, and additive
Monte Carlo, it ships a **Karp-Luby FPRAS** (`karp-luby` method) giving *relative*
(1±ε) multiplicative guarantees with confidence 1−δ (the `Relative` tolerance in
`ProbabilityMethod.h`, chosen by the exact ⊂ relative ⊂ additive path selector).
So provably-(1±ε) answers are **present** — in **data** complexity. The specific,
narrower gap is **combined** complexity: Karp-Luby (like every intensional method)
runs on the *lineage*, which can be exponential in query length (a five-atom CQ
over a few hundred rows can yield a DNF with >10¹² clauses), so it is not
polynomial in the query size. Meel et al. (PODS'23) give an FPRAS for any
self-join-free CQ of bounded hypertree width polynomial in **both** query length
and database size — sidestepping the lineage entirely. No such FPRAS can exist for
*all* CQs (that would need NP ⊆ BPP), but bounded-hypertree-width covers most real
workloads (typically width ≤ 3).

**Use cases.** Guaranteed relative-error probabilities on queries whose *lineage*
is intractably large — i.e. many-atom / high-join queries where even Karp-Luby's
per-sample DNF evaluation is prohibitive.

**Complexity — Med–High, and partly foreign to the design.** This is an
extensional algorithm that deliberately avoids materialising the circuit ProvSQL's
whole pipeline produces; it is complementary to the shipped intensional stack
(exact / d-tree / Karp-Luby / Monte Carlo), not a replacement, and it is arguably
a critique of the lineage-based approach itself.

**Caveat — practical payoff unproven; treat as research, not an engineering win.**
The Meel et al. result is a *combined-complexity* guarantee; as with many FPRAS
constructions the polynomial degree and hidden constants may make it slower than
the shipped intensional methods on the workloads that actually arise, so it is not
obviously a practical speed-up over exact KC + Karp-Luby except on queries whose
lineage is genuinely intractable. Its value is the *guarantee where the lineage
blows up*, and whether that regime is common enough to pay for the machinery is an
open empirical question. Scope it as a **research** item (benchmark the crossover
against Karp-Luby before committing), not a routine P2 build.

### Tier P3 — valuable, research-flavoured

#### 5. Open-world PDB semantics

**Gap.** ProvSQL is closed-world tuple-independent: an absent ground fact has
probability 0, so a true query over a truncated knowledge base can evaluate to 0.
OpenPDBs (Ceylan et al.) give every absent fact probability in [0, λ], so
queries return **probability intervals**. Real KBs (NELL, YAGO, ReVerb,
PaleoDeepDive) violate CWA by truncating low-probability extractions — concrete
demand.

**Use cases.** Query answering / KB completion over incomplete extracted
knowledge bases with honest upper bounds instead of spurious zeros.

**Complexity — Low–Med for the tractable class.** The Dalvi–Suciu dichotomy
transfers unchanged; `LiftO^R` computes open-world upper bounds for safe UCQs in
**linear time**, so this layers cheaply on the existing safe-query/read-once
path. (Negation makes it genuinely harder — some queries jump to NP^PP — so
scope the first cut to monotone UCQs.) A good candidate precisely because it is
cheap where ProvSQL is already strong.

#### 6. Provenance minimal factorisation / compression

**Gap.** ProvSQL's circuit DAG shares subexpressions structurally but does not
compute a **minimal factorisation** of provenance. Finding the minimal-size
factorisation of self-join-free-CQ provenance is NP-complete — notably *easier*
than general Boolean minimisation (Σ₂ᵖ) — and a large PTIME-factorisable class
strictly extends read-once/hierarchical (what the safe-query rewriter covers). A
single ILP (via variable-elimination orders) solves it exactly; a max-flow /
LP-relaxation gives an efficient approximation exact on all known PTIME cases.

**Use cases.** Smaller persisted circuits; faster downstream evaluation; more
readable exported/visualised provenance (Studio).

**Complexity — Med.** A factorisation pass over the Boolean circuit (ILP +
max-flow fallback), naturally a companion to `simplify_on_load`.

#### 7. Dissociation as a complementary bound (refines the shipped d-tree engine)

**Not a gap in the strict sense** — deterministic anytime bounds already ship
(the d-tree engine, `src/DTree.cpp`). Dissociation would add a *different*
bounding mechanism. The d-tree handles a multiply-occurring variable by Shannon
expansion, which branches and can blow up; when its subproblem budget is
exhausted the frontier falls back to the cheap Bonferroni/union leaf bound, loose
on high-inversion circuits. Dissociation (Gatterbauer & Suciu) instead **relaxes**
shared variables into independent copies, giving a single **non-branching PTIME**
one-sided bound; scaled dissociation tunes the relaxation weights and its lower
bounds provably dominate all "oblivious" model-based ones (a reported 42–81%
aggregate-error reduction over model-based branch-and-bound on hard
non-hierarchical TPC-H / YAGO3 queries).

**Value is narrow and empirical.** On circuits with heavy variable sharing where
the d-tree's Shannon frontier is budget-starved, a dissociation pass could supply
a tighter deterministic bound at guaranteed polynomial cost. But neither family
strictly dominates the other, so the win is workload-dependent, and the
"safe iff one plan" property adds nothing the safe-query rewriter does not
already give.

**Complexity — Med.** A relaxation pass over the Boolean circuit plus the
gradient-descent weight choice (non-convex, but the authors report no bad local
optima), plugged in as an alternative leaf bound inside `dtreeBoundsCircuit`. Do
only if a workload shows the d-tree's Shannon frontier is the bottleneck.

### Tier P4 — niche, larger, or off-core

- **Top-k / ranking under uncertainty (8).** U-Topk (max-probability k-vector
  across worlds) and U-kRanks are *distinct operators* that traditional top-k
  cannot deliver and that probability evaluation does not give for free; the
  representation (TID + exclusiveness rules) is one ProvSQL already supports via
  `set_prob`/`repair_key`, so the gap is the ranking semantics + a
  state-space-search operator, not the data model. Med complexity, moderate
  demand.
- **Deletion propagation / GDPR erasure (9).** Minimum source-side-effect
  deletion is NP-hard (set-cover-hard) for PJ/JU views but PTIME for
  key-preserving (foreign-key) joins and chain joins via flow networks. ProvSQL
  already has the provenance and update-provenance substrate; a restricted
  key-preserving feature is implementable and compliance-relevant.
- **Incremental probability maintenance (10).** Safe UCQs admit constant-work
  probability updates per single-tuple insert/delete (dynamic lifted inference);
  ProvSQL recomputes from the circuit. Aligns with update provenance but needs a
  dynamic data structure distinct from static circuit compilation. Med–High.
- **Transaction-level provenance / reenactment (11).** ProvSQL tracks
  per-statement INSERT/UPDATE/DELETE provenance but has no transaction-level
  model. GProM's reenactment (replaying a transaction under snapshot isolation via
  MV-semirings) needs no new syntax and could be exposed as a *function* over a
  transaction/audit identifier. Separate, larger, function-based.

---

## Cross-reference with existing plans

To avoid double-counting, candidates already tracked elsewhere:

- **Multivariate Normal / copulas / stochastic processes / do-calculus** →
  `continuous_distributions.md` (candidate 3 here is the systems-side view).
- **Conditioning as denial constraints / materialised posterior / Shapley over
  evidence** → `conditioning.md` (the materialising-update row).
- **Method-selection catalog, SUM-safe FPTRAS, the d-tree engine** →
  `probability-evaluation.md`. The d-tree engine documented there **is** the
  deterministic-anytime-bounds feature (not a gap); candidate 7 only refines it.
- **FBDD per-branch orders, inversion-free UCQ(OBDD) extensions, discrete RV
  HAVING** → `safe-query-followups.md`.
- **Data-side treewidth / MSO / tree-automaton, plus the finite-semantics-for-
  divergent-semirings recursion nuance** → `bounded-treewidth-data.md`.

---

## Prioritised roadmap

Ordering by *(value × architecture-fit) ÷ complexity*, with dependencies:

1. **P1 — WSMS responsibility + approximate Shapley FPRAS (1)**. Small, extends a
   feature ProvSQL already computes exactly, explicitly requested by a survey
   aimed at ProvSQL, and dependency-free.
2. **P2a — Why-not / missing-answer provenance (2)**, scoped to UCQ¬ over TID/BID,
   question-directed, with a sampled summary — the flagship new capability, and a
   research contribution.
3. **P2b — Attribute-level correlated uncertainty (3)**
   (`gate_mvnormal`/`gate_copula` + schema-level joint-column declaration),
   converging with the `continuous_distributions.md` multivariate roadmap. The
   **bounded-hypertree-width FPRAS (4)** — a *combined-complexity* complement to
   the shipped intensional approximations (Karp-Luby relative + d-tree additive),
   with unproven practical payoff — is a separate **research** track, not a build.
4. **P3 — pick by appetite:** open-world interval semantics (5, cheapest — linear
   on the safe path), provenance minimal factorisation (6,
   storage/readability), or the dissociation complementary bound (7, only if a
   workload shows the d-tree Shannon frontier is the bottleneck).
5. **P4 — demand-gated:** top-k ranking (8), deletion propagation / GDPR (9),
   incremental probability maintenance (10), transaction-level provenance /
   reenactment (11) — implement when a concrete workload asks.

The single highest-leverage first move is **P1 (WSMS + approximate Shapley)**: a
small, high-certainty extension of a feature ProvSQL already computes exactly,
explicitly asked for in the literature, with no dependency on the harder items.

---

## Sources

Primary sources fetched (subset cross-verified before a model-quota
interruption; the remainder are corroborating):

- MayBMS — U-relations, `assert`, compositional approximation, `esum`/`ecount`:
  https://maybms.sourceforge.net/download/maybms.pdf
- MCDB — VG-functions, tuple bundles:
  https://www.researchgate.net/publication/221213454
- SimSQL — database-valued Markov chains, in-SQL MCMC:
  https://dl.acm.org/doi/10.1145/2463676.2465283 ;
  https://link.springer.com/article/10.1007/s00778-013-0310-5
- Orion 2.0 — attribute-level uncertainty, `DEPENDENT`, partial pdfs, uncertain
  indexes: https://www.researchgate.net/publication/221214086
- Soliman & Ilyas — Top-k in uncertain databases (U-Topk / U-kRanks):
  https://www.semanticscholar.org/paper/cb932f00cb7884dbebf83e3ec607d8a47b7c5a7b
- Gatterbauer & Suciu — dissociation / oblivious bounds:
  https://arxiv.org/pdf/1310.6257
- Scaled dissociation / anytime deterministic bounds & FPRAS demand context:
  https://dl.acm.org/doi/full/10.1145/3643027
- Meel et al. — FPRAS for bounded-hypertree-width sj-free CQ (combined
  complexity): https://www.cs.toronto.edu/~meel/Papers/pods23.pdf
- Glavic — GProM (transaction provenance, why-not, PROV interop, optimiser):
  http://sites.computer.org/debull/A18mar/p51.pdf
- PUG — why/why-not for FO queries with negation:
  https://arxiv.org/pdf/1808.05752
- Why-not summarisation at scale (10⁶⁰–10⁸⁰ derivations):
  https://dl.acm.org/doi/10.1145/3299869.3319900 ; http://www.vldb.org/pvldb/vol13/p912-lee.pdf
- Buneman, Cheney & Tan — deletion/annotation propagation through views:
  https://www.researchgate.net/publication/45598929
- Makhija & Gatterbauer — minimal provenance factorisation (ILP / max-flow):
  https://arxiv.org/pdf/2103.07561
- Factorised computation / F-IVM / factorised ML: https://arxiv.org/pdf/1907.10125
- UA-DB / incomplete K-relations / certain-answer approximation:
  https://dl.acm.org/doi/abs/10.1145/3452021.3458326 ; https://arxiv.org/abs/1904.00234
- Ceylan, Darwiche & Van den Broeck — Open-world PDBs (KR'16):
  https://web.cs.ucla.edu/~guyvdb/papers/CeylanKR16.pdf
- Dynamic / ranked enumeration over probabilistic databases:
  https://arxiv.org/pdf/2202.10766 ; https://arxiv.org/pdf/2105.14307
- Bienvenu, Figueira & Lafourcade — WSMS tractable responsibility (PODS'25):
  https://arxiv.org/abs/2503.22358
- Shapley-in-databases survey (calls for ProvSQL integration):
  https://sigmodrecord.org/?smd_process_download=1&download_id=13453
- Semiring provenance for recursion / Datalog beyond Boolean, execution-based
  semantics: https://arxiv.org/pdf/2202.10766 (and the recursion-semantics
  strand within the fetched theory set)
