# TODO

Ideas surfaced during the safe-query / `provsql.boolean_provenance`
discussion that are deferred but worth coming back to.

## Boolean-only optimisations

Currently implemented: hierarchical-CQ safe-query rewriter
(`src/safe_query.c`), Boolean-folded gate marker (`gate_assumed_boolean`).
Next up: `foldBooleanIdentities` (idempotence, absorption,
plus-with-one absorber) as a new phase of `foldSemiringIdentities`.

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

## TID / BID propagation through derived relations

The per-relation `provsql_table_info` store today only records the
kind a user set at `add_provenance` / `repair_key` time.  Derived
relations (CTAS, views) start at the default TID classification ;
that's right for many simple cases but wrong in general.

- **`propagate_provenance(T regclass, source regclass)` helper.**
  After a CTAS that's syntactically TID-preserving (projection +
  filter only, or single-FROM with GROUP BY) the user calls this to
  copy `source`'s kind into `T`'s entry.  First slice : do not try
  to auto-detect ; require the user to assert by calling.
- **Multi-source CTAS classifier.**
  Run a subset of `safe_query.c`'s detector on the source query to
  decide TID / BID / OPAQUE automatically.
- **View descent in the safe-query detector.**
  Today the detector treats `RTE_VIEW` / `RTE_SUBQUERY` as opaque.
  Recursing into the view body and re-classifying inline would let
  the safe-query rewriter fire on queries that join through simple
  views.

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
  thresholds.  When children are i.i.d. Bernoulli leaves and the
  predicate is `COUNT(*) op c`, the answer is a binomial CDF in
  O(N).  When children are independent but not i.i.d., a
  Poisson-binomial CDF DP gives O(N^2).  Both bypass the DNF
  construction entirely ; slot into `probability_evaluate.cpp` as a
  pre-pass next to `runAnalyticEvaluator`, not into
  `pw_from_cmp_gate` (the closed-form value is a probability, not
  a polynomial, so it produces a Bernoulli `gate_input` leaf and
  is sound only for the probability path).
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
