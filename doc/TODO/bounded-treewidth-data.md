# Exploiting bounded-treewidth data

Design space for exploiting *bounded treewidth of the input data* in ProvSQL,
in the spirit of Courcelle's theorem and its provenance refinement. Anchored on
Amarilli, Bourhis & Senellart, *Provenance Circuits for Trees and Treelike
Instances* (ICALP 2015) and *Combined Tractability of Query Evaluation via Tree
Automata and Cycluits* (ICDT 2017). Complements
[`safe-query-followups.md`](safe-query-followups.md), whose tractability classes
are query-side; this one is data-side.

This file is a **to-do**: it records the design rationale and the work that
remains. Shipped work is not duplicated here -- see the git history, the
CHANGELOG, and the user manual (`probabilities.rst`, `semirings.rst`,
`temporal.rst`) for what already exists.

## The gap (why)

Courcelle's theorem: a fixed MSO query is evaluable in time linear in the data
on instances of bounded treewidth, by compiling the query to a bottom-up tree
automaton run over a tree decomposition of the instance. The provenance
refinement strengthens this to provenance: for fixed `Q` and an instance `I` of
treewidth `k`, one can compute, in time `O(f(k,Q)·|I|)`, a provenance circuit
whose treewidth is `g(k,Q)` -- *independent of* `|I|`. That bounded-treewidth
circuit is exactly what `TreeDecomposition` + `dDNNFTreeDecompositionBuilder`
turn into a d-DNNF for linear-time probability evaluation.

But the guarantee holds only for a circuit **constructed the right way**. ABS
bound the treewidth of a provenance circuit *constructed along a tree
decomposition of the data*; ProvSQL instead builds provenance along the
**relational-algebra plan** (`make_provenance_expression`, `src/provsql.c`), so
the gate DAG mirrors the join plan and nothing certifies its treewidth ahead of
building it. Empirically the line is **local vs non-local**: local joins on
bounded-degree data stay flat in `|I|`; high degree (a self-join through a star
centre, `tw(I)=1`, gives circuit treewidth `n`) and cross-products blow up; and
the **recursive fragment** is the one place the gap is intrinsic and `|I|`-driven
(reachability on a bounded-treewidth grid grows circuit treewidth with `|I|`
even at constant `tw(I)`).

