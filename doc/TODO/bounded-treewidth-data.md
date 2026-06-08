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

**Status: the seed below is implemented** (1.10.0-dev). `ReachabilityCompiler`
(`src/ReachabilityCompiler.{h,cpp}`) compiles s-t reachability over a
probabilistic `edge` relation along a min-fill tree decomposition of the *data*
graph (`TreeDecomposition` gained a public `Graph` constructor with an
elimination-bag map; `Graph` a builder for arbitrary node/edge sets).  The
construction is the bag-state dynamic program whose state is the
transitively-closed reachability relation over bag ∪ {s, t}; its transitions
emit a d-DNNF **directly** -- states are mutually exclusive and exhaustive, so
the OR gates are deterministic by construction; each edge variable is
introduced at a unique bag, so the AND gates are decomposable by construction
-- which is then evaluated by the existing `dDNNF::probabilityEvaluation`.  No
knowledge-compilation step, no circuit-treewidth cap: `MAX_TREEWIDTH` now
bounds the *data* treewidth, which is exactly the assumption.  (The dedicated
`reachability_probability(rel, ...)` wrappers this seed first shipped with
were replaced by the planner integration of the v1 plan below; the columnar
`reachability_evaluate` / `reachability_compile_stats` forms remain as
internal/testing surfaces.)  Directed reachability and
undirected connectivity are both supported (the relation-based state makes the
undirected case the directed one with both arcs -- no `Bell(k+1)` partition
machinery), and **cyclic data is handled natively**, with no
`boolean_provenance` value-fixpoint.  Differentially tested against
possible-worlds enumeration; regression test `btw_reachability`.

Measured on 2×n ladders (`tw(I)=2`): the d-DNNF is linear (~46 gates per rung,
≤10 DP states throughout), 300k edges compile + evaluate exactly in seconds,
while the lineage route crosses `MAX_TREEWIDTH` from n=14 (~40 edges) on, and
the cyclic/undirected case exceeds two minutes via the boolean fixpoint
already at n=10 (28 edges) against ~4 ms here.

A design deviation from the cycluit plan: the recursion's fixpoint is
evaluated *inside the DP state* (the transitive closure of the bag relation),
so the emitted circuit stays an acyclic d-DNNF -- the cyclic-provenance
(cycluit) semantics is not needed for this query family.

#### Agreed v1 plan: planner integration replacing the dedicated function

**Status: implemented**, including the first staged extensions.  Everything
below is in the tree: the certificate (5), `independent` (6) and
`interpret-as-dd` (7) consumers (with iterative island walks, and an
iterative `GenericCircuit::evaluate`, since certified circuits are as deep
as the data), the all-targets construction (3), the content-addressed
materialisation (4) -- plus, in the load path, certificate round-tripping
through `createGenericCircuit` -- the rewriter detection with plan-time
fallback (2), and the surface rework (8).  Of the staged extensions, the
undirected (CASE / `IN`) shape and deterministic edge-column filters are
done; multi-source base arms and the rest are still open (below).
Measured end to end through the plain recursive query: a directed 2x10000
ladder (29998 probabilistic edges) answers exactly in ~5.4 s, the
undirected *cyclic* 2x1000 ladder in ~0.3 s -- against >120 s at 28 edges
for the boolean-fixpoint route.  Tests `btw_reachability` (columnar) and
`btw_recursive` (integrated, PG15+).

The dedicated `reachability_probability` surface is the wrong interface: the
technique must surface through the **normal ProvSQL query rewriter**.  Agreed
design (June 2026):

