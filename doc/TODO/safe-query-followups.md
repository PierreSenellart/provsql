# Safe-Query Follow-Ups

Ideas surfaced during the safe-query / `provsql.boolean_provenance`
discussion that are deferred but worth coming back to. The work that
has already landed is described in the *Safe-Query Rewriter* section
of `doc/source/dev/query-rewriting.rst`.

## Boolean-only optimisations

Currently implemented (all gated on `provsql.boolean_provenance`):
the hierarchical-CQ safe-query rewriter (`src/safe_query.c`) with the
six FD-aware extensions, the persistent `gate_assumed_boolean` marker
emitted at the rewritten root, and the load-time
`foldBooleanIdentities` peephole (idempotence, plus-with-one
absorber, absorption) with its in-memory side-band Boolean-assumption
set.

Further down the road:

- **Factor-structure probability via independent-subtree detection.**
  Even when the safe-query rewriter doesn't apply, scan the circuit
  for `gate_plus` / `gate_times` whose children have pairwise-disjoint
  leaf supports; evaluate probability componentwise.  Strictly more
  general than `independentEvaluation` on read-once circuits.
  *Tried MVP-1 (top-level factoring with per-component
  tree-decomposition fallback) ; benchmark showed it is 3-14x slower
  than plain tree-decomposition on every disconnected-component shape
  tested (4-16 components, 80-320 leaves).  The existing d-DNNF
  builder already factors disconnected components internally, so the
  outer factoring just adds traversal + virtual-gate overhead.  Any
  future attempt should target a regime where tree-decomp's
  heuristic actually struggles on the joint circuit -- e.g. large
  circuits where the elimination-order heuristic gets confused by
  top-level cross-talk.*
- **Read-once recognition (Golumbic-Mintz-Rotics, linear-time).**
  When a Boolean formula has a read-once representation, the same
  probability shortcut as safe-query applies without needing to
  recognise the SQL shape upstream.
  *Deferred as theoretical-interest-only after review.  GMR
  uniquely catches functions that are read-once semantically but
  not syntactically (e.g. (a∨b) ∧ (a∨c) = a ∨ (b∧c)) ; that
  requires distributivity rewrites beyond what
  `foldBooleanIdentities` does.  In ProvSQL practice the cases
  where GMR would help over the existing safe-query rewriter +
  Boolean-fold pipeline are narrow and hard to characterise from a
  query class.  40 years after the algorithm, no probabilistic-DB
  system that we know of implements it.  GMR also assumes input in
  DNF, which is exponential to extract from an arbitrary circuit.
  Revisit if a real workload surfaces queries that would
  meaningfully benefit.*
- **First-class negation gate (`gate_not`).**
  Lets monus normalise to `a ∧ ¬b` under Boolean and opens De
  Morgan / NOT EXISTS / EXCEPT rewrites.
  *Deferred.  Adding `gate_not` to `GenericCircuit` taxes every
  Boolean-compatible semiring that isn't strictly Boolean (notably
  `IntervalUnion`, which has no natural negation), so the change
  would belong in `BooleanCircuit` instead.  But the
  `BoolExpr` semiring used by `getBooleanCircuit` already
  translates `gate_monus(a, b)` into `AND(a, NOT(b))` at conversion
  time, so `BooleanCircuit::NOT` already exists in practice ; we
  just don't apply Boolean-with-negation simplification rules
  (NNF push, ¬¬x → x, x ∧ ¬x → ⊥) on it.  Those rules would
  matter mainly for EXCEPT- and `update_provenance`-heavy
  workloads, and even there `makeDD` accepts NOT gates anywhere
  (not just at leaves) so NNF normalisation buys little.  Revisit
  if a real workload makes the case ; the more interesting axis is
  extending the safe-query rewriter to handle UCQ with one layer
  of negation, not Boolean-level rules in isolation.  See also
  the Möbius / Monet entry below : a future Monet-style rewriter
  would systematically emit `gate_monus(one, x)`, which `gate_not`
  would render more ergonomic, but `gate_monus` already suffices,
  so Monet does not, by itself, justify a new gate type.*
