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
  With Route C (decomposition-aligned reachability compilation) shipped
  and the mulinput-OR deterministic mark now backed by a Lean soundness
  proof, the open work is: Route 3 structural factoring, the Route C
  leftovers (shared-support join-defined edges, non-recursive triggers,
  any-reach collector chains, k-terminal side filters), a treewidth-aware
  general m-semiring evaluator, and the full Route A MSO / tree-automaton
  pipeline.
- [`conditioning.md`](conditioning.md) : the conditioning primitive,
  unifying discrete tuple-correlation (MarkoViews, Jha & Suciu PVLDB
  2012) and continuous random variables as one operation at two carriers.
  With the core surface (`|` / `cond` / `given`, `gate_conditioned`,
  `probability_evaluate(A|B)`, the "probability calculator" case study 8)
  already shipped, the open work is: arbitrary denial constraints
  (general `¬W`), a re-based materialised discrete posterior for
  re-composition, Shapley over evidence, and soft/weighted conditioning
  (explicitly not a priority).
- [`case-studies.md`](case-studies.md) : plan for closing the
  remaining feature-coverage gaps in the user tutorial and case
  studies -- the CS4 temporal / data-modification extensions and a
  future UDF / aggregate-join study (CS9).
- [`case_aggregation.md`](case_aggregation.md) : feasibility study for a
  searched `CASE` guarded by aggregate comparisons (`CASE WHEN SUM(x) > 3
  THEN SUM(y) ELSE SUM(z) END`), lowering the `agg_token`-typed `CASE` to
  the existing carrier-agnostic `gate_case`. Exact evaluation reuses the
  structure-aware HAVING possible-worlds evaluator (`provsql_having`):
  correlation between guard and arm is free by co-enumeration, and the
  PICKFIRST / monotone closed forms carry into value-carrying selection,
  with `2^n` co-enumeration and Monte Carlo as backstops.
- [`continuous_distributions.md`](continuous_distributions.md) : roadmap
  of the still-open extensions to the continuous random-variable surface
  (native analytic discrete families, multivariate Normal, CDF / monotone
  transforms, frozen snapshots, copulas, stochastic processes,
  do-calculus, and the provenance × probability research directions).
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
  optimisations (independent-subtree detection, the deferred intensional
  Monet construction now that the extensional Möbius route has shipped),
  the remaining inversion-free `UCQ(OBDD)` extensions (FD-aware /
  per-branch FBDD orders; `UNION` support has shipped), discrete
  `random_variable` extensions, the hierarchical-detector
  follow-ups (FD-induced nested rewrite, soft keys, view-descent FD
  chases, data-safe plans), and the two deferred joint-width hardening
  notes.
- [`scalar-subqueries.md`](scalar-subqueries.md) : the remaining
  unsupported scalar-/correlated-subquery forms -- scalar sublinks nested
  in arithmetic (today a passthrough-with-warning; the decorrelation
  follow-up now has its `agg_token`-arithmetic prerequisite in place),
  different-`(Q, corr)` multi-sublinks, and `GROUP BY` bodies.
- [`studio.md`](studio.md) : open ProvSQL Studio work -- the
  "undo last DML" button, batch result-table evaluation, multi-user
  demo deployment, and Notebook-mode polish (collapse / clear output,
  run-from-here, per-cell row cap).
