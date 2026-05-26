# Exploiting bounded-treewidth data

Feasibility study and design space for exploiting *bounded treewidth of the
input data* in ProvSQL, in the spirit of Courcelle's theorem and its
provenance refinement. Anchored on Amarilli, Bourhis & Senellart,
*Provenance Circuits for Trees and Treelike Instances* (ICALP 2015) and
*Combined Tractability of Query Evaluation via Tree Automata and Cycluits*
(ICDT 2017). Complements [`safe-query-followups.md`](safe-query-followups.md),
whose tractability classes are query-side; this one is data-side.

## The opportunity

Courcelle's theorem: a fixed MSO query is evaluable in time linear in the
data on instances of bounded treewidth, by compiling the query to a bottom-up
tree automaton and running it over a tree decomposition (tree encoding) of the
instance. The provenance refinement strengthens this to provenance: for a
fixed MSO (or CQ / UCQ) query `Q` and an instance `I` of treewidth `k`, one
can compute, in time `O(f(k,Q) * |I|)`, a provenance circuit whose treewidth
is bounded by `g(k,Q)` -- *independent of* `|I|`.

That conclusion lands squarely on machinery ProvSQL already has. A
bounded-treewidth Boolean provenance circuit is exactly the input
`TreeDecomposition` + `dDNNFTreeDecompositionBuilder` turn into a d-DNNF for
linear-time probability evaluation (`dDNNF::probabilityEvaluation`). So in
principle, bounded-treewidth data would unlock linear-time probabilistic
evaluation for a query class far beyond the safe / hierarchical fragment the
`provsql.boolean_provenance` rewriter accelerates today -- including queries
the safe-query detector rejects, and (via the cycluit extension) recursive
queries.

## It is not a free lunch

The guarantee holds only for a circuit **constructed the right way**. ABS
bound the treewidth of a provenance circuit *constructed along a tree
decomposition of the data*; nothing forces the lineage to acquire that bound
automatically. A purely extensional lineage -- the flat sum of one monomial
per answer, no sharing -- generally does *not* have bounded treewidth on
bounded-treewidth data; bounded *clause width* (each CQ monomial has `<= |Q|`
literals) does not imply bounded treewidth, the same way width-3 CNF can have
unbounded treewidth. So the question is whether ProvSQL's *specific*
construction lands a good circuit, and that is a research question rather than
a guarantee. The probe (§1) confirms the caveat bites even in the
**relational** fragment: a fixed, two-atom self-join already drives circuit
treewidth to `Theta(|I|)` on treewidth-1 data.

ProvSQL builds provenance along the **relational-algebra plan**. The planner
hook discovers per-relation tokens (`get_provenance_attributes`) and combines
them with `plus` / `times` / `monus` in `make_provenance_expression`
(`src/provsql.c`): a join becomes a variadic `provenance_times` over the joined
relations' tokens, a union or an aggregation becomes a `provenance_plus`. The
resulting gate DAG mirrors the join plan, and its primal graph -- gates as
nodes, wires as edges -- is what the knowledge compiler decomposes: `Graph`'s
only constructor is `Graph(const BooleanCircuit&)` (`src/Graph.h:48`), and
`TreeDecomposition(const BooleanCircuit&)` min-fills that graph. **At no point
does the data's adjacency structure inform the gate layout.** Nothing in the
design certifies, ahead of building the circuit, that its treewidth will be
bounded; ProvSQL discovers the treewidth only *after* construction, and when it
exceeds `MAX_TREEWIDTH` (10) the only recourse is the external-compiler (d4) or
Monte Carlo fallback.

The empirical probe (§1) shows the dividing line is not relational-vs-recursive
but **local-vs-non-local**. When the join is local and the data has bounded
degree (path / grid UCQs), ProvSQL's content-addressed, shared-circuit
construction stays good: treewidth is governed by `tw(I)` and the query and is
flat in `|I|` (measured to `|I| ~ 1300`). But two ingredients break it, both
without leaving the relational fragment: **high degree** (a self-join through a
star centre -- `tw(I) = 1`, centre degree `n` -- yields treewidth `n`) and
**cross-products** (`SELECT DISTINCT 1 FROM e a, e b` with no join condition
yields treewidth `n`). In both, the collapsed Boolean function is trivial (a
threshold or a conjunction of two disjunctions, so a treewidth-`<=2` circuit
exists), yet ProvSQL builds the full `Theta(n)`-treewidth sum-of-products --
the naive construction missing the ABS bound, exactly the "specific way"
problem. The **recursive fragment** then compounds this: reachability on a
bounded-treewidth grid grows treewidth with `|I|` and crosses the cap even
though `tw(I)` is a small constant.

