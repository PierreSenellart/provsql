# ProvSQL TODO

Planning material for upcoming ProvSQL work, kept alongside the source
tree so the plans evolve with the code that implements them.

Each plan document follows a consistent layout:

1. **Intro** : one paragraph stating the scope of the plan and the
   reference material it is anchored on.
2. **Out of scope** (optional) : items deliberately excluded, with a
   pointer to where they are handled instead.
3. **Plan** : the proposals themselves, each self-contained.
4. **Priorities** : ship-when ordering.
5. **Implementation observations** (optional) : reusable notes from
   prior work in the same area.

## Contents

- [`priorization.md`](priorization.md) : a cross-file prioritisation of
  every open point, each with a concrete example, its **current
  behaviour tested on the installed build** (verbatim output), and an
  after-implementation sketch. Start here for the ship-order across all
  the plans below; the per-plan files keep the full design rationale.
- [`bounded-treewidth-data.md`](bounded-treewidth-data.md) :
  feasibility study for exploiting bounded treewidth of the input data
  (Courcelle's theorem and its provenance refinement, ABS 2015 / 2017).
  Route C has landed end to end: under `provsql.boolean_provenance`,
  the query rewriter recognises recursive reachability (directed,
  undirected, edge-filtered) over a tracked edge relation and compiles
  every reachable vertex's provenance along a tree decomposition of
  the data graph into certified d-DNNF tokens (linear size, cyclic
  graphs native, plan-time fallback to the generic fixpoint), which
  the standard surface evaluates linearly (`independent`,
  `interpret-as-dd`, Shapley).  Open: multi-source base arms,
  bounded-hop patterns, BID blocks, join-defined graphs, the full
  data-decomposition + tree-automaton pipeline (now cheaper: emitted
  gates only need the d-DNNF certificate), and a treewidth-aware
  general m-semiring evaluator.
- [`conditioning.md`](conditioning.md) : the conditioning primitive,
  unifying discrete tuple-correlation (MarkoViews, Jha & Suciu PVLDB
  2012) and continuous random variables as one operation at two carriers.
  Landed end to end: inert scope-local `provenance()`, the `|` / `cond` /
  `given` surface and `gate_conditioned` gate (terminal for `uuid`,
  composable for `rv` / `agg_token`), `probability_evaluate(A|B)`, and
  the "probability calculator" case study 8. Open: arbitrary denial
  constraints (general `¬W`), a re-based materialised discrete posterior
  for re-composition, Shapley over evidence, and soft/weighted
  conditioning (explicitly not a priority).
- [`case-studies.md`](case-studies.md) : plan for closing the
  feature-coverage gaps in the user tutorial and the existing case
  studies by extending CS1-CS5, plus a future UDF / aggregate-join
  study (CS8).
- [`continuous_distributions.md`](continuous_distributions.md) : roadmap
  for extending the continuous random-variable surface beyond the shipped
  Normal/Uniform/Exponential/Erlang/Categorical/Mixture baseline (further
  parametric families, quantiles / function application / order
  statistics, empirical & structural distributions, conditioning, copulas).
- [`probability-evaluation.md`](probability-evaluation.md) : the
  **remaining** probability-method-selection work, atop the now-landed
  method catalog + three-path (exact / relative / additive) chooser:
  the d-tree's remaining pieces (BID / multivalued circuits, non-DNF
  exact auto-selection, memoising the approximate path), the SUM-safe
  rounding FPTRAS, the exact residual HAVING shapes (branch-spanning SUM,
  BID disjoint blocks, UNION/EXCEPT over a shared-tuple join), catalog
  follow-ups (lazy Boolean build, guarantee propagation, independence-cert
  cache), RV-probability transparency, and d-tree research polish. Borders
  [`safe-query-followups.md`](safe-query-followups.md).
- [`safe-query-followups.md`](safe-query-followups.md) : deferred ideas
  bordering the `provsql.boolean_provenance` work -- further Boolean-only
  optimisations (independent-subtree detection, Möbius / Monet…), the
  inversion-free `UCQ(OBDD)` extensions (UNION in a view, FD-aware orders),
  discrete `random_variable` extensions, and the hierarchical-detector
  follow-ups (FD-induced nested rewrite, soft keys, view-descent FD
  chases, data-safe plans).
- [`scalar-subqueries.md`](scalar-subqueries.md) : the remaining
  unsupported scalar-/correlated-subquery forms -- scalar sublinks nested
  in arithmetic (today a passthrough-with-warning; the decorrelation
  follow-up now has its `agg_token`-arithmetic prerequisite in place),
  different-`(Q, corr)` multi-sublinks, and `GROUP BY` bodies.
- [`studio.md`](studio.md) : open ProvSQL Studio work -- the
  Contributions (Shapley / Banzhaf heat-map) and Time-travel / Temporal
  modes, batch result-table evaluation, multi-user demo deployment,
  and Notebook-mode polish (collapse / clear output, run-from-here,
  per-cell row cap).
