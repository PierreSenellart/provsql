# Safe-Query Follow-Ups

Open items bordering the safe-query / `provsql.boolean_provenance`
machinery, deferred but worth coming back to. The shipped machinery is
described in the *Safe-Query Rewriter* section of
`doc/source/dev/query-rewriting.rst`.

This file is organised by **priority**: Tier 2 (reusable prerequisites
and certificates) and Tier 3 (workload-gated).

## Tier 2 – reusable prerequisites and certificates

- **Inversion-free `UCQ(OBDD)` extensions.**  The implemented
  inversion-free path (the `UCQ(OBDD)` rung of Jha & Suciu, ICDT 2011;
  see the *Inversion-Free UCQ(OBDD) Path* section of
  `doc/source/dev/probability-evaluation.rst`) admits two extensions,
  in increasing difficulty:
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

- **Discrete `random_variable` sum machinery.**  The discrete families
  (Poisson, Binomial, Geometric, Negative binomial) exist as
  self-registering `Distribution` subclasses under `src/distributions/`,
  and the family-agnostic analytic evaluator consumes their CDFs; what
  is missing is the *sum* structure that would give HAVING over an
  uncertain aggregated value closed forms:
  - **Sum-closure rules for discrete families** (sum of independent
    Poissons is Poisson with summed rate; fixed-`p` Binomial sums
    close), landing in the existing sum-closure registry -- no discrete
    family registers a `ClosureRule` today.  (The same registry item as
    [`continuous_distributions.md`](continuous_distributions.md) §A.1.)
  - **General convolution of independent discrete RVs**: an O(C × N) DP
    for the surrounding cmp; the sum closure currently bails on
    categorical / mixture children (`src/HybridEvaluator.cpp`).

- **Möbius "Increment 3": ranking / shattering** (research-flavoured,
  may never be needed).  The extensional Möbius-inversion route to safe
  non-hierarchical UCQs (`src/mobius_evaluate.cpp`, the `gate_mobius`
  signed top combination, the `mobius` method, `provsql.mobius` GUC)
  soundly declines queries whose reduced form still carries a
  within-disjunct self-join; handling them needs ranking / shattering
  in the compiler.
  *Rejected alternative, recorded so it is not re-attempted:* the
  **intensional** Monet 2019 construction (arXiv:1912.11864, a PTIME
  deterministic-decomposable circuit using negation in place of
  inclusion-exclusion, which would drop into the existing pipeline
  since `gate_monus` already means `AND(a, NOT(b))` under Boolean)
  stays deferred on cost-benefit grounds: the polynomial degree depends
  on query structure, the emitted circuit may be much larger than the
  natural lineage, the safety detector is meaningfully more expensive
  than the hierarchical one, the paper covers only the inversion-free
  fragment, and no implementation has appeared since the paper -- for
  any safe non-hierarchical workload encountered today, handing the
  natural lineage to d4 is likely as fast or faster.  Revisit only if a
  benchmark surfaces where d4 visibly chokes on a safe non-hierarchical
  UCQ and Monet's construction plausibly helps.

### Hierarchical-detector follow-ups (rewriter coverage)

The FD-aware safe-query rewriter's implemented extensions are described
in `src/safe_query.c` and the dev-doc *Safe-Query Rewriter* section.
The items below were surfaced during that work and deliberately
deferred for a future slice.

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
  dDNNF construction (see the rejected-alternative record in the
  *Möbius* entry above) or proper
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
  pass is restricted to FROM lists of base relations.  The
  TID/BID-propagation prerequisites (view descent in the safe-query
  rewriter, CTAS lineage hook, ancestry-based disjointness gate,
  independent-TID join inference, BID block-key preservation under
  projection / GROUP BY) are in place; the remaining work is to teach
  the PK-FD pass itself to follow the lineage rather than stopping at
  view / CTAS-derived RTEs.  *Deferred; the CTAS-correlation trap on
  deterministic atoms (`CREATE TABLE foo AS SELECT * FROM <tracked>`
  without `add_provenance`) is the most visible symptom, but in
  practice it is covered by the CTAS hook seeding ancestry on
  lineage-bearing CTAS.*

- **Data-safe plans.**  FDs that hold on the *instance* but not in the
  schema, per Jha, Olteanu & Suciu (EDBT 2010).  Larger project; the
  rewrite would need to certify the FD against the actual data before
  trusting it.  Separate from the schema-FD-aware analysis the FD
  closure currently implements.  *Deferred; revisit only with a
  clearly-motivated use case.*

### TID / BID propagation follow-ups

One item from the TID / BID propagation roadmap remains, deliberately
deferred for the tradeoff described below.

- **`UNION ALL` of compatible BID legs.**  The
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
     disjoint-constant self-join pass in `src/safe_query.c`
     uses.  ~100-150 LOC, no schema impact.
  2. **Opt-in synthesis via a GUC**:
     `provsql.synthesize_union_leg_id = off` by default; `on`
     adds `__provsql_leg_id` to derived tables that would otherwise
     miss the BID promotion.  Documented as an advanced option;
     only fires when the user requests it.  ~200-300 LOC including
     parse-tree surgery for the new column and the composite
     block-key recording on the worker side.

### Joint-width hardening (deferred)

Two hardening items deliberately left out of the joint-width UCQ
compiler's first version:

- **Binary-wire accumulation instead of ternary gate cliques.** The
  current encoding emits ternary gate cliques along the elimination
  order; a binary-wire accumulation would shrink the circuit on
  high-degree states.  Accepted as-is for v1; a possible future
  optimisation, not a correctness gap.
- **Slice-size memory-abort GUC.** Only `provsql.joint_max_treewidth`
  and `provsql.joint_max_states` bound the run today; a dedicated
  slice-size budget that aborts gracefully on a memory blow-up (rather
  than relying on the state cap) is unimplemented.
