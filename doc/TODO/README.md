# ProvSQL TODO

Planning material for upcoming ProvSQL work, kept alongside the source
tree so the plans evolve with the code that implements them. Only open
work lives here: shipped work is recorded in the git history, the
CHANGELOG, and the user / developer manuals, and is pruned from these
files as it lands.

Each plan document follows a consistent layout:

1. **Intro** : one paragraph stating the scope of the plan and the
   reference material it is anchored on.
2. **Out of scope** (optional) : items deliberately excluded, with a
   pointer to where they are handled instead.
3. **Plan** : the proposals themselves, each self-contained.
4. **Priorities** : ship-when ordering.
5. **Implementation observations** (optional) : reusable notes from
   prior work in the same area, including rejected alternatives kept so
   they are not re-attempted.

## Contents

- [`bounded-treewidth-data.md`](bounded-treewidth-data.md) :
  feasibility study for exploiting bounded treewidth of the input data
  (Courcelle's theorem and its provenance refinement, ABS 2015 / 2017).
  Open work: Route 3 structural factoring, the Route C leftovers
  (shared-support join-defined edges, non-recursive triggers, any-reach
  collector chains, k-terminal side filters), a treewidth-aware general
  m-semiring evaluator, and the full Route A MSO / tree-automaton
  pipeline.
- [`conditioning.md`](conditioning.md) : open work around the
  conditioning primitive -- arbitrary denial constraints (re-running the
  safety analysis on the constraint-augmented circuit), a re-based
  materialised discrete posterior for re-composition, Shapley over a
  Boolean-conditioned root, and soft/weighted conditioning of Boolean
  evidence (explicitly not a priority).
- [`case-studies.md`](case-studies.md) : plan for closing the
  remaining feature-coverage gaps in the user tutorial and case
  studies -- the CS4 temporal / data-modification extensions and a
  future UDF / aggregate-join study (CS9).
- [`continuous_distributions.md`](continuous_distributions.md) : roadmap
  of the still-open extensions to the continuous random-variable surface
  (finishing the native discrete families, multivariate Normal, CDF /
  monotone transforms, frozen snapshots, copulas, stochastic processes,
  do-calculus, and the provenance × probability research directions).
- [`latent-variables.md`](latent-variables.md) : remaining
  latent-variable inference scale-up -- broader recognition of
  exact-inference structure (several shared latents, SUM-conditioned
  collapsed posteriors, latent hierarchies), the hypergeometric ABI
  widening, and the deferred SMC-then-MCMC ladder.
- [`conjugate-posteriors.md`](conjugate-posteriors.md) : optional
  follow-ups to the exact conjugate-posterior recogniser -- cancelling
  independent evidence factors, affine slot matching (1-D Bayesian
  linear regression), posterior predictives as distributions, Normal
  σ-slot conjugacy, and a Studio introspection surface.
- [`feature-gap-analysis.md`](feature-gap-analysis.md) :
  literature-driven gap analysis of ProvSQL against probabilistic- and
  provenance-database systems and theory, with a P1-P4 tiered roadmap
  (WSMS responsibility measures + approximate Shapley first; why-not
  provenance, open-world semantics, minimal factorisation, and further
  demand-gated items behind). Overlaps with sibling plans are
  cross-referenced, not re-argued.
- [`probability-evaluation.md`](probability-evaluation.md) : the
  remaining probability-method-selection work atop the method catalog +
  three-path (exact / relative / additive) chooser: the SUM-safe
  rounding FPTRAS, the exact residual HAVING shapes (coupled
  branch-spanning SUM, shared-tuple UNION/EXCEPT), catalog follow-ups
  (lazy Boolean build, guarantee propagation, independence-cert cache),
  RV-probability transparency, and d-tree research polish. Borders
  [`safe-query-followups.md`](safe-query-followups.md).
- [`safe-query-followups.md`](safe-query-followups.md) : deferred ideas
  bordering the `provsql.boolean_provenance` work -- the inversion-free
  `UCQ(OBDD)` extensions (FD-aware / per-branch FBDD orders), discrete
  `random_variable` sum machinery, the Möbius ranking / shattering
  increment, the hierarchical-detector follow-ups (FD-induced nested
  rewrite, soft keys, view-descent FD chases, data-safe plans), the
  `UNION ALL`-of-BID-legs classification, and the joint-width hardening
  notes.
- [`scalar-subqueries.md`](scalar-subqueries.md) : the remaining
  unsupported scalar-/correlated-subquery forms -- sublinks nested in
  WHERE expressions or opaque function arguments, different-`(Q, corr)`
  multi-sublinks, and `GROUP BY` bodies.
- [`studio.md`](studio.md) : open ProvSQL Studio work -- the
  "undo last DML" button, batch result-table evaluation, multi-user
  demo deployment, and Notebook-mode polish (collapse / clear output,
  run-from-here, per-cell row cap).
