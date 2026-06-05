# Exploiting bounded-treewidth data

Design space for exploiting *bounded treewidth of the input data* in ProvSQL,
in the spirit of Courcelle's theorem and its provenance refinement. Anchored on
Amarilli, Bourhis & Senellart, *Provenance Circuits for Trees and Treelike
Instances* (ICALP 2015) and *Combined Tractability of Query Evaluation via Tree
Automata and Cycluits* (ICDT 2017). Complements
[`safe-query-followups.md`](safe-query-followups.md), whose tractability classes
are query-side; this one is data-side.

## The opportunity

Courcelle's theorem: a fixed MSO query is evaluable in time linear in the data
on instances of bounded treewidth, by compiling the query to a bottom-up tree
automaton and running it over a tree decomposition of the instance. The
provenance refinement strengthens this to provenance: for a fixed query `Q` and
an instance `I` of treewidth `k`, one can compute, in time `O(f(k,Q) * |I|)`, a
provenance circuit whose treewidth is bounded by `g(k,Q)` -- *independent of*
`|I|`. That bounded-treewidth circuit is exactly what
`TreeDecomposition` + `dDNNFTreeDecompositionBuilder` turn into a d-DNNF for
linear-time probability evaluation (`dDNNF::probabilityEvaluation`).

## It is not a free lunch

The guarantee holds only for a circuit **constructed the right way**. ABS bound
the treewidth of a provenance circuit *constructed along a tree decomposition of
the data*; nothing forces the lineage to acquire that bound automatically.
ProvSQL builds provenance along the **relational-algebra plan** instead: the
planner hook combines per-relation tokens with `plus` / `times` / `monus` in
`make_provenance_expression` (`src/provsql.c`), and the resulting gate DAG
mirrors the join plan. The knowledge compiler then decomposes its primal graph
(`Graph(const BooleanCircuit&)`, `src/Graph.h:48`; `TreeDecomposition(const
BooleanCircuit&)`). **At no point does the data's adjacency structure inform the
gate layout**, so nothing certifies the circuit's treewidth ahead of building
it.