This is a different axis from the deferred item in
[`safe-query-followups.md`](safe-query-followups.md) ("Other tractable CQ
subclasses"), which considers treewidth of the *query hypergraph* -- a
query-side parameter that "changes only when we discover bounded treewidth, not
the evaluation set". The present study is about *data* treewidth, which changes
*what circuit we construct*.

## Out of scope

- **Query-hypergraph (query-side) treewidth** -- covered, and deferred, in
  [`safe-query-followups.md`](safe-query-followups.md).
- **Circuit-side treewidth** -- already exploited; that is precisely what
  `TreeDecomposition` + `dDNNFTreeDecompositionBuilder` do today.
- **Re-proving the ABS / Courcelle guarantees** -- cited, not reproved.

## Plan

### 1. Empirical probe: pin the gap with numbers (done)

Method. Generate bounded-treewidth edge instances (path `tw=1`, cycle `tw=2`,
caterpillar `tw=1`, `3 x m` grid `tw=3`, plus `2/3/4 x m` grids for the
recursive case), parameterised by `|I|`. For each, run a query that collapses
the whole answer into one provenance token (`SELECT provsql.provenance() FROM
(SELECT DISTINCT 1 FROM ...) t`, i.e. a single `plus` over the per-answer
products). Read off the circuit treewidth two ways and cross-check them:
ProvSQL's own `tree_decomposition_dot` (the authoritative min-fill it would
compile, but it raises above `MAX_TREEWIDTH=10`), and an independent
min-degree elimination over the DAG exported via `get_children` (uncapped).
The two agreed on every case computable within the cap, validating the
measurement. The harness lives at `/tmp/ttw_probe/` (scratch, not committed);
it touches only existing SQL surface, no extension changes.

Result (a) -- **local** relational queries on bounded-degree data, treewidth
flat in `|I|`:

| instance (tw)     | query      | `|I|` edges    | circuit treewidth |
|-------------------|------------|----------------|-------------------|
| path (1)          | 2-path SJ  | 63 .. 1023     | 2 (flat)          |
| path (1)          | 3-chain    | 63 .. 1023     | 3 (flat)          |
| path (1)          | common-tgt | 7 .. 127       | 1 (flat)          |
| cycle (2)         | 2-path SJ  | 8 .. 128       | 3 (flat)          |
| cycle (2)         | 3-chain    | 8 .. 128       | 5 (flat)          |
| caterpillar (1)   | 2-path SJ  | 14 .. 254      | 2 (flat)          |
| `3 x m` grid (3)  | 2-path SJ  | 77 .. 1277     | 6 (flat)          |
| `3 x m` grid (3)  | common-tgt | 37 .. 887      | 3 (flat)          |
| `3 x m` grid (3)  | 3-chain    | 37 .. 1062     | ~11 (flat, **> cap**) |

Circuit *size* grows linearly with `|I|`, but treewidth is set by `tw(I)` and
the query, not by `|I|`. So local UCQs on bounded-degree data are well served.

