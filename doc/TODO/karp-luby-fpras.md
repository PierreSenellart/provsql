# Karp-Luby FPRAS for DNF provenance

ProvSQL's `'karp-luby'` probability method is an in-process FPRAS for `#DNF`
(the probability of a positive DNF Boolean formula under tuple-independent
inputs). It is the rare-event tool: naive `'monte-carlo'` needs
`Theta(log(1/delta)/(eps^2 * p))` samples for an `(eps, delta)` *relative*
guarantee, inversely proportional to `p`, so it collapses on low-probability
outputs, whereas Karp-Luby's `O(m/eps^2 * log(1/delta))` over `m` clauses is
independent of `p`. The UCQ-over-a-probabilistic-database lineage of a rare
output tuple is exactly the DNF shape it targets.

References: Karp-Luby (J. Complexity 1985); Karp-Luby-Madras "KLM" (J.
Algorithms 1989, the tight bound and the self-adjusting stopping rule);
Bringmann-Friedrich (ICALP 2008) and Meel-Shrotri-Vardi (FSTTCS 2017) for
sharper per-sample costs; the Vinodchandran-Meel streaming-DNF line (2020+).

## Implemented (Tier 1)

- **Shape detector + sampler.** `BooleanCircuit::dnfShape` recognises the two
  tractable regimes -- (a) a single AND-of-leaves clause, and (b) a top-level
  OR whose children are AND-only sub-circuits over `input` leaves (cross-clause
  leaf sharing allowed) -- and extracts the per-clause support sets;
  `BooleanCircuit::karpLuby` is the coverage estimator (draw a clause with
  probability `p_i/S`, force its support true and draw the rest from marginals,
  accept iff it is the first clause the assignment covers, return
  `S * accepts / N`). Both in `src/BooleanCircuit.cpp`; the dispatch and the
  `(eps, delta) -> N` conversion in `src/probability_evaluate.cpp`. Any other
  positive shape (an internal OR, or a non-OR root) and the non-monotone gates
  (`monus`, `gate_cmp`, aggregation, `gate_rv`, multivalued) are **refused with
  a warning**, not silently approximated.
- **Shared argument grammar** (`parse_sample_spec`): `samples=N` |
  `epsilon=E[,delta=D][,max_samples=M]`, default `(eps=0.1, delta=0.05)`, with
  the bare-integer and legacy `delta;epsilon` shorthands kept. The same parser
  now also drives `'monte-carlo'` (an *additive* Hoeffding `(eps, delta)`,
  `N = ceil(ln(2/delta)/(2 eps^2))`) and `'weightmc'` / `'wmc'`
  (`epsilon=` / `delta=` / `tool=`).
- **Guarantee surfacing.** An `approximation-guarantee` NOTICE (gated on
  `provsql.verbose_level >= 5`) that ProvSQL Studio parses into the eval-strip
  bound; karp-luby is in the Studio method picker and the probability
  benchmark.
- **Tests / docs.** `test/sql/karp_luby.sql`,
  `studio/tests/test_evaluate_karp_luby.py`, `studio/tests/e2e/test_smoke.py`;
  `doc/source/user/probabilities.rst`,
  `doc/source/dev/probability-evaluation.rst`.

## Remaining

### Tier 2 -- sampler polish (internal to `BooleanCircuit::karpLuby`, same API)

- **KLM self-adjusting stopping rule** (KLM 1989): replace the fixed-`N` loop
  with sampling until the *accept* count reaches a deterministic threshold,
  then return the unbiased ratio. Same `(eps, delta)` surface; smaller hidden
  constants; fewer wasted samples when `Pr[F]/S` is close to 1.
- **Per-clause stratified sampling**: when the `p_i` differ widely, the
  categorical clause draw's variance is dominated by the largest clauses;
  standard stratification cuts the constant by up to a factor `m`.

### Tier 3 -- positive-DAG generalisation (regime (c), research)

Handle a fully general positive Boolean DAG -- an internal OR below the root,
or a non-OR root -- where `Pr[C_i]` is no longer a free product (it becomes
`#P`-hard), so the cheap Karp-Luby contract breaks. Two routes:

- **Refinement-tree decomposition**: walk the DAG top-down splitting OR nodes,
  recording the disjunct chosen at each OR ancestor; each refinement leaf is a
  conjunction (one DNF clause), enumerated on the fly (no materialised DNF).
  Can blow up exponentially, but mid-sized positive DAGs stay manageable.
- **Self-reducibility + sampling** (Jerrum-Valiant-Vazirani): an FPRAS for
  `#DNF` yields a (weighted) sampler over satisfying assignments, which in turn
  supports recursive estimation of any positive-circuit probability. Heavier,
  but the asymptotically right answer.

## Open questions

- **Which workloads benefit?** No empirical probe was run before shipping. The
  pattern: a UCQ over a probabilistic database with source probabilities skewed
  toward 0; tabulate the output marginals and see what fraction fall in the
  `m * p << 1` regime where Karp-Luby is asymptotically necessary. Candidates:
  TPC-H with derived probabilities, the shipped case studies, MayBMS-style
  corpora.
- **Interaction with the safe-query rewriter** (`provsql.boolean_provenance`):
  does `foldBooleanIdentities` / the hierarchical rewriter ever turn a Tier-1
  shape into one `dnfShape` rejects, or produce circuits where Karp-Luby beats
  the post-rewrite `independent` / `inversion-free` exact path?
- **Correlated inputs**: `set_prob` is leaf-level today, so the
  sample-leaves-independently step is exact. Once correlated inputs land (see
  [`conditioning.md`](conditioning.md), MarkoViews) the conditional sampler
  must come from the correlation structure; the unbiasedness proof still holds,
  but the per-sample cost depends on that structure.
- **HAVING**: the HAVING route (`enumerate_valid_worlds`, see
  [`safe-query-followups.md`](safe-query-followups.md)) emits a DNF, so a
  Karp-Luby branch on it is natural -- though for MIN / MAX (which emit `2^N`
  clauses) the `m = 2^N` factor almost certainly loses to the closed forms
  proposed there.

## Settled (decided against, kept for rationale)

*(none yet)*