- **Möbius / inclusion-exclusion via Monet 2019's construction.**
  Beyond hierarchical CQs, the Dalvi-Suciu dichotomy puts many
  non-hierarchical UCQs in PTIME, with the canonical algorithm
  being lattice-walking inclusion-exclusion which is naturally
  extensional.  Monet (arXiv:1912.11864) closes the architectural
  gap by giving a PTIME construction of a deterministic-decomposable
  circuit, using negation in place of IE ; the output drops
  straight into our intensional pipeline (`gate_monus` already
  means `AND(a, NOT(b))` under Boolean, so no new gate type
  required).
  *Deferred ; the architectural mismatch is no longer the blocker
  but practicality is open.  The construction is PTIME but the
  polynomial degree depends on query structure, not linear in
  database size ; the emitted circuit may be much larger than the
  natural lineage ; the safety detector is meaningfully more
  expensive than the hierarchical one ; the paper covers a strict
  subset of safe non-hierarchical UCQs (the inversion-free
  fragment) ; and no implementation has appeared in the six years
  since the paper, so there is no empirical evidence for the
  practical regime.  The honest cost-benefit is that for any safe
  non-hierarchical workload we encounter today, handing the
  natural lineage to d4 is likely as fast or faster than
  constructing-then-evaluating Monet's circuit.  Revisit only if
  a benchmark surfaces where d4 visibly chokes on a safe
  non-hierarchical UCQ and Monet's construction plausibly helps.*
- **Other tractable CQ subclasses.**
  Forest-shaped CQs are a subset of hierarchical (no new ground).
  Treewidth-parameterised CQs would bound treewidth on the query
  hypergraph rather than on the circuit graph we already exploit
  in `TreeDecomposition` ; the evaluation set does not change, only
  when we discover bounded treewidth.  Neither is worth a dedicated
  slice.
- **BDD / SDD compilation with Boolean-only minimisation.**
  When the circuit is neither read-once nor independent-factorable,
  compile to a decision diagram ; under Boolean, the variable
  ordering and reduction rules are well-known and the resulting
  structure supports model counting in linear time.
  *Skip.  d4 is state-of-the-art for d-DNNF compilation and we
  already wire it as the default knowledge compiler ; for one-shot
  WMC d-DNNF dominates BDD.  The genuine BDD/SDD advantage is
  canonicity, which only pays off across query reuse (apply /
  restrict / equivalence checks).  ProvSQL has no cross-query
  diagram reuse story today, and adding one would be a much bigger
  project than the compiler itself.  Revisit only if cross-query
  diagram caching becomes a goal.*

## HAVING-clause provenance optimisation

Today every `gate_cmp(gate_agg(semimod children), gate_value)` reaches
`provsql_having`, which dispatches via `enumerate_valid_worlds` :
`count_enum` for COUNT (binom(N, k) minimal generators under the
absorptive-upset path), `sum_dp` for SUM, and exhaustive 2^N
enumeration for everything else (MIN, MAX, …).  For every emitted
world W the walker constructs `present_prod × monus(one, missing_sum)`
and ORs them all into a DNF that is then handed to the downstream
evaluator.  MIN / MAX hit the exhaustive path and emit 2^N clauses
even though their satisfying-world structure is trivially
closed-form.  The work-in-progress write-up at
`~/git/students/aryak/provsql_having/algorithms.tex` (algorithms
section, with proofs and Lean references) is the reference for the
chosen HAVING semantics ; the layering below matches that
semantics directly.

The natural home for the new dispatch is `pw_from_cmp_gate` in
`src/having_semantics.hpp`, which already has `mvals[]` and `kvals[]`
in hand for every cmp gate it sees.  Three layers, in increasing
specialisation.

### Layer A : closed-form circuits using the implementation's convention

For MIN(a) op c and MAX(a) op c with deterministic per-occurrence
values `m_i`, partition the group children on whether `m_i op c`.
The satisfying-world predicates have direct closed forms using the
existing per-world convention, `monus(one, x)` for "x is not
contributed" (mirroring how `monus_factor = monus(one, missing_sum)`
is built today in `pw_from_cmp_gate`) :

- `MAX >= c`, `MAX > c` : satisfying iff some good child is present.
  `S.plus({k_i : i in G})`.  Single sum, no monus.
- `MIN <= c`, `MIN < c` : dual, partition on the satisfying side.
- `MIN >= c`, `MIN > c` : satisfying iff no bad child is present and
  the group is non-empty.
  `S.times(S.monus(S.one(), S.plus({k_j : j in B})), S.plus({all k_i}))`.
- `MAX <= c`, `MAX < c` : dual, partition swapped.
- `MIN = c`, `MAX = c` : `MIN >= c` ∧ `MIN <= c` (and dual), each
  factor built from the monotone-direction forms ; combined via
  `S.times`.
- `MIN <> c`, `MAX <> c` : `S.monus(S.plus({all}), <EQ-form>)`,
  using the same convention.