Result (a') -- **pathological** fixed relational queries: treewidth `= Theta(|I|)`
even on treewidth-1 data. `selfcart` is the cross self-product
`SELECT DISTINCT 1 FROM e a, e b` (no join); `costar` is the self-join
`E(x,d), E(y,d)` over an in-star of `n` edges (`tw(I) = 1`, centre degree `n`);
`cart` is the cross-product of two unary tracked relations of size `n`:

| query     | data (tw)        | `n`        | circuit treewidth |
|-----------|------------------|------------|-------------------|
| selfcart  | path (1)         | 4 .. 32    | `n` (= 4 .. 32)   |
| costar    | in-star (1)      | 4 .. 32    | `n` (= 4 .. 32)   |
| cart      | two sets (~0)    | 4 .. 32    | 1 (flat)          |

The collapsed Boolean functions are trivial -- `selfcart` is
`(OR e_i)` (absorption), `costar` is "at least two `e_i` true" (a symmetric
threshold), both with treewidth-`<=2` circuits -- yet ProvSQL materialises the
full `Theta(n)`-treewidth sum-of-products. `cart` is the instructive contrast:
there ProvSQL's aggregation path *does* factor the independent product into
`(OR r_x) AND (OR s_y)` (treewidth 1, `~2n` gates), so the construction is
sometimes optimal and sometimes not -- it is not a property of the fragment but
of whether the construction happens to factor. This is the relational "specific
way" gap made concrete.

Checked under `provsql.boolean_provenance = on` (with `simplify_on_load = on`).
As originally measured the explosion persisted for both, but for two distinct
reasons -- and one of them has since been fixed:

- `selfcart` -- a **phase-ordering miss**, now **fixed**. `foldBooleanIdentities`
  already implements Boolean absorption `a OR (a AND b) -> a` (Rule B3,
  `src/GenericCircuit.cpp`), keyed on the dominating literal `a` being a
  *direct sibling* under the `plus`. For `selfcart`, `e_i` becomes a direct
  sibling only through the diagonal `times(e_i, e_i)`, which B1 dedups to a
  single-wire `times`; the `-> e_i` collapse is `foldSemiringIdentities`, which
  the old code ran *once after* the B3 loop, so absorption never saw `e_i` and
  the off-diagonal `times` survived. `foldBooleanIdentities` now runs the
  Boolean sweep and `foldSemiringIdentities` to a **joint fixpoint**
  (alternating until neither changes anything), so the collapse re-exposes the
  literal and B3 absorbs every off-diagonal: under `boolean_provenance` on,
  `selfcart` now folds to `OR e_i` (**treewidth 1**) and compiles via the
  tree-decomposition path at all `n` (regression test: `boolean_fold` case 7).
- `costar` -- still uncollapsed, and correctly so: a symmetric threshold-2
  (`OR_{i<j} e_i AND e_j`, no diagonal, no literal dominates any product), so B3
  is inapplicable *in principle*; no ordering fix helps -- it needs structural
  (threshold / separator) recognition, the harder lever in route 3.

So the fold now realises the ABS bound for the absorption-reachable shapes
(`selfcart`); the threshold shapes (`costar`) remain the open half.

Result (b) -- **recursive** reachability, treewidth grows with the instance
and crosses the cap (`None` = ProvSQL raised "treewidth greater than 10"):

| grid         | `|I|` edges | reachability circuit treewidth |
|--------------|-------------|--------------------------------|
| `2 x 3`      | 7           | 4                              |
| `2 x 4`      | 10          | 6                              |
| `2 x {6..}`  | >= 16       | None (> 10)                    |
| `3 x 3`      | 12          | 7                              |
| `3 x 4`      | 17          | 10                             |
| `3 x {6..}`  | >= 27       | None (> 10)                    |
| `4 x 3`      | 17          | 10                             |
| `4 x {4..}`  | >= 24       | None (> 10)                    |

(On 1D shapes -- path, cycle, caterpillar -- recursive reachability stays at
treewidth 0-3, because the reachability lineage is itself essentially a chain.)

Result (c) -- **in-process tree-decomposition vs d4 as a function of
treewidth.** Pathological `costar` circuits of controlled treewidth `t`
(`~600`-`900` gates), timing only `tree-decomposition` and `compilation,d4`
via `_probability_benchmark_one` (both probabilities agree to `1e-6`):

| treewidth | gates | tree-decomposition (ms) | d4 (ms) |
|-----------|-------|-------------------------|---------|
| 4         | 361   | 21                      | 82      |
| 6         | 477   | 93                      | 432     |
| 8         | 586   | 669                     | 967     |
| 9         | 661   | 2404                    | 903     |
| 10        | 661   | exceeds cap (errors)    | 1490    |
| 12        | 820   | -                       | 4652    |
| 14        | 961   | -                       | 11855   |

Both methods are exponential in treewidth (`#P`-hard counting), but the
in-process builder's `2^treewidth * |circuit|` base overtakes d4's search
around **treewidth 8-9**: it is `4-5x` faster up to `tw 6`, ties at `tw 8`,
and is `~2.7x` slower at `tw 9`. The cap (`10`) therefore sits just *above*
the performance crossover, not below it.

Reading. The caveat is real across both fragments, but the actionable shape is
not "build a new relational construction":

- **Relational.** ProvSQL's construction is good for local UCQs on
  bounded-degree data and bad for high-degree self-joins and cross-products,
  where it inflates treewidth to `Theta(|I|)` despite a trivial optimal circuit.
  But (a') `cart` shows the existing aggregation path already factors *some*
  independent products optimally, so the lever is **better factoring in the
  existing construction** (extend the `cart`-style factorisation to more
  independent / low-degree-cut substructures), not a tree-automaton rebuild.
  Whether the pathological shapes occur in real workloads is the open question;
  the benign-UCQ result says many do not.
- **Recursive.** Reachability on a `2/3/4 x m` grid grows treewidth with `|I|`
  at constant `tw(I)` and crosses the cap -- the one place the gap is both
  intrinsic and `|I|`-driven, and the strongest case for a
  decomposition-aligned (cycluit) construction (Route C).
- **The cap.** Result (c) says `MAX_TREEWIDTH = 10` is roughly well-placed
  (the crossover is `~8-9`); *raising* it would route more circuits to the
  slower method. The refinement is a treewidth-conditioned dispatch, not a
  bigger cap.

### 2. The `MAX_TREEWIDTH` cap: dispatch, do not raise

Result (c) overturns the intuition that the cap throws away tractability.
At the treewidths where the in-process builder hits the cap (`>= 10`), d4 is
already the faster compiler, so the existing fallback is doing the right thing;
a fixed 3-chain join on a `3 x m` grid (treewidth `~11`) is correctly handled
by d4 today. The improvement is therefore not a higher cap but a
**treewidth-conditioned choice**: compute the (cheap, min-fill) treewidth
estimate first, use the in-process `dDNNFTreeDecompositionBuilder` when it is
`<~ 8` and d4 above, instead of trying the builder up to `10` and only then
falling back. Small, self-contained, and it shaves the `tw 9-10` cases where
the builder currently wins the right to run but loses the race. Independent of
Routes A/C; worth confirming the crossover on non-pathological circuit shapes
and a range of sizes before pinning the threshold.

### 3. Lighter relational lever: extend independent-product factoring

The §1 (a') explosions are an artefact of emitting the full sum-of-products
when a much smaller equivalent circuit exists, and the `cart` contrast shows
ProvSQL's aggregation path already factors *independent* products optimally.
The lever is to extend that factoring to the connected-but-low-cut cases the
naive construction misses: a construction- or load-time pass that detects when
the products feeding a `gate_plus` decompose (independent components; a small
leaf-separator; absorption / threshold structure) and re-brackets them before
they reach the knowledge compiler. This is a Boolean-only optimisation in the
spirit of [`safe-query-followups.md`](safe-query-followups.md); note that the
independent-subtree-factoring MVP-1 there was abandoned *on disconnected
components* (the d-DNNF builder already factors those), with the explicit note
that the regime worth targeting is "large circuits where the elimination-order
heuristic gets confused by top-level cross-talk" -- which is exactly `selfcart`
/ `costar` (joint treewidth `Theta(n)`, factored form `O(1)`). The §1
fold-check split this into two sub-levers, one now **done**:

- `selfcart` (absorption-reachable) -- **landed.** It was a phase-ordering miss
  in B3 (`foldBooleanIdentities` already encodes `a OR (a AND b) -> a`, but the
  single-wire `times(e_i, e_i) -> e_i` collapse that exposes the dominating
  literal ran only after the B3 loop). Fixed by running the Boolean sweep and
  `foldSemiringIdentities` to a **joint fixpoint** in `foldBooleanIdentities`
  (`src/GenericCircuit.cpp`); `selfcart` now folds to treewidth 1 under
  `boolean_provenance` (regression: `boolean_fold` case 7).
- `costar` (threshold) -- **open.** A symmetric threshold-2 with no literal to
  absorb; needs structural detection (a threshold / small-separator recogniser),
  the harder half. Medium cost, inside existing circuit code, no automaton.
  Crux/risk: recognising the factorisation cheaply without re-deriving the full
  structure.

### 4. Route A -- full pipeline: data decomposition + tree automaton

The faithful ABS construction. Build a tree decomposition of the instance's
Gaifman graph (elements as nodes, the elements co-occurring in a fact as a
clique); compile `Q` to a bottom-up tree automaton; run the automaton over the
tree encoding while emitting provenance gates per transition; hand the
resulting bounded-treewidth circuit to the existing d-DNNF builder.

- *Cost: very high.* FO / MSO to tree-automaton compilation is non-elementary
  in `|Q|` in the worst case; even restricted to CQ / UCQ, the encoding
  (a finite alphabet over bag contents, equality types and fact incidence) and
  the automaton construction are research-grade work to land inside a Postgres
  extension.
- *Payoff:* the only route that delivers the provable linear-time guarantee on
  the queries where the lineage blows up.
- *Status after §1:* the probe *did* find fixed relational queries whose
  circuit treewidth grows with `|I|` (`selfcart`, `costar`), but they are
  addressable by cheaper means -- better factoring inside the existing
  construction (the `cart` contrast shows the machinery already factors some
  independent products) -- so the full automaton pipeline stays deferred until
  a workload defeats the lighter factoring, against the same
  d4-on-natural-lineage cost-benefit that deferred Monet's construction in
  [`safe-query-followups.md`](safe-query-followups.md).

### 5. Route B -- post-hoc refactor of the existing circuit

Keep the planner-hook construction, but given a data tree decomposition,
re-bracket the materialised lineage to follow the bag tree before knowledge
compilation.

- *Cost: medium-high, but murky.* Recovering the correct factorisation from a
  flattened circuit needs the leaf-to-data-element map and effectively
  re-derives the automaton structure -- so it risks reproducing Route A under a
  harder guise.
- *Assessment: likely a trap.* Documented here so it is not re-litigated.

### 6. Route C -- narrow, high-value special case (recommended seed)

Implement the construction for **one** scenario where the automaton is trivial
and the payoff is real: reachability / linear-recursive queries over a
bounded-treewidth graph relation -- the cycluit case of ABS 2017. The
"automaton" for reachability is a handful of states.

The win is a capability ProvSQL genuinely lacks. The current `eval_recursive`
fixpoint prototype (`lower_recursive_cte` in `src/provsql.c`, SQL
`eval_recursive`) unrolls a `WITH RECURSIVE` to a value-fixpoint: it is
Boolean / acyclic-only, UNION-only, and emits a strictly-DAG circuit
(`Circuit.h:18`) whose size is unbounded on cyclic data (it leans on
absorptive Boolean semantics to converge). A bounded-treewidth construction
over the graph's tree decomposition would instead give recursion on cyclic data
a principled, tractable, linear-size provenance circuit, and extends to general
semirings. It is the relational study's natural sequel and dovetails with the
in-flight recursive-query work rather than standing alone.

### 7. Semiring threads (investigated separately)

- **Boolean / probability** -- primary thread. The bounded-treewidth circuit
  feeds `TreeDecomposition` to `dDNNFTreeDecompositionBuilder` to
  `dDNNF::probabilityEvaluation`, all already linear-time on bounded treewidth.
  Smallest scope; matches the `provsql.boolean_provenance` regime.
- **General m-semirings** -- the ABS construction is semiring-agnostic, but
  ProvSQL's any-semiring evaluation runs over the `GenericCircuit`
  (`provenance_evaluate_compiled`), which does not exploit treewidth at all
  (treewidth is a knowledge-compilation / probability concern today). A
  bounded-treewidth `GenericCircuit` would still need a *width-aware semiring
  evaluator* (a bag-by-bag dynamic program analogous to the d-DNNF builder, but
  over the semiring carrier) to cash in the bound. A separate, larger axis; not
  assumed by the Boolean thread.

## Reuse map

Reusable as-is, or with a thin adapter:

- Min-fill elimination, `PermutationStrategy`, and the `Graph` mutating ops
  (`remove_node`, `fill`) -- but `Graph`'s only constructor takes a
  `BooleanCircuit` (`src/Graph.h:48`); a new builder taking a relational
  instance's Gaifman graph is needed.
- `TreeDecomposition`'s bag / tree representation, `makeFriendly`, and the
  PACE-format reader (`src/TreeDecomposition.h`) -- the bag-tree structure is
  generic; today it is only ever built from a circuit.
- `dDNNFTreeDecompositionBuilder` and the whole probability path -- consume a
  bounded-treewidth `BooleanCircuit` unchanged.

Genuinely new (the hard core):

- A tree *encoding* of a relational instance over a finite alphabet.
- Query-to-tree-automaton compilation, even for a restricted fragment.
- The provenance-emitting automaton run (a gate per transition), including, for
  Route C, the cyclic-provenance (cycluit) semantics that the strictly-DAG
  circuit framework does not currently support.

## Priorities

1. **§1 probe -- done.** Local UCQs on bounded-degree data: treewidth flat in
   `|I|`. High-degree self-joins and cross-products: treewidth `Theta(|I|)`.
   Recursive reachability on grids: grows and crosses the cap. d4 overtakes the
   in-process builder at treewidth `~8-9`.
2. **Cap dispatch (route 2).** Smallest, self-contained: choose the in-process
   builder vs d4 by the min-fill treewidth estimate (`<~8` -> builder), rather
   than try-then-fall-back at `10`. Do first; confirm the threshold on
   non-pathological shapes.
3. **Route C** (recursive / reachability on bounded-treewidth graphs). The only
   place the probe shows treewidth genuinely *growing* with `|I|` at constant
   `tw(I)`; the automaton is trivial; it composes with the in-flight
   recursive-query work. The substantive build.
4. **Factoring lever (route 3).** The `selfcart` (absorption) half is **done**
   (joint-fixpoint fold; `boolean_fold` case 7). The `costar` (threshold) half
   remains, via structural detection; scope only if a real workload exhibits it.
5. **Route A** (full automaton) -- deferred until the factoring lever proves
   insufficient. **Route B** -- recorded as a likely trap, not a priority.

## Verdict

The Courcelle / ABS guarantee is real and reuses ProvSQL's existing probability
back end, and the probe sharpens where it pays off. The caveat is genuine and
bites in both fragments -- a fixed two-atom self-join already inflates treewidth
to `Theta(|I|)` on treewidth-1 data -- so bounded data treewidth is *not* a free
lunch. But the actionable conclusion is not "build a tree-automaton relational
construction":

- For the **relational** fragment, the explosions are confined to high-degree
  self-joins and cross-products; local UCQs on bounded-degree data already
  compile with treewidth bounded by `f(tw(I), Q)`. The proportionate response is
  to **extend the existing fold/factoring** rather than the full ABS pipeline
  (Route A, deferred): the absorption-reachable case (`selfcart`) is already
  fixed by the joint-fixpoint fold (route 3, landed), leaving only the threshold
  case (`costar`). Route B remains a trap.
- For the **recursive** fragment, the fixpoint construction does *not* inherit
  the bound: reachability on a bounded-treewidth grid yields a circuit whose
  treewidth grows with the instance and crosses the cap. This is the genuine,
  `|I|`-driven Courcelle gap, exactly the cycluit case, and the strongest case
  for Route C -- a decomposition-aligned construction that would keep recursion
  on cyclic, bounded-treewidth data tractable, which ProvSQL otherwise cannot do.
- On the **cap**, the d4-crossover (`~8-9`) shows `MAX_TREEWIDTH = 10` is about
  right; the win is a treewidth-conditioned dispatch (route 2), not a higher cap.

In short: the cheap, immediate fix is the cap dispatch; the substantive build is
Route C for the recursive fragment; the relational pathologies are real but
addressed more cheaply by better factoring than by a new construction.