This is a different axis from the deferred item in
[`safe-query-followups.md`](safe-query-followups.md) ("other tractable CQ
subclasses"), which is treewidth of the *query hypergraph*. Here it is *data*
treewidth, which changes *what circuit we construct*.

## Out of scope

- **Query-hypergraph (query-side) treewidth** -- in
  [`safe-query-followups.md`](safe-query-followups.md).
- **Circuit-side treewidth** -- already exploited (`TreeDecomposition` +
  `dDNNFTreeDecompositionBuilder`).
- **Re-proving the ABS / Courcelle guarantees** -- cited, not reproved.

## Open work

### Verified mulinput-OR certificate (correctness hardening)

The BID route marks a block's `plus(mulins)` *deterministic* and the
certified-island evaluator trusts the mark (sums the alternatives, registers the
block key once, reads the none branch as `monus(one, plus(mulins))` = `1 - Σp`).
That trust is unproven. (The read-once `independent` evaluator, by contrast,
*verifies* a block directly -- its OR case groups `MULIN` children by block key
and sums within a block -- so the obligation is specifically about
`evaluateCertifiedIsland`, which plain-sums marked ORs *without* that check, and
about the constructor's internal *state* ORs whose determinism is global, not
locally checkable.) The to-do is a Lean statement backing it (a spec the C++
must meet, not a verification of the C++ itself), in the **probability / WMC**
semantics only -- the absorptive-semiring BID path is a separate question.

Modeling choice (the crux): model a block as one *categorical* random variable
`B : Fin (k+1)` with `μ(B=i)=p_i` (i<k), `μ(B=k)=1-Σp`, and the mulinput as the
event `m_i := {B = i}` -- not as independent Booleans with an at-most-one
constraint. Then the three block lemmas are near-immediate:

- `mulin_disjoint : i ≠ j → Disjoint (m_i) (m_j)`   (licenses the deterministic-OR mark)
- `mulin_or_prob  : Pr[⋃ᵢ m_i] = Σᵢ pᵢ`             (the sum is exact)
- `mulin_none     : Pr[(⋃ᵢ m_i)ᶜ] = 1 - Σᵢ pᵢ`      (the `monus(one,·)` none branch)

The content is the soundness theorem they feed: lifting d-DNNF WMC correctness
from *free Boolean variables* to *categorical block variables*. The structural
evaluator (`detOR` → sum, `decAND` → product, `NOT` → `1-`, leaf → weight) equals
`Pr[⟦C⟧]` when every marked-OR has pairwise-disjoint children as events, every
marked-AND has children with disjoint **support counted by block** (this is what
"register the key once" implements -- all `m_{b,*}` share block `b`), and every
NOT is over a sub-circuit *complete over its support* (the block's outcomes
partition into the OR and the none-complement, which is why `1-Σ` is legitimate
where general d-DNNF forbids negation). Minimal first target: the three lemmas
plus single-block soundness; the multi-block theorem with the support /
island discipline is most of a general d-DNNF-correctness development and should
be scoped deliberately.

### Route 3 -- `costar` threshold half (structural factoring)

The remaining non-recursive blow-up: the in-star self-join `SELECT DISTINCT 1
FROM e a, e b WHERE a.x <> b.x` over `n` edges of a star (data treewidth 1)
yields the symmetric threshold-2 function `⋁_{i<j} eᵢ∧eⱼ`, emitted as a
`Θ(n)`-treewidth sum-of-products. Absorption cannot touch it (no literal
dominates any product), so it needs a *structural* rewrite: recognise the
threshold / small-separator shape feeding a `gate_plus` and re-bracket it into
the linear-size sequential counter (`seen1ᵢ = seen1ᵢ₋₁ ∨ eᵢ`;
`seen2ᵢ = seen2ᵢ₋₁ ∨ (seen1ᵢ₋₁ ∧ eᵢ)`), which is a certified d-DNNF by
construction (counter states partition worlds). The independent-product factoring
and the Boolean fold already cover the disjoint and absorption-reachable shapes;
this is the residue.

Notes: (1) the equivalent `HAVING count(*) >= 2` is *already* evaluated exactly
by the `CountCmpEvaluator` DP, so the pathology only bites when a workload
insists on the self-join form -- route 3 is purely a *recognition* problem.
(2) When recognised, emit the counter with the d-DNNF certificate so the result
is a first-class token. (3) The `times-canonical` namespace (added for the
k-terminal conjunctions) could serve the factored form. Crux/risk: recognising
the factorisation cheaply without re-deriving the structure. Medium cost, inside
existing circuit code, no automaton; scope only if a real workload exhibits it.

### Route C leftovers

Small extensions to the shipped reachability route, each deferred until a real
workload needs it:

- **Shared-support join-defined edges.** Today join-defined edges are accepted
  only when their token supports are pairwise disjoint (the dynamic
  conjunctive-leaf walk). The faithful variables-in-the-decomposition DP with
  late-branching states (tractability depending on how widely tuples are shared)
  is the open generalisation, plus static certification of disjointness from
  keys / FDs to skip the dynamic walk.
- **Non-recursive triggers.** Rewriting a non-recursive self-join *chain*
  (`e a, e b, … WHERE a.dst = b.src …` with DISTINCT) into the depth-bounded
  recursive CTE the bounded-hop route already compiles; and the in-star
  self-join into the counting form (overlaps Route 3). Only worthwhile if such
  shapes show up in real workloads.
- **Any-reach shared sweep, next leap.** The shared multi-group sweep still pays
  one per-group DP pass (the dirty region is the seed-to-root bag paths). If
  group counts grow, per-group *virtual collector chains* (member → chain arcs,
  "some member reachable" = "chain end reachable") would turn any-reach into
  per-vertex reads of one two-sweep `compileAll`, at width `+O(batch)` for
  batched groups with chain order aligned to the elimination order -- needs the
  width impact validated empirically.
- **K-terminal side filters.** The k-terminal (self-join conjunction) detector
  could gain the member-filter treatment the any-reach detector has, for
  conjunctions with deterministic side predicates. No workload wants it yet.

### General (non-absorptive) m-semirings -- width-aware evaluator

Absorptive-semiring evaluation of the route's certified circuits is done (it
runs over the plain `GenericCircuit`, linear in the circuit). General
m-semirings are a larger axis: ProvSQL's `provenance_evaluate_compiled` does not
exploit treewidth, so a bounded-treewidth `GenericCircuit` would need a
*width-aware semiring evaluator* (a bag-by-bag DP analogous to the d-DNNF
builder, over the semiring carrier), and a finiteness story for recursion
(Mumick-Shmueli) first. Cf. Ramusat, Maniu & Senellart (EDBT 2021): the
absorptive case is the compile-once / evaluate-per-0-closed-semiring counterpart
of their NodeElimination on the treewidth axis.