All polynomial-size in N, built from `plus`, `times`, `one`,
`monus(one, _)`.  These match the per-world enumeration in
`pw_from_cmp_gate` whenever the surrounding semiring is absorptive
(Boolean, BoolExpr, Tropical, Viterbi, MinMax, IntervalUnion, …).
For non-absorptive semirings (Counting, How, Why polynomials, …) the
per-world enumeration assigns a distinct symbolic value per world ;
no shorter circuit reproduces that value, and the exhaustive path is
what is semantically intended.  **Implementation rule** : gate Layer
A on `S.absorptive()`, fall through to `enumerate_valid_worlds`
otherwise.

Already poly-time and implemented in `pw_from_cmp_gate`'s existing
path : `COUNT(*) = k`, `COUNT(*) <= k`, `COUNT(*) < k` for fixed k
(satisfying-world count bounded by binom(N, k)).  CHOOSE / PICKFIRST
is poly-time by construction (one occurrence picked deterministically
per group) ; documented in Aryak's write-up but unimplemented in this
branch.

### Layer B : absorptive-specific tightenings beyond Layer A

The Layer A closed forms above fire on every absorptive semiring.
Layer B is where the existing absorptive shortcuts for COUNT and SUM
get pinned, and where the same minimal-generator reasoning can be
documented explicitly so the test surface covers it :

- `COUNT(*) >= k` / `> k` for fixed k : `count_enum` already
  enumerates the binom(N, k) minimal generators under its
  absorptive-upset path.  No new code, but worth a pinned test that
  the emitted DNF is the minimal-generator form rather than the
  full upset.
- `SUM(a) >= c` / `> c` with `c / a_min <= k` for fixed k :
  bounded by sum_{i=1}^{k} binom(N, i) minimal generators ; `sum_dp`
  covers this, but the absorptive narrowing (compared to the
  general DP across all reachable sums) is worth pinning.

### Layer C : Boolean-specific specialisations

Boolean is more restrictive than absorptive (two-valued, with a
sound NOT via `monus(one, x)`), which buys tricks the absorptive
layer cannot use ; these apply to probability evaluation but not to
the broader Boolean-fold pipeline :

- **Probabilistic closed forms** for symmetric COUNT / SUM
  thresholds, via the
  `provsql.cmp_probability_evaluation` umbrella GUC (on by default,
  hidden diagnostic).
  - `COUNT(*) op c` over distinct `gate_input` leaves : **landed**
    as `src/CountCmpEvaluator.{h,cpp}`, the Poisson-binomial DP
    pre-pass slotted in `probability_evaluate.cpp` between
    `runAnalyticEvaluator` and `getBooleanCircuit`.  Soundness
    is certified per cmp by requiring `ref_count == 1` on every
    gate of the chain `K_i -> semimod_i -> gate_agg`, which
    guarantees the cmp's input leaves do not appear anywhere
    outside its subtree.  Partial DP dispatched on the smaller
    side of `C` (lower-tail directly, or upper-tail via inverted
    Bernoullis) ; total cost per cmp is
    `O(N x min(C, N - C))`.
  - `SUM(a) op c` over distinct deterministic-weighted `gate_input`
    leaves : same shape, replace the binomial recurrence with a
    weighted-sum DP.  Not yet implemented.
  - `MIN(a) / MAX(a) op c` : even simpler closed forms (single
    `plus` over the satisfying singletons under absorptive ;
    `1 - prod(1-p_i)` over good / `prod p_i` over good for the
    probability path).  Not yet implemented.
  All four bypass the DNF construction entirely ; each lives in
  `probability_evaluate.cpp` as a pre-pass that produces a
  Bernoulli `gate_input` and is sound only for the probability
  path.  Future cmp evaluators should gate on the same
  `provsql.cmp_probability_evaluation` flag.
- **Compact threshold-function encodings** (sorting / cardinality
  networks) for the `k ≈ N/2` regime where even the absorptive
  minimal-generator DNF is `m × binom(N, m)`.  O(N log^2 N) Boolean
  circuit instead, then standard probability pipeline.  Useful
  only at the corners ; for `COUNT(*) >= small_constant` the
  absorptive layer already produces a manageable DNF.
- **Independence certification via the safe-query rewriter.**
  Probabilistic closed forms are sound only when the children of
  `gate_agg` share no leaves.  Extending the safe-query detector
  to recognise "the FROM / WHERE / GROUP-BY subquery feeding this
  HAVING is hierarchical" certifies this property cheaply ; the
  rewriter today bails on `hasAggs` and would need a one-level
  descent for the HAVING body.

