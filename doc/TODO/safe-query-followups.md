# Safe-Query Follow-Ups

Open items surfaced during the safe-query / `provsql.boolean_provenance`
discussion that are deferred but worth coming back to. The work that has
already landed is described in the *Safe-Query Rewriter* section of
`doc/source/dev/query-rewriting.rst`.

This file is organised by **priority**: Tier 2 (reusable prerequisites
and certificates) and Tier 3 (workload-gated). The HAVING-clause
optimisation layers (Tier 1) and the base FD-aware rewriter extensions
have all landed.

## Tier 2 – reusable prerequisites and certificates

- **Inversion-free `UCQ(OBDD)` extensions.**  The inversion-free path is
  implemented (the `UCQ(OBDD)` rung of Jha & Suciu, ICDT 2011: a detector
  plus an in-process structured-d-DNNF builder over the Prop. 4.5
  query-derived order, with non-integer key columns, deterministic-relation
  filters, and SPJ / nested view flattening); see the *Inversion-Free
  UCQ(OBDD) Path* section of
  :cfile:`doc/source/dev/probability-evaluation.rst`.  Three extensions to
  that class remain open, in increasing difficulty:
  - **UNION nested in an inlined subquery / view** (the inlined body is a
    `setOperations` UCQ): flatten per branch with the order markers fanned
    across branches.  This is not really a *view* quirk -- a **top-level**
    `UNION` is already handled, because the planner's recursive
    `process_query` splits it branch-by-branch and each branch reaches the
    safe-query gate (`is_safe_query_candidate`, which bails on
    `setOperations`) on its own and is certified independently.  The gap is
    only the *nested* case: a `UNION` inlined from a view / derived table
    lands as a `setOperations` node *inside one* safe-query candidate's
    scope, with no re-entry to split it, so the single global Prop. 4.5
    order cannot span the branches.  The fix gives the nested case the
    per-branch treatment the top-level case gets for free; the class is
    defined for UCQs, so this is a flattening extension, not a core change,
    and it declines gracefully today.  *Sensible next slice.*
  - **Functional dependencies do not fit this builder.**  The canonical
    FD-tractable queries (the H-query `R(x),S(x,y),T(y)` under a PK on `S`,
    the two-PK triangle) have no single root class, which the detector and
    the single global Prop. 4.5 order both require; the read-once path
    handles them via per-atom anchors precisely because no single root
    exists, and they are anyway safe-but-not-poly-OBDD (read-once
    territory).  *Out of reach for a surface tweak to the precedence and
    positional checks; needs the per-branch builder below.*
  - **Per-branch decision orders (FBDD / Decision-DNNF).**  The builder is
    OBDD-class: one global order.  The `h_k` queries (Beame, Li, Roy &
    Suciu) have inversions, so no poly OBDD, yet admit poly FBDD / d-DNNF
    with different orders on different branches.  Choosing the decision
    variable per branch would capture strictly more, and subsume the FD
    case above.  *Research extension, not a surface one.*

## Tier 3 – workload-gated

These widen coverage or reach harder query classes, but each is
explicitly deferred until a real workload motivates it.