### Route A -- full MSO / CQ automaton (deferred research bet)

The faithful ABS construction for arbitrary (non-recursive) queries: tree
decomposition of the instance's Gaifman graph, `Q` compiled to a bottom-up tree
automaton, the automaton run over the tree encoding emitting one provenance gate
per transition, the bounded-treewidth circuit handed to the existing d-DNNF
builder. The automaton is mechanical -- gate `g[ν,q]` per node and state, leaves
`x_t` / `NOT x_t`, internal `g[ν,q] = ⋁_{δ(q1,q2,a)=q} g[l,q1] ∧ g[r,q2] ∧ χ_a`,
output `⋁_{q∈F} g[root,q]`; size `O(|Q|²·|E|)`, treewidth `O(|Q|)` independent of
`|I|` (the state count *is* the treewidth bound). The two moves a relational-plan
builder lacks are **restrict the state to the active bag** (forget = drop) and
**cap the summary** -- e.g. the in-star "at least two" becomes a counter capped
at 2.

Integration cost has dropped now that Route C exists: the automaton run only has
to *mark its gates* (deterministic state ORs, decomposable transition ANDs) and
materialise through the same content-addressed channel; the whole evaluation and
artefact surface (chooser, `independent`, `interpret-as-dd`, Shapley) then picks
the result up. What is still missing is exactly the **query-to-automaton
compiler** -- non-elementary in `|Q|` in general, research-grade even restricted
to CQ / UCQ. Deferred until a workload defeats the lighter levers, against the
same cost-benefit that defers Monet's construction in
[`safe-query-followups.md`](safe-query-followups.md).

*Prior art.* Mikaël Monet's MPRI M2 internship (2015, supervised by Senellart),
*Probabilistic Evaluation of MSO Queries on Bounded Treewidth Instances*,
implements this pipeline in Java end-to-end (`libtw` Gaifman-graph decomposition,
the `lethal` tree-automata library, an on-the-fly automaton run emitting a
Boolean provenance circuit, circuit-shrinking, linear-time probability eval,
MayBMS benchmark) -- a reusable design reference (especially the on-the-fly run
and the propagator-state trick that keeps the circuit linear), though its
MSO-to-automaton step was done by hand (no implemented compiler) and "treewidth
quickly becomes a limiting factor".

**Reuse map** (for Route A). As-is or with a thin adapter: min-fill elimination
and the `Graph` mutating ops -- but `Graph`'s only constructor takes a
`BooleanCircuit` (`src/Graph.h:48`), so a builder over a relational instance's
Gaifman graph is needed (Route C added one for node/edge sets); the
`TreeDecomposition` bag-tree representation and PACE reader;
`dDNNFTreeDecompositionBuilder` and the probability path (consume a
bounded-treewidth `BooleanCircuit` unchanged). Genuinely new: a finite-alphabet
tree *encoding* of a relational instance, the query-to-automaton compilation, and
the provenance-emitting run. Monet's prototype covers the first three for the
acyclic relational case.

## Priorities

1. **Verified mulinput-OR certificate** -- bounded correctness-hardening; the
   only open item with a clear, self-contained deliverable.
2. **Route 3 / non-recursive triggers** -- structural detection; scope only if a
   real workload exhibits the in-star or chain shapes.
3. **General-semiring width-aware evaluator** and **Route A** -- the research
   bets, deferred until the lighter levers prove insufficient.