**When Layer C earns its weight.**  Layer B handles
`COUNT(*) >= small_constant` cleanly today.  Layer C pays off
specifically at the corners (median-count thresholds, EQ / NE,
SUM with mid-range thresholds) and for probability-only shortcuts
that bypass circuit construction altogether.  Worth a benchmark
analogous to the absorption bench (`test/bench/absorption_bench.sql`)
before scoping any individual sub-item.

### Discrete random_variable extensions

`random_variable` today carries continuous distributions (Normal,
Uniform, Exponential, Erlang) plus Categorical and Mixture.  HAVING
evaluators assume `m_i` extracted from `gate_semimod` is a
deterministic integer.  Extending `gate_rv` (and the surrounding
analytic evaluators) to discrete distributions (Poisson, Binomial,
Geometric, Multinomial) opens a parallel optimisation track for
HAVING when the *aggregated value* itself is uncertain :

- Sum of independent Poissons is Poisson with summed rate ;
  closed-form CDF on the surrounding `gate_cmp`.  Same shape as the
  existing Normal-sum closed form in `Expectation.cpp`.
- COUNT(*) over Bernoulli presence-tokens is Poisson-binomial ;
  same machinery as Layer C above, now generic to "the count of
  present rows is itself a random variable" rather than
  Boolean-specific.
- General convolution of independent discrete RVs : O(C × N) DP for
  the surrounding cmp.

**Integration point.**  An analogue of `runAnalyticEvaluator`
(`src/AnalyticEvaluator.{h,cpp}`) that walks `gate_cmp(gate_agg(
gate_arith over gate_rv children), gate_value)`, computes the
analytical CDF, replaces the cmp with a Bernoulli `gate_input`
leaf.  Probability-specific (the `gate_input` carries a numeric
probability, semiringly meaningless to symbolic semirings) ; lives
in `probability_evaluate.cpp` before `getBooleanCircuit`, the same
slot as the existing analytic / hybrid passes.

**Architectural fit.**  The extension stays inside the existing
intensional pipeline : new distribution variants in the `gate_rv`
`extra` blob, new closed-form CDF entries in the analytic evaluator,
new structural reductions in the HAVING pre-pass.  No new gate type ;
no change to the circuit framework itself.  The MC fallback in
`MonteCarloSampler` already handles cases where the closed form does
not apply.  The work composes with Layer C : the Poisson-binomial CDF
that Layer C would compute for symmetric COUNT over Bernoulli leaves
is exactly the closed form the discrete-RV analytic evaluator would
deliver generically.

## Hierarchical-detector follow-ups

The FD-aware safe-query rewriter currently lands six extensions on top
of the base hierarchical-CQ detector : constant-selection elimination,
PK / NOT-NULL UNIQUE FDs from the catalog, deterministic-relation
transparency, PK-unifiable self-joins, FD closure on the union-find
(detector-only), and disjoint-constant self-joins.  See
:cfile:`src/safe_query.c` and the dev-doc *Safe-Query Rewriter* section
for the implementation; the foundational papers (Dalvi & Suciu 2007 ;
Gatterbauer & Suciu 2015 ; Suciu, Olteanu, Ré & Koch 2011) are linked
from the website's *Foundational Works* listing.

The items below were surfaced during that work and deliberately deferred
for a future slice.

- **FD-induced nested rewrite (function/free split).**  The current
  FD closure accepts every query whose FD-reduced atom-sets are
  pairwise nested-or-disjoint *and* whose existing single-level wrap
  is already read-once.  When the FD closure splits the atoms into a
  *function layer* (PK-determined relations) and a *free layer* (no
  FD on them) and the function layer indexes into the free layer,
  the single-level wrap duplicates each function-layer atom's
  `provsql` across the per-row `gate_times`, breaking the read-once
  invariant.  The canonical motivating case is the composition of
  constant-selection elimination with a PK on the same relation :
  a constant pinning combined with a PK that determines another
  column collapses an atom that neither pass handles in isolation.
  The nested rewrite would emit function-layer atoms as independent
  `gate_plus` subqueries Cartesian-joined with the free-layer at the
  top, factoring each function-layer token out as a single OR'd
  input.  *Deferred per the cost/payoff ordering : the
  triangle-with-two-PKs pattern is real but not common, the
  implementation cost is meaningful (250+ lines of rewriter surgery,
  careful interaction-testing with multi-component and inner-group
  paths), and the existing FD closure already lands the most common
  shapes through the constant-selection pass's multi-component
  routing for pinned atoms.  Revisit only after a real workload
  surfaces a triangle-with-two-PKs pattern that the existing rewrite
  cannot handle.*