Empirically the dividing line is **local-vs-non-local**. Local joins on
bounded-degree data (path / grid UCQs) stay good: treewidth is governed by
`tw(I)` and the query, flat in `|I|`. Two ingredients break it without leaving
the relational fragment -- **high degree** (a self-join through a star centre,
`tw(I)=1`, yields treewidth `n`) and **cross-products** (`SELECT DISTINCT 1 FROM
e a, e b` yields treewidth `n`) -- where ProvSQL materialises the full
`Theta(n)`-treewidth sum-of-products despite a trivial optimal circuit. The
absorption-reachable case (`SELECT DISTINCT 1 FROM e a, e b` with a join) folds
to treewidth 1 today via the joint-fixpoint Boolean fold in
`foldBooleanIdentities`; the threshold case (the in-star self-join, "at least
two edges true") does not (see Route 3). The **recursive fragment** compounds
this: reachability on a bounded-treewidth grid grows circuit treewidth with
`|I|` and crosses `MAX_TREEWIDTH` even though `tw(I)` is a small constant -- the
one place the gap is intrinsic and `|I|`-driven.

This is a different axis from the deferred item in
[`safe-query-followups.md`](safe-query-followups.md) ("Other tractable CQ
subclasses"), which considers treewidth of the *query hypergraph* (a query-side
parameter). The present study is about *data* treewidth, which changes *what
circuit we construct*.

## Out of scope

- **Query-hypergraph (query-side) treewidth** -- covered, and deferred, in
  [`safe-query-followups.md`](safe-query-followups.md).
- **Circuit-side treewidth** -- already exploited; that is precisely what
  `TreeDecomposition` + `dDNNFTreeDecompositionBuilder` do today.
- **Re-proving the ABS / Courcelle guarantees** -- cited, not reproved.

## Open work

### Route 3 -- extend independent-product factoring (`costar` threshold half)

The relational explosions are an artefact of emitting the full sum-of-products
when a much smaller equivalent circuit exists. ProvSQL's aggregation path
already factors *independent* products optimally (`(OR r_x) AND (OR s_y)` for a
disjoint cross-product, treewidth 1), and the absorption-reachable self-join now
folds to treewidth 1 via the joint-fixpoint Boolean fold. What remains is the
**threshold** case: the in-star self-join collapses to a symmetric threshold-2
(`OR_{i<j} e_i AND e_j`, "at least two of `n` edges true"), where no literal
dominates any product, so the absorption rule B3 is inapplicable *in principle*
and no ordering fix helps. It needs structural (threshold / small-separator)
recognition -- a construction- or load-time pass that detects when the products
feeding a `gate_plus` decompose and re-brackets them before the knowledge
compiler. Medium cost, inside existing circuit code, no automaton. Crux/risk:
recognising the factorisation cheaply without re-deriving the full structure.
Scope only if a real workload exhibits it.

### Route C -- recursive / reachability on bounded-treewidth graphs (recommended seed)

Implement the decomposition-aligned (cycluit) construction for **one** scenario
where the automaton is trivial and the payoff is real: reachability /
linear-recursive queries over a bounded-treewidth graph relation -- the cycluit
case of ABS 2017. This is the place the gap is both intrinsic and `|I|`-driven,
and a capability ProvSQL genuinely lacks. The current `eval_recursive` fixpoint
(`lower_recursive_cte` in `src/provsql.c`) unrolls a `WITH RECURSIVE` to a
value-fixpoint: it is Boolean / acyclic-only, UNION-only, and emits a
strictly-DAG circuit (`Circuit.h:18`) whose size is unbounded on cyclic data. A
bounded-treewidth construction over the graph's tree decomposition would give
recursion on cyclic data a principled, tractable, linear-size provenance circuit
and extend to general semirings.

#### Candidate query family: two-terminal reachability / network reliability

The concrete anchor is **`s`-`t` reachability (network reliability) on a
bounded-treewidth probabilistic graph**:

```sql
WITH RECURSIVE reach(v) AS (
    SELECT $s                                    -- source
  UNION
    SELECT e.dst FROM reach r, edge e WHERE e.src = r.v
)
SELECT count(*) > 0 FROM reach WHERE v = $t;     -- is t reachable from s?
```

over a probabilistic `edge(src, dst)` whose graph has bounded treewidth:
series-parallel networks (`tw=2`), outerplanar graphs, transit / utility
networks, workflow / process graphs. Why this family:

- *Genuine capability gap, not a speedup.* `Pr[t reachable from s]` is
  two-terminal network reliability -- `#P`-hard in general (Valiant), linear-time
  on bounded treewidth. Competing PDBs (MayBMS) cannot express it once
  connectivity is needed (MSO, not FO).
- *ProvSQL cannot do it tractably today.* `eval_recursive` is acyclic / Boolean
  only and emits a strictly-DAG circuit of unbounded size on cyclic data;
  reachability circuit treewidth *grows with `|I|`* and crosses the cap even at
  constant `tw(I)`.
- *The cheap levers do not apply.* It is recursive, so the Boolean fold and the
  route-3 threshold / factoring recogniser cannot reach it. Only a
  decomposition-aligned (cycluit) construction helps.

And the automaton is small. For directed `s -> t` reachability the state at a bag
is the subset of currently-active vertices already known reachable from `s`, plus
a "reached `t`" flag:

```
introduce s                : R = {s}
edge (u,v) present, u in R : R := R ∪ {v}        ( gated by x_e )
forget w                   : if w = t and w in R, set done; drop w from R
combine (join bag)         : R := R1 ∪ R2 ;  done := done1 OR done2
final                      : done (or t in R at the root)
```

`2^(k+1)` states (`*2` for the flag) -- for series-parallel data (`tw=2`) a
handful -- so the ABS circuit has constant treewidth `O(2^k)` and feeds the
existing `dDNNFTreeDecompositionBuilder` unchanged. Undirected connectivity is
the same idea with a *partition* of the bag in place of a subset (`Bell(k+1)`
states).

The minimal seed: take `tw(I)=1` outright -- **probabilistic tree / forest
data** (XML / JSON, taxonomies, phylogenies, org charts), the original ABS
"trees" case. The tree decomposition is the document itself (no min-fill), tree
patterns map to tree automata natively, and ancestor / reachability queries
collapse to a 2-3 state automaton. It ties into the probabilistic-XML line and
makes a clean first prototype before general bounded-treewidth graphs.

### Route A -- full pipeline: data decomposition + tree automaton (deferred)

The faithful ABS construction. Build a tree decomposition of the instance's
Gaifman graph; compile `Q` to a bottom-up tree automaton; run the automaton over
the tree encoding while emitting provenance gates per transition; hand the
resulting bounded-treewidth circuit to the existing d-DNNF builder.

- *Cost: very high.* FO / MSO to tree-automaton compilation is non-elementary in
  `|Q|` in the worst case; even restricted to CQ / UCQ, the encoding and the
  automaton construction are research-grade work to land inside a Postgres
  extension.
- *Payoff:* the only route that delivers the provable linear-time guarantee on
  the queries where the lineage blows up.
- *Status:* deferred until a workload defeats the lighter levers (the Boolean
  fold and route-3 factoring already address the absorption-reachable relational
  shapes), against the same d4-on-natural-lineage cost-benefit that deferred
  Monet's construction in [`safe-query-followups.md`](safe-query-followups.md).
- *Prior art -- a standalone prototype already exists.* Mikaël Monet's MPRI M2
  internship (2015, supervised by Senellart), *Probabilistic Evaluation of MSO
  Queries on Bounded Treewidth Instances*, implements this exact pipeline in
  Java: the `libtw` (`nl.uu.cs.treewidth`) tree-decomposition of the instance's
  Gaifman graph, the `lethal` tree-automata library, an on-the-fly automaton run
  that emits a Boolean provenance circuit, circuit-shrinking optimisations
  (final-state collapse, fact-irrelevance detection), linear-time probability
  evaluation, and a head-to-head benchmark against MayBMS. Its
  `tree/FunctionAutom*.java` are hand-written per-query automata and
  `tree/SFC.java` is a hand-coded MSO connectivity automaton. The report's
  stated limits corroborate this study: (i) MSO-to-automaton compilation is
  non-elementary and *was done by hand* (no implemented compiler), exactly the
  "genuinely new hard core" below; (ii) "treewidth quickly becomes a limiting
  factor". It is a reusable design reference (especially the on-the-fly automaton
  and the propagator-state trick that keeps the circuit linear) rather than
  something to port, so Route A is not a from-scratch research bet.