- **Discrete `random_variable` extensions.**  `random_variable` today
  carries continuous distributions (Normal, Uniform, Exponential, Erlang)
  plus Categorical and Mixture, and the HAVING evaluators assume the `m_i`
  extracted from `gate_semimod` is a deterministic integer.  Extending
  `gate_rv` (and the surrounding analytic evaluators) to discrete
  distributions (Poisson, Binomial, Geometric, Multinomial) opens a
  parallel optimisation track for HAVING when the *aggregated value* itself
  is uncertain:
  - Sum of independent Poissons is Poisson with summed rate; closed-form
    CDF on the surrounding `gate_cmp`.  Same shape as the existing
    Normal-sum closed form in `Expectation.cpp`.
  - COUNT(*) over Bernoulli presence-tokens is Poisson-binomial; same
    machinery as the landed `CountCmpEvaluator`, now generic to "the count
    of present rows is itself a random variable" rather than
    Boolean-specific.
  - General convolution of independent discrete RVs: O(C × N) DP for the
    surrounding cmp.

  **Integration point.**  An analogue of `runAnalyticEvaluator`
  (`src/AnalyticEvaluator.{h,cpp}`) that walks `gate_cmp(gate_agg(
  gate_arith over gate_rv children), gate_value)`, computes the analytical
  CDF, and replaces the cmp with a Bernoulli `gate_input` leaf.
  Probability-specific (the `gate_input` carries a numeric probability,
  semiringly meaningless to symbolic semirings); lives in
  `probability_evaluate.cpp` before `getBooleanCircuit`, the same slot as
  the existing analytic / hybrid passes.

  **Architectural fit.**  The extension stays inside the existing
  intensional pipeline: new distribution variants in the `gate_rv` `extra`
  blob, new closed-form CDF entries in the analytic evaluator, new
  structural reductions in the HAVING pre-pass.  No new gate type; no change
  to the circuit framework itself.  The MC fallback in `MonteCarloSampler`
  already handles cases where the closed form does not apply.

- **Möbius / inclusion-exclusion via Monet 2019's construction.**
  Beyond hierarchical CQs, the Dalvi-Suciu dichotomy puts many
  non-hierarchical UCQs in PTIME, with the canonical algorithm
  being lattice-walking inclusion-exclusion which is naturally
  extensional.  Monet (arXiv:1912.11864) closes the architectural
  gap by giving a PTIME construction of a deterministic-decomposable
  circuit, using negation in place of IE; the output drops
  straight into our intensional pipeline (`gate_monus` already
  means `AND(a, NOT(b))` under Boolean, so no new gate type
  required).
  *Deferred; the architectural mismatch is no longer the blocker
  but practicality is open.  The construction is PTIME but the
  polynomial degree depends on query structure, not linear in
  database size; the emitted circuit may be much larger than the
  natural lineage; the safety detector is meaningfully more
  expensive than the hierarchical one; the paper covers a strict
  subset of safe non-hierarchical UCQs (the inversion-free
  fragment); and no implementation has appeared in the six years
  since the paper, so there is no empirical evidence for the
  practical regime.  The honest cost-benefit is that for any safe
  non-hierarchical workload we encounter today, handing the
  natural lineage to d4 is likely as fast or faster than
  constructing-then-evaluating Monet's circuit.  Revisit only if
  a benchmark surfaces where d4 visibly chokes on a safe
  non-hierarchical UCQ and Monet's construction plausibly helps.*
  Superseded as the route to safe non-hierarchical UCQs by
  [`mobius.md`](mobius.md), which plans the **extensional**
  lattice-walking algorithm (Möbius certificate at plan time,
  compile-at-execution circuit with a signed top combination);
  the intensional construction above stays deferred on the same
  grounds.

### Hierarchical-detector follow-ups (rewriter coverage)

The FD-aware safe-query rewriter currently lands six extensions on top
of the base hierarchical-CQ detector: constant-selection elimination,
PK / NOT-NULL UNIQUE FDs from the catalog, deterministic-relation
transparency, PK-unifiable self-joins, FD closure on the union-find
(detector-only), and disjoint-constant self-joins.  See
:cfile:`src/safe_query.c` and the dev-doc *Safe-Query Rewriter* section
for the implementation.  The items below were surfaced during that work
and deliberately deferred for a future slice.

