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
- [`karp-luby-fpras.md`](karp-luby-fpras.md) : feasibility study for
  adding a Karp-Luby FPRAS for `#DNF` (Karp-Luby 1985; KLM 1989; modern
  hashing- and streaming-DNF variants) as an in-process probability
  method. Closes a real gap -- the existing `'monte-carlo'` needs
  `~ 1/p` samples for rare events and `'weightmc'` requires an external
  SAT-based binary -- on positive UCQ-shaped lineage. Tier 1 covers
  top-OR-over-AND-only sub-circuits (cross-clause leaf sharing fine,
  no internal ORs in a clause; ~300 LOC): shape detector, dispatcher
  branch, default `(eps, delta)` policy, tests, docs. Conditioned on a
  short empirical probe of how often real ProvSQL outputs land in the
  `m * p << 1` regime.
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
- [`playground-warm-backend.md`](playground-warm-backend.md) : design
  for keeping the browser Playground's WASM backend (PGlite + Pyodide)
  alive across mode / database switches, which today each reload the
  whole page and re-initialise everything. Splits the single document
  into a shell page that owns the warm backend and an iframe running the
  unmodified Studio UI, bridging `/api/*` over `postMessage`; a mode
  switch then reloads only the iframe and a DB switch reopens just
  PGlite, keeping Pyodide / Flask warm (and retiring the `/api/conn`
  IndexedDB flush-race workaround). Includes the file-by-file touch
  list, the e2e iframe refactor, risks, and why the shell beats a
  SharedWorker or an SPA-intercept. Composes with the shipped cheap win
  (manifest-signature skip of the bootstrap PGlite open).
