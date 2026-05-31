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

- [`bounded-treewidth-data.md`](bounded-treewidth-data.md) :
  feasibility study for exploiting bounded treewidth of the input data
  (Courcelle's theorem and its provenance refinement, ABS 2015 / 2017),
  with empirical probes. Findings: it is not a free lunch (a fixed
  two-atom self-join already inflates circuit treewidth to `Theta(|I|)`
  on treewidth-1 data), but the explosions are confined to high-degree
  self-joins / cross-products, while local UCQs stay bounded and
  recursive reachability on grids grows past the cap. d4 overtakes the
  in-process tree-decomposition compiler at treewidth ~8-9. Concludes:
  treewidth-conditioned dispatch (not a higher cap), extend
  independent-product factoring for the relational pathologies, and a
  decomposition-aligned (cycluit) construction for the recursive
  fragment.
- [`conditioning.md`](conditioning.md) : plan for a conditioning
  primitive, unifying discrete tuple-correlation (MarkoViews, Jha &
  Suciu PVLDB 2012) and continuous random variables as one operation at
  two carriers. Extracts the conditioning-as-a-gate design from
  `continuous_distributions.md` §D.1, folds in what MarkoViews teaches
  (conditioning on a constraint circuit; the exact `P(Q ∧ C)/P(C)`
  formula needing no discrete gate; exact-only under negative weights;
  safety re-checked on the conditioned circuit), adds soft/weighted
  conditioning, and grounds it all in concrete use cases.
- [`case-studies.md`](case-studies.md) : plan for closing the
  feature-coverage gaps in the user tutorial and the five existing
  case studies (CS1-CS5), plus a sketch of CS6 for upcoming features.
- [`continuous_distributions.md`](continuous_distributions.md) : plan
  for adding continuous probability distributions (Gaussian, uniform,
  exponential, ...) to ProvSQL's pc-table model, anchored on Timothy
  Leong's 2022 BSc thesis (NUS).
- [`having-trichotomy.md`](having-trichotomy.md) : assessment of what
  ProvSQL gains from Ré & Suciu's HAVING-trichotomy paper (VLDB J.
  2009 / DBPL 2007). Finds the paper's framework (semiring annotation
  + recovery function + safe plan) maps almost one-to-one onto
  ProvSQL's `gate_cmp(gate_agg(gate_semimod …), gate_value)`, and that
  its **marginal-vector + monoid-convolution** algorithm is the PTIME
  replacement for the exponential possible-worlds enumeration in
  `provsql_having`. Four gains: exact PTIME evaluators generalising
  `CountCmpEvaluator` to MIN/MAX/SUM/COUNT(DISTINCT); a safe /
  apx-safe / hazardous classifier (à la `classify_top_level`) to drive
  method selection; principled FPRAS routing for apx-safe queries onto
  the new karp-luby method; and independence certification via the
  safe-query rewriter to extend the closed forms to joins. Grounds the
  Tier-1 HAVING items in [`safe-query-followups.md`](safe-query-followups.md).
- [`safe-query-followups.md`](safe-query-followups.md) : deferred
  ideas surfaced during the `provsql.boolean_provenance` work --
  further Boolean-only optimisations beyond the hierarchical-CQ
  rewriter and `foldBooleanIdentities` (independent-subtree
  detection, Möbius / Monet, ...), the layered HAVING-clause
  optimisation plan, and the hierarchical-detector follow-ups
  (FD-induced nested rewrite, soft keys, view-descent FD chases,
  data-safe plans).
- [`studio.md`](studio.md) : plan for ProvSQL Studio work landing
  alongside or after the first PyPI release (`studio-v1.0.0`):
  release plumbing, CI, Docker swap-over, in-app polish, and the
  Contributions / Time-travel modes scheduled for later versions.