- **FD-induced nested rewrite (function/free split).**  The current
  FD closure accepts every query whose FD-reduced atom-sets are
  pairwise nested-or-disjoint *and* whose existing single-level wrap
  is already read-once.  When the FD closure splits the atoms into a
  *function layer* (PK-determined relations) and a *free layer* (no
  FD on them) and the function layer indexes into the free layer,
  the single-level wrap duplicates each function-layer atom's
  `provsql` across the per-row `gate_times`, breaking the read-once
  invariant.  The canonical motivating case is the composition of
  constant-selection elimination with a PK on the same relation:
  a constant pinning combined with a PK that determines another
  column collapses an atom that neither pass handles in isolation.
  The nested rewrite would emit function-layer atoms as independent
  `gate_plus` subqueries Cartesian-joined with the free-layer at the
  top, factoring each function-layer token out as a single OR'd
  input.  *Deferred per the cost/payoff ordering: the
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
  disjoint-constant certification fires: there is no key whose
  columns are equated pairwise, and the constant predicates either
  don't exist or aren't pairwise disjoint.  (These shapes all carry an
  inversion, hence are not even in `UCQ(OBDD)`; the inversion-free
  self-joins (consistent unification, e.g.
  `S(x,y),A(x,y),S(x,z),B(x,z)`) are the easier subset already handled by
  the implemented inversion-free path, see *Inversion-free `UCQ(OBDD)`
  extensions* in Tier 2.)  Resolving the read-once
  question for these shapes requires either Monet-style intensional
  dDNNF construction (see the *Möbius / Monet* entry above) or proper
  handling of the full Dalvi & Suciu 2012 JACM dichotomy for UCQs
  (which subsumes self-joins implicitly via the lattice-of-valuations
  criterion).  *Deferred: both directions are larger projects than
  the FD-aware extensions combined, and the workloads where the
  symmetric-closure / path shapes show up tend to be recursive-query
  territory that ProvSQL's CQ-only rewriter already excludes.*

- **Soft keys.**  Functional dependencies that hold probabilistically
  rather than absolutely, per Jha, Rastogi & Suciu (PODS 2008).  Separate
  axis from schema FDs: the rewrite would have to weight each FD's
  consequent by its observed reliability rather than assume it holds in
  every world.  *Deferred; revisit if a real workload makes the case.*

- **FD chases through views and CTAS-derived relations.**  The PK-FD
  pass is currently restricted to FROM lists of base relations.
  Extending it through views and `CREATE TABLE AS` -derived tables
  was blocked on the TID/BID-propagation work that landed in the
  `safe_queries` branch (view descent in the safe-query rewriter,
  CTAS lineage hook, ancestry-based disjointness gate,
  independent-TID join inference, BID block-key preservation under
  projection / GROUP BY).  The propagation prerequisites are now in
  place; the remaining work is to teach the PK-FD pass itself to
  follow the lineage rather than stopping at view / CTAS-derived
  RTEs.  *Still deferred; the CTAS-correlation trap on
  deterministic atoms (`CREATE TABLE foo AS SELECT * FROM
  <tracked>` without `add_provenance`) is the most visible
  symptom, but in practice it's covered by the CTAS hook seeding
  ancestry on lineage-bearing CTAS now.*

- **Data-safe plans.**  FDs that hold on the *instance* but not in the
  schema, per Jha, Olteanu & Suciu (EDBT 2010).  Larger project; the
  rewrite would need to certify the FD against the actual data before
  trusting it.  Separate from the schema-FD-aware analysis the FD
  closure currently implements.  *Deferred; revisit only with a
  clearly-motivated use case.*

### TID / BID propagation follow-ups

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

  1. **Disjoint-range certification**: when the legs' block-key
     value ranges are provably disjoint (e.g., a leg with
     `WHERE k < 100` UNION ALL with another with `WHERE k >= 100`),
     the natural `k` column suffices as the block key and no
     synthesis is needed.  The CTAS hook could be taught to
     recognise such patterns, possibly via the same
     `safe_is_var_const_equality`-style detector the
     disjoint-constant self-join pass in :cfile:`src/safe_query.c`
     uses.  ~100-150 LOC, no schema impact.
  2. **Opt-in synthesis via a GUC**:
     `provsql.synthesize_union_leg_id = off` by default; `on`
     adds `__provsql_leg_id` to derived tables that would otherwise
     miss the BID promotion.  Documented as an advanced option;
     only fires when the user requests it.  ~200-300 LOC including
     parse-tree surgery for the new column and the composite
     block-key recording on the worker side.