1. **Boolean only, hence GUC-gated.**  The construction computes the Boolean
   function (mutually exclusive states, negated edge literals); general
   semirings are out (cyclic recursion has no finite provenance outside
   absorptive semantics anyway; the semiring thread below is a separate axis).
   The integration therefore activates only under
   `provsql.boolean_provenance = on` -- coherently, the only regime where
   `eval_recursive` accepts cyclic data today.
   *Superseded (June 2026): the gate widened to the `'absorptive'`
   provenance class.*  The certified d-DNNF evaluates exactly in **any
   absorptive semiring** -- its deterministic world enumeration surfaces
   every minimal derivation support (every path) as a world term, and
   absorption (`1 ⊕ a = 1`) erases the rest, so the value is the image of
   the Sorp provenance; differentially validated for nonnegative min-plus
   (including cost-0 edges, whose negation's native `monus(one, x)` kills
   only worlds dominated by their free-edge supersets at equal cost) and
   for Viterbi, Łukasiewicz, max-min and a Boolean set lattice whose monus
   is a genuine complement (the interval-union case: pointwise-exact, so
   `sr_temporal` computes temporal reachability).  The
   materialised roots are wrapped in `assume(·, 'absorptive')` (created in
   the C materialisers, the hops canonicals keyed on the wrapped tokens),
   so counting / why-provenance refuse loudly while probability and the
   absorptive semirings pass through; min-cost reachability via
   `sr_tropical(..., nonnegative => true)` works on plain, hop-bounded
   (constrained shortest path) and per-region (any-member) tokens.  Test
   `btw_tropical`.
2. **Detection in `lower_recursive_cte`, decision fully at plan time.**  The
   method catalog cannot arbitrate between construction routes: the fork
   happens before any artefact exists, and the lineage route's cost is
   unknowable without running the fixpoint.  One-sided plan-time rule: GUC on
   + shape match (base arm = constant source, recursive arm = single
   equi-join of a tracked *base* relation with the recursive table, optional
   deterministic quals on edge columns) + cheap degeneracy lower-bound probe
   on the data graph; then *attempt* the decomposition route under its hard
   caps and **fall back to `eval_recursive`** on any failure
   (`TreeDecompositionException`, state bound), preserving today's behaviour
   exactly.  Catalog facts replace per-row checks: a base tracked relation
   has fresh independent `input` tokens by construction; `repair_key` /
   `mulinput` relations are refused (v1); directionality is read off the
   query shape, not reverse-engineered from token sharing.
3. **Tokens, not numbers: all-targets construction.**  One bottom-up pass
   (per-subtree state tables) plus one top-down pass (rest-of-graph tables),
   reading each vertex at its elimination bag, yields the reachability
   circuit of *every* vertex in linear total size -- one ordinary provenance
   token per `reach` row, no per-target recompilation.  Everything downstream
   (`probability_evaluate`, `sr_formula`, Shapley, composition) then works
   unchanged.
4. **Materialisation without new gate types.**  The d-DNNF is stored with
   `plus` / `times` gates and `NOT x` encoded as `monus(one, x)` (the Boolean
   reading already evaluates it as such).  Gate UUIDs are content-addressed
   with the standard v5 recipes, so identical sub-circuits dedup across
   constructions and re-running the query on unchanged data re-derives the
   same gates (idempotent `createGate` doubles as a cache).
5. **The d-DNNF certificate.**  Per-gate marking in the (gate-type-specific)
   `info1` field, value 1: on `gate_plus` "deterministic" (children mutually
   exclusive), on `gate_times` "decomposable" (children variable-disjoint).
   Free space-wise, backward compatible (absence = uncertified), and sound
   under content-addressing: the mark asserts a property of the gate's
   children, a global semantic fact that remains true however the gate is
   later re-derived.  Trust model as for the inversion-free certificate.
6. **`independent` evaluates certified circuits** (no new catalog method): a
   certified d-DNNF is the generalisation of read-once, so marked OR = sum
   (mutual exclusivity replaces variable-disjointness), marked AND = product.
   Island discipline for sound mixing: each certified island is walked with
   island-local memoisation and registers its inputs in the global seen set;
   cross-island or certified/uncertified entanglement throws and the chooser
   escalates.  Compositions of certified islands over disjoint inputs (e.g.
   reach events on different graphs under a read-once skeleton) evaluate
   linearly for free.