#### What the automaton looks like

A bottom-up automaton `A = (Q, delta, F)` runs over a tree encoding: bag elements
carry names `1..k+1`, and a node may carry one fact `R(i1, ...)` over active
names whose provenance annotation `x_t` is a Boolean input. The provenance
circuit is then mechanical -- a gate `g[nu,q]` per node `nu` and state `q`:

- leaf carrying fact `t` (present `-> q+`, absent `-> q-`): `g[nu,q+] = x_t`,
  `g[nu,q-] = NOT x_t`;
- internal node, children `l`, `r`:
  `g[nu,q] = OR over {delta(q1,q2,a)=q} of (g[l,q1] AND g[r,q2] AND chi_a)`;
- output: `OR over q in F of g[root,q]`.

Size is `O(|Q|^2 * |E|)` (linear in the instance); **treewidth is `O(|Q|)`,
independent of `|I|`**, because wires only ever connect a node's gates to its
children's. So the **state count *is* the treewidth bound**. The pathological
relational shapes are all constant-state automata -- e.g. the in-star self-join
("at least two of `n`") becomes a running counter capped at 2, three gates per
spine node, the textbook linear-size treewidth-~3 sequential-threshold circuit
for the same function ProvSQL emits as a `Theta(n)`-treewidth sum-of-products.
The two moves a relational-plan builder has no analogue for are: **restrict the
state to the active bag** (forget = drop) and **cap the summary**. This is the
concrete case for Route C over more folding, and shows the bound is a property of
*how* the circuit is built, not of the query fragment.

### Semiring threads

- **Boolean / probability** -- primary thread, served by the existing
  `TreeDecomposition` -> `dDNNFTreeDecompositionBuilder` ->
  `dDNNF::probabilityEvaluation` path (already linear-time on bounded treewidth);
  matches the `provsql.boolean_provenance` regime.
- **General m-semirings** -- the ABS construction is semiring-agnostic, but
  ProvSQL's any-semiring evaluation runs over the `GenericCircuit`
  (`provenance_evaluate_compiled`), which does not exploit treewidth at all. A
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

Monet's 2015 Java prototype (see Route A, *Prior art*) is a worked, end-to-end
reference for the first three in the relational (acyclic) case, so the design
space is mapped even where the code is not directly portable. Only the
cyclic-provenance (cycluit) semantics for Route C is genuinely uncharted.

## Priorities

1. **Route C** (recursive / reachability on bounded-treewidth graphs) -- the only
   place treewidth genuinely *grows* with `|I|` at constant `tw(I)`; the automaton
   is trivial; it composes with the in-flight recursive-query work. The
   substantive build.
2. **Factoring lever (route 3), `costar` threshold half** -- structural
   detection; scope only if a real workload exhibits it.
3. **Route A** (full automaton) -- deferred until the factoring lever proves
   insufficient.