- **Self-joins without PK or constant rescue.**  Symmetric closure
  `R(x, y), R(y, x)`, path-of-length-2 `R(x, y), R(y, z)`, and similar
  shapes remain hard.  Neither the PK-unification pass nor the
  disjoint-constant certification fires : there is no key whose
  columns are equated pairwise, and the constant predicates either
  don't exist or aren't pairwise disjoint.  Resolving the read-once
  question for these shapes requires either Monet-style intensional
  dDNNF construction (see the Möbius / Monet entry above) or proper
  handling of the full Dalvi & Suciu 2012 JACM dichotomy for UCQs
  (which subsumes self-joins implicitly via the lattice-of-valuations
  criterion).  *Deferred : both directions are larger projects than
  the FD-aware extensions combined, and the workloads where the
  symmetric-closure / path shapes show up tend to be recursive-query
  territory that ProvSQL's CQ-only rewriter already excludes.*

- **Soft keys.**  Functional dependencies that hold probabilistically
  rather than absolutely, per Jha, Rastogi & Suciu (PODS 2008).  Separate
  axis from schema FDs : the rewrite would have to weight each FD's
  consequent by its observed reliability rather than assume it holds in
  every world.  *Deferred ; revisit if a real workload makes the case.*

- **FD chases through views and CTAS-derived relations.**  The PK-FD
  pass is currently restricted to FROM lists of base relations.
  Extending it through views and `CREATE TABLE AS` -derived tables
  was blocked on the TID/BID-propagation work that landed in the
  `safe_queries` branch (view descent in the safe-query rewriter,
  CTAS lineage hook, ancestry-based disjointness gate,
  independent-TID join inference, BID block-key preservation under
  projection / GROUP BY).  The propagation prerequisites are now in
  place ; the remaining work is to teach the PK-FD pass itself to
  follow the lineage rather than stopping at view / CTAS-derived
  RTEs.  *Still deferred ; the CTAS-correlation trap on
  deterministic atoms (`CREATE TABLE foo AS SELECT * FROM
  <tracked>` without `add_provenance`) is the most visible
  symptom, but in practice it's covered by the CTAS hook seeding
  ancestry on lineage-bearing CTAS now.*

- **Data-safe plans.**  FDs that hold on the *instance* but not in the
  schema, per Jha, Olteanu & Suciu (EDBT 2010).  Larger project ; the
  rewrite would need to certify the FD against the actual data before
  trusting it.  Separate from the schema-FD-aware analysis the FD
  closure currently implements.  *Deferred ; revisit only with a
  clearly-motivated use case.*

## TID / BID propagation follow-ups

The TID / BID propagation roadmap shipped in full on the
`safe_queries` branch with one exception, deliberately deferred for
the tradeoff described below.

- **`UNION ALL` of compatible BID legs (was "Slice E").**  The
  classifier reports OPAQUE on a `UNION ALL` whose legs are BID
  under the same block-key column at the same target-list position,
  and the CTAS lineage hook propagates that as no metadata recorded
  for the derived relation.  This is correct (no false TID / BID
  classifications), just a missed optimisation.

  *Why not implemented.* A row from leg A and a row from leg B with
  the same block-key value are independent (different source
  relations), not mutually exclusive, so the union is not BID under
  the natural key.  Recovering BID-ness would require synthesising
  a composite block-key column `(leg_id, k)` and surfacing it on
  the derived relation -- which changes the user-visible schema in
  a non-obvious way (the user wrote `CREATE TABLE t AS SELECT k
  FROM bid_a UNION ALL SELECT k FROM bid_b` and would end up with a
  table whose schema has a column they didn't ask for).  The
  tradeoff between automatic BID coverage and surprising-column
  injection didn't favour the synthesis path.

  *Two future paths.*

  1. **Disjoint-range certification** : when the legs' block-key
     value ranges are provably disjoint (e.g., a leg with
     `WHERE k < 100` UNION ALL with another with `WHERE k >= 100`),
     the natural `k` column suffices as the block key and no
     synthesis is needed.  The CTAS hook could be taught to
     recognise such patterns, possibly via the same
     `safe_is_var_const_equality`-style detector the
     disjoint-constant self-join pass in :cfile:`src/safe_query.c`
     uses.  ~100-150 LOC, no schema impact.
  2. **Opt-in synthesis via a GUC** :
     `provsql.synthesize_union_leg_id = off` by default ; `on`
     adds `__provsql_leg_id` to derived tables that would otherwise
     miss the BID promotion.  Documented as an advanced option ;
     only fires when the user requests it.  ~200-300 LOC including
     parse-tree surgery for the new column and the composite
     block-key recording on the worker side.