7. **`interpretAsDD` honours the marks too**: marked OR copied as a native
   deterministic OR (no De Morgan rewriting), same island discipline, NOT
   only over inputs checked.  This opens the whole d-DNNF-artefact surface
   to certified circuits -- `shapley` / `banzhaf` (edge criticality on
   reliability networks), `compile_to_ddnnf*`, `ddnnf_stats`, conditioning --
   linearly and without external compilers, through the existing `makeDD`
   ladder which already tries `interpret-as-dd` first.
8. **Surface rework.**  The user-facing `reachability_probability` /
   `reachability_compile_stats(regclass, ...)` wrappers are removed; the
   columnar `reachability_evaluate` forms remain as internal/testing
   surfaces.

#### Join-defined graphs: the disjoint-support half is implemented

The recursive arm may join a derived edge subquery (an inlined view
included): the rewriter deparses it, the gathering walks each derived
token's circuit through the conjunctive gate types (`times` / `project` /
`eq`) to its input leaves (`token_conjunctive_leaves`), accepts the edge
as a compound variable when every token is a pure conjunction and the
supports are pairwise disjoint across edges (probability = product over
the support; the compound token itself is the literal, so the materialised
circuit references it directly and the island evaluator's uncertified
fallback evaluates it read-once), and otherwise raises -- which the driver
turns into the usual fallback, exercised by the overlapping-support test.
The *shared-support* half (the faithful variables-in-the-decomposition DP
with late-branching states, where tractability depends on how widely
tuples are shared) remains the open extension, along with static
certification of disjointness from keys/FDs to skip the dynamic walk.

#### BID blocks: implemented

`repair_key` edge relations compile natively: the gathering classifies
`mulinput` tokens by their key variable, and each block becomes one
(k+1)-way deterministic branching in the DP -- per alternative, the arcs
applied and the outcome gated by its `mulinput` literal; the none outcome
(probability `1 - sum p_i`, weight zero under exactly-one `repair_key`
semantics) gated by `monus(one, plus(mulins))` with the plus *marked
deterministic* (the alternatives are mutually exclusive by construction).
Endpoint co-location is forced by a clique among each block's endpoints in
the decomposed graph -- the honest treewidth condition for BID data.  The
certified-island evaluator registers a block's key variable once per
island, so the linear `independent` route covers BID circuits.
Differentially tested (200 random mixed instances incl. sub-stochastic
blocks) against outcome-enumerating brute force, and in-database against
the generic fixpoint; a 1000-vertex ladder whose every rung is a two-way
`repair_key` block evaluates exactly in ~0.3 s through the plain recursive
query.

#### Multi-source base arms: implemented

`SELECT v FROM sources` base arms are recognised and compiled: the compiler
gained certain (ungated) arcs and a virtual super-source whose arcs to the
sources are gated by the source tuples' tokens when the source relation is
tracked (probabilistic source sets) and certain otherwise; the gathering
maps sources into the shared dense-ID space, and `reachability_materialize`
takes the source arrays (nil UUID = certain).  Differentially tested (200
random instances, every vertex, mixed certain/probabilistic sources)
against brute force, and in-database against the generic fixpoint route on
the same query.  The implementation surfaced a real compiler bug: `join`'s
trivial-table shortcut tested only `gate == TRUE`, which dropped the
relation of single-state TRUE tables that certain arcs produce -- the
identity-relation check now closes it.

#### Table-characterisation registry: consulted

The gathering now reads `get_table_info` before inspecting any row.  A
relation the registry certifies as TID skips per-row gate introspection
entirely (tokens are known base inputs, block keys nil); a BID relation
gets its block keys *from the registered key columns* -- a v5 UUID over
the key values, computed inside the gathering CTAS while the columns
are in scope -- with a single `get_gate_type` per row only to separate
`mulinput` alternatives from independent rows added by plain `INSERT`
after `repair_key` (the registry keeps the BID kind for those, and the
mixed case is exact -- tested against possible worlds).  Source
relations symmetrically: TID skips the input-gate check, BID raises
immediately (block-correlated source sets are not expressible as
independent super-source arcs), surfacing as the usual clean fallback
notice.  Derived, unregistered, or subquery-defined edges keep the
fully dynamic per-token path (shape validation, conjunctive supports),
untouched beneath as the safety net; the C compiler's shared-token
check guards all paths regardless.  A pleasant consequence of the
lineage classifier: a CTAS that lifts the `provsql` column verbatim
from a TID source is itself certified TID, so derived snapshot tables
ride the fast path too.  Measured on the 30k-edge ladder: gathering
~0.6 s -> ~0.45 s, integrated query ~2.8 s end-to-end.

*Deliberately deferred*: ancestry-based static disjointness
certification (rejecting or accepting join-defined edge relations by
`get_ancestors` overlap).  Ancestor overlap is the wrong granularity --
two derived edge relations over the same base table can still have
pairwise disjoint supports (the join-key test is per *tuple*, not per
relation) -- so a static rejection would be strictly more conservative
than the dynamic check the route already runs, and a static acceptance
would still need the per-tuple conjunctive-shape walk for the block
keys and probabilities.  Ancestry stays what it is elsewhere: the safe
rewriter's concern.

#### Bounded-hop reachability: implemented

The hop-counting CTE shape -- a counter column seeded by an integer
constant, incremented in the recursive arm, bounded by a mandatory
`WHERE r.hops < B` / `<= B` qual (either column order, any seed; the
bound is required because an unbounded counter has no fixpoint on
cyclic data) -- compiles through the same DP with a richer state
algebra: relation entries refine from reachability bits to *sets of
achievable walk lengths* up to the bound (bitmasks, cap 62), composed
in the capped Minkowski semiring by algebraic-path closure with
diagonal star.  The scaffold is one template over the state ops
(`BoolOps` / `HopOps` in `ReachabilityCompiler.cpp`); worlds still map
to exactly one state, so the determinism/decomposability argument and
the certificate carry over verbatim.  Row `(v, h)` faithfully matches
the generic fixpoint: "some *walk* of exactly h edges" (cycles pump
lengths; self-loops, droppable for plain reachability, are kept as
weight-1 arcs here precisely because the fixpoint derives `(v, h+1)`
from them).  Multi-source arcs contribute length 0; the undirected,
filtered, BID and multi-source variants compose with the counter.
Differentially tested (400 random instances, per-(v,h) *and*
within-bound, against world-enumerating brute force) and in-database
against the generic fixpoint.  Scaling on the 2xN undirected cyclic
ladder (p=.9): `max_states` is independent of N -- 143 at L=15, 408 at
L=30, a function of (treewidth, bound) only -- gates linear in N
(228k at N=1000 to 2.07M at N=10000), compile 0.39 s to 3.7 s, 0.73 s
end-to-end through SQL at N=1000.

The natural follow-up query -- `SELECT node FROM reach GROUP BY node`,
"which nodes are within k hops" -- ORs each vertex's per-length tokens,
which are *correlated* (lengths share edges): unmarkable as
deterministic (the certified evaluator would sum overlapping children)
and otherwise generic-evaluation territory.  But the DP natively owns a
deterministic circuit for the same Boolean function (the OR over
mutually exclusive (below, above) pairs with a nonempty length set,
i.e. the decomposition by world-class rather than by length), so the
materialiser *pre-creates the gate the deduplication will address*:
at `uuid5('plus-canonical{sorted per-length tokens}')`, a certified
single-child plus over the within-bound root.  `provenance_plus`
probes that canonical address before creating anything and never
creates under it, so a hit is always a deliberate plant and ordinary
plus gates are byte-identical to before.  Two rejected designs, for
the record: sorting `provenance_plus` recipes globally (canonical, but
gate sharing then couples formula rendering across queries through the
shared store, run-randomly -- the suite caught it), and probing the
plain sorted recipe (ordinary creations occupy it by coincidence, 50%
for pairs).  Also fixed en route: computing `array_agg(t)` and
`array_agg(t ORDER BY t)` in one SELECT makes the planner feed *both*
aggregates sorted input, scrambling the stored children order.

*Cross-vertex aggregations: implemented* (the S-bit compilation).
`compileAnyReach` extends the DP state by one bit per domain position
-- "reaches some S-vertex within the part", the Courcelle congruence
for the existential query: seeded by domain membership, propagated
backwards by closure, folded for free under forgetting -- and reads
the single acceptance bit off the root's below-table, so one bottom-up
sweep suffices (no top-down pass).  The lowering detects, before CTE
inlining, the outer shape `GROUP BY T.g` over `reach JOIN T ON node =
T.a` (single grouping column of a single joined *untracked* relation,
one join equality, no other quals; PG 18's synthetic RTE_GROUP is
resolved through `groupexprs`), and plants, per multi-member group,
the certified any-member circuit at the canonical address of the
group's token multiset (`plant_reach_any_groups`, best-effort with
fallback notices).  Differentially tested (400 random instances incl.
BID blocks and probabilistic sources -- which exposed a vertex-id
collision between a caller-supplied set member and the virtual
super-source, fixed by intersecting the set with the edge/source
universe); pinned in-database by `btw_anyreach` (region reliability
exact vs possible-worlds, tracked-member skip).

*Shared multi-group sweep: implemented* (`compileAnyReachAll`).  The
naive product state (all groups' bits in one state identity) explodes,
so the sharing is structural instead: the prelude -- variable
grouping, tree decomposition, bag assignments, literal gates -- is
built once, one bottom-up sweep runs per group, and the emission is
*hash-consed* (AND keyed on the packed child pair, OR on the sorted
children), so the parts of the circuit a group's seeds do not touch
are literally the same gates across groups, materialised once by a
single batched store pass.  2.2x end-to-end on the 15k-edge ladder
with 100 regions (35 s -> 16 s, byte-identical planted canonicals;
cold = warm, since shared gates ship once).  The residual cost is the
per-group sweep itself (~60%): the bits must propagate from each
group's seeds to the root, so the "dirty" region is the seed-to-root
bag paths -- about half the decomposition on path-like data.  *Next
leap if needed*: per-group virtual collector chains (member -> chain
arcs, "some member reachable" = "chain end reachable") turn any-reach
into per-vertex reads of one two-sweep compileAll pass, in width
+O(batch) for batched groups with chain order aligned to the
elimination order; needs the width impact validated empirically.

*K-terminal / Steiner: implemented* (`compileCoverReach[All]`,
`CoverOps`).  The richer congruence is the *pending rescuer-set
antichain*: a forgotten terminal not yet reached by the source pends
on the boundary positions that reach it; the sets stay closed under
the relation (one backward pass per closure), shrink by plain
intersection at forgets (lossless by transitivity), discharge on
acquiring the source position, and the empty set is the absorbing
reject; acceptance, after a final re-expression onto the singleton
source domain, is the empty antichain.  Worlds still map to one state
each, so the certificate carries over -- probability gives k-terminal
reliability, nonnegative min-plus the exact directed Steiner cost
(cheapest covering subgraph, shared edges paid once).  `max_states`
stays a function of the treewidth alone (28 on tw-2 ladders,
independent of N and of the terminal count; the Sperner bound
C(d, d/2) is the worst case).  Surface: self-join conjunctions
(`FROM reach r1, reach r2 WHERE r1.v = c1 AND r2.v = c2`) detected at
lowering and planted at the *times-canonical* address probed by
`provenance_times` -- the exact twin of the plus-canonical machinery,
so any phrasing computing the same token multiset lands on the
planted gate.  Per-terminal pitfall caught differentially: a vertex
appearing only in self-loops never enters the DP and must count as
absent (constant false), not silently covered.  Test `btw_cover`.
(The grouped HAVING `count(*) = |group|` surface was considered and
dropped: proving the constant equals the group size is data-dependent
and the HAVING cmp construction has no probe point; the self-join
form covers the need.)

*DISTINCT-shaped aggregations: implemented* (detection-only, no new
compiler work).  `SELECT DISTINCT region` is provenance-identical to
`GROUP BY region`; the existing `transform_distinct_into_group_by`
already normalises it, but it ran *after* `inline_ctes` (hence after
the reachability detection inside it).  Hoisting a guarded
`normalize_distinct_into_group_by` to before `inline_ctes` (idempotent
with the historical late site) makes both the any-reach and the
k-terminal detectors see the `GROUP BY` form with zero
DISTINCT-specific arms.  Folded into `btw_anyreach` (the DISTINCT twin
gives a byte-identical result).

*Open*: extra member-relation quals in the detection.

*Other reuse candidates for the canonical-address machinery*: the
times-canonical namespace now exists (serving the k-terminal
conjunctions above) and could equally serve factored forms if Route 3
materialises; canonical agg-gate addressing for commutative aggregates
(pure dedup -- only if duplicate agg gates show up in profiles).  The
general rule learned: planted gates must live in namespaces ordinary
creation never touches.

#### Degeneracy pre-probe: implemented, measured, not enabled

`TreeDecomposition::degeneracyLowerBound` now has a `Graph` overload (the
`BooleanCircuit` one delegates), so probing a data graph before min-fill is
a one-liner.  Measured, the probe never wins: graphs above the cap hand
min-fill a high-fill node almost immediately, so its own abort is at least
as fast as the O(V+E) peel on every adversarial family tried -- cliques
(K200: 0 ms abort vs 8 ms probe; K500: 7 ms vs 39 ms; K1000 probe 248 ms)
and supercritical random graphs (n=20000, m=60k/100k: 45/51 ms abort vs
62/78 ms probe) -- while an always-on probe taxes every *accepted*
compilation by a linear pass (~0.1-0.2 s at 300k edges).  The probe is
therefore not wired into the compiler; the `Graph` overload stays for the
chooser-style uses where the bound is wanted without an attempt.

#### Decomposition caching: measured, redirected to a creation cache

Per-relation caching of the min-fill decomposition was the plan, but the
measurement undercuts it: on bounded-treewidth data the decomposition is
3-4% of `compileAll` (10 ms of 284 ms at 30k edges, 106 ms of 3.3 s at
300k) -- min-fill is only expensive on circuit-shaped graphs near the cap,
not on genuinely treelike data.  What repeat queries actually pay for is
the gate-creation IPC of re-materialising an unchanged circuit, and
content addressing makes that cache trivial: the materialiser now keeps a
per-backend set of tokens it has already created (sound: the store is
append-only and the worker pipe ordered; bounded by a clear-past-cap, which
only costs re-sending idempotent creates).  Warm repeat of the 30k-edge
integrated query: ~5.6 s -> ~5.2 s.  The remaining warm profile is the
edge gathering (~0.6 s: per-token `get_prob` / `get_gate_type` round
trips, batchable into an array call if it ever matters), the evaluation's
circuit load (~1.6 s), and the driver's temp-table work -- none of which a
decomposition cache would touch.  (The gathering cost has since been
cut by the registry consultation below.)

#### Non-recursive queries: what the new infrastructure changes

The relational pathologies of the introduction, revisited with the v1
machinery in place:

- **Cross products** (`SELECT DISTINCT 1 FROM e a, e b`): already served --
  the independent-product factoring and the Boolean fold cover the
  disjoint and absorption-reachable shapes, as noted above.
- **The `costar` threshold half** (the in-star self-join, "at least two of
  `n` edges true"): the *function* is a symmetric threshold, and its
  textbook linear-size circuit -- the running counter capped at `k` -- is a
  **certified d-DNNF by construction** (the counter states partition the
  worlds; each variable branches deterministically once).  Two
  consequences.  First, the equivalent `HAVING count(*) >= k` formulation
  is *already* exactly evaluated in polynomial time by the
  `CountCmpEvaluator` DP, so the pathology only bites when the workload
  insists on the self-join form; route 3 is therefore purely a
  *recognition* problem (rewrite the self-join-through-a-centre shape to
  the counting form), and stays deferred until a workload exhibits it.
  Second, when it is recognised, the emitted counter circuit should carry
  the d-DNNF certificate, making the result a first-class token evaluable
  by `independent` -- the certificate is the missing output channel route 3
  previously lacked.
- **Fixed-length path patterns** (self-join chains
  `e a, e b [, e c] WHERE a.dst = b.src ...` with `DISTINCT` projection,
  i.e. bounded-hop reachability): **done on the recursive side** -- the
  depth-bounded recursive CTE is recognised and compiled with
  walk-length-set states (see "Bounded-hop reachability: implemented"
  above; the state refinement landed as length *sets*, not capped
  minima, to match the fixpoint's exact-length row semantics).  What
  remains of this bullet is the non-recursive trigger: rewriting a
  self-join chain into the depth-bounded CTE shape (or detecting it
  directly), worthwhile only if such chains show up in real workloads.
- **Multi-terminal variants** (Steiner / k-terminal reliability, "all of
  these nodes pairwise connected"): same DP with the terminal set forced
  into the domains; the acceptance condition reads several bits instead of
  one.  Expressible today only as dedicated calls (no natural SQL shape),
  so the columnar surface is the place to expose them if a use case
  appears.
- **Route A (general MSO / CQ over treelike data)** remains the research
  bet, but its integration cost has dropped: the provenance-emitting
  automaton run no longer needs new evaluator machinery -- it only has to
  *mark its gates* (deterministic state ORs, decomposable transition ANDs)
  and materialise through the same content-addressed channel, and the
  whole evaluation and artefact surface (chooser, `independent`,
  `interpret-as-dd`, Shapley) picks the result up.  What is still missing
  is exactly the query-to-automaton compiler, as before.

The original analysis follows.

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
- **Absorptive semirings** -- *landed* (June 2026): the route's certified
  d-DNNFs evaluate exactly in any absorptive semiring through the plain
  `GenericCircuit` evaluation (linear in the circuit, hence in the data for
  bounded treewidth), with no width-aware evaluator needed -- the
  decomposition already shaped the circuit.  This is the compile-once /
  evaluate-per-semiring counterpart of the 0-closed algorithms of Ramusat,
  Maniu & Senellart (EDBT 2021): Sorp(X) is the free 0-closed semiring, and
  the certificate's evaluation computes its image; min-plus gives Dijkstra's
  answers (plus hop-bounded and any-of-S variants Dijkstra does not answer
  directly), on the treewidth axis NodeElimination exploits.
- **General (non-absorptive) m-semirings** -- the ABS construction is
  semiring-agnostic, but ProvSQL's any-semiring evaluation runs over the
  `GenericCircuit` (`provenance_evaluate_compiled`), which does not exploit
  treewidth at all.  A bounded-treewidth `GenericCircuit` would still need a
  *width-aware semiring evaluator* (a bag-by-bag dynamic program analogous to
  the d-DNNF builder, but over the semiring carrier) to cash in the bound --
  and a finiteness story for recursion (Mumick-Shmueli) first.  A separate,
  larger axis.

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
   substantive build.  **Done**, including the one-time follow-ups
   (`WITH RECURSIVE` shape recognition, token output, edge Shapley
   values) and the extension round: multi-source, BID blocks,
   join-defined edges, registry consultation, bounded hops.  Remaining
   Route C items are listed in their status notes (shared-support
   join-defined graphs, cross-vertex aggregations / S-bit compilation,
   non-recursive triggers).
2. **Factoring lever (route 3), `costar` threshold half** -- structural
   detection; scope only if a real workload exhibits it.
3. **Route A** (full automaton) -- deferred until the factoring lever proves
   insufficient.
