# Karp-Luby FPRAS for DNF provenance

Feasibility study for adding an FPRAS for `#DNF` (probability of a positive
DNF Boolean formula under independent inputs) to ProvSQL, alongside the
existing exact (`independent`, `inversion-free`, `tree-decomposition`,
external `compilation`) and approximate (`monte-carlo`, `weightmc`)
probability-evaluation methods. Anchored on:

- Karp, Luby, *Monte-Carlo Algorithms for the Planar Multiterminal Network
  Reliability Problem*, J. Complexity 1(1), 1985 (original `#DNF` FPRAS).
- Karp, Luby, Madras, *Monte-Carlo Approximation Algorithms for Enumeration
  Problems*, J. Algorithms 10(3), 1989 (the tight `O(m/eps^2 log 1/delta)`
  sample-complexity analysis, hereafter "KLM").
- Meel, Shrotri, Vardi, *On Hashing-based Approaches to Approximate DNF
  Counting*, FSTTCS 2017 (modern variants).
- Bringmann, Friedrich, *Approximating the Volume of Unions and Intersections
  of High-Dimensional Geometric Objects*, ICALP 2008 (faster per-sample
  costs).
- The existing implementation in `src/BooleanCircuit.cpp:355`
  (`BooleanCircuit::monteCarlo`, naive bit-by-bit independent sampling) and
  the dispatcher in `src/probability_evaluate.cpp`.

## The gap

ProvSQL exposes two approximate probability methods today:

1. `'monte-carlo'` -- naive sampling. For each of `N` samples, every input
   leaf is drawn independently from its marginal and the full circuit is
   evaluated. The estimator has variance `p(1-p)/N` with `p = Pr[circuit]`.
   For an `(epsilon, delta)` guarantee the required sample count is
   `Theta(log(1/delta) / (eps^2 * p))` -- **inversely** proportional to `p`.
   On rare-event outputs (`p << 1`), naive Monte Carlo is asymptotically
   useless: at `p = 10^{-4}` and `eps = 0.1` the user must already pick
   `N >= 10^8`, and `provsql_interrupted` will fire long before.
2. `'weightmc'` -- weighted approximate model counting via XOR-hashing,
   delegated to the external `weightmc` binary (`src/BooleanCircuit.cpp:1158`).
   This *is* an FPRAS, but on CNF, via Tseytin (`src/BooleanCircuit.cpp:423`)
   and an external SAT solver. It pays the full general-`#SAT` cost and
   requires the user to install `weightmc`.

There is currently **no in-process FPRAS** for rare-event probability
estimation. The case is real: an output tuple of a UCQ over a probabilistic
database with low-probability source facts has lineage of size `m` derivations,
each a conjunction over rare leaves; its marginal is small and naive MC
collapses, but the lineage is shape-wise a DNF, exactly the regime Karp-Luby
was designed for.

## Why this matches ProvSQL's circuit shape

ProvSQL's provenance circuits over positive queries (SELECT-FROM-WHERE,
UNION/UNION ALL, no `monus`, no `gate_cmp`, no aggregation) are monotone:
they use only `plus` and `times` gates over `input` leaves. The Boolean
function they compute -- modulo the planner's `times` flattening and `plus`
deduplication -- is a positive Boolean function in `n` propositional variables
(the leaves), and every positive Boolean function admits a DNF (its prime
implicants, or equivalently the conjunctions read off the AND-paths from leaves
to the root through the positive DAG). ProvSQL stores it as a DAG, not as the
flattened DNF -- the DAG is exponentially more compact for queries with much
intermediate sharing -- but Karp-Luby never requires the flat DNF, only the
list of top-level disjuncts together with a way to compute and sample each
disjunct.

Three regimes for ProvSQL, in increasing structural difficulty:

(a) **Top-level OR over AND-of-leaves** -- the literal 2-level DNF. The
provenance of a Boolean / set-semantics UCQ before any further combination
(no GROUP BY, no nested aggregation, no HAVING) takes this shape: the top
`plus` enumerates derivations; each derivation is a `times` of source-tuple
leaves. Karp-Luby applies directly with no preprocessing.

(b) **Top-level OR over AND-only sub-circuits** -- each top-`plus` child is
an AND-DAG (a tree, or DAG with internal sharing) over `input` leaves,
**with no internal OR**. Leaves may be shared *across* clauses (that is
the standard Karp-Luby setting: variables can repeat between clauses);
what (b) adds over (a) is that a clause can be structurally a nested AND
or an AND with internal sub-AND sharing, rather than a flat AND-of-literals.
This is the common case after one level of view inlining or after the
safe-query rewriter has pushed conjunctions inwards. Karp-Luby's contract is
satisfied: each clause `C_i` has a well-defined support set (the leaves it
reaches), `Pr[C_i]` is the product of those leaf marginals (computable in
linear time by a bottom-up pass), and the conditional sampler `· | C_i = 1`
pins the support to true and draws the rest from marginals. The only
genuine extension over (a) is the support-extraction sweep through the
AND-DAG.

(c) **Anything else positive** -- the top gate is not an OR, *or* some
clause contains an internal OR. The function is still a DNF (its prime
implicants), but the disjuncts are not syntactically exposed, and the
cheap version of Karp-Luby no longer applies: an internal OR makes
`Pr[C_i]` itself `#P`-hard, so step (1) of the FPRAS contract is no longer
free. Two routes are possible: (i) restrict to (a)+(b) and *bail* on (c);
(ii) recurse à la Jerrum-Valiant-Vazirani -- replace exact `Pr[C_i]` and
exact conditional samplers with FPRAS estimates and approximate samplers,
with composed error bounds. Tier 1 below restricts to (a)+(b); the
recursive generalisation is a research direction (§Open questions).

**Non-applicable regimes**: anything involving `monus` (negation), `gate_cmp`
(HAVING), `gate_agg` / `gate_semimod` (aggregation), `gate_rv` (continuous),
`gate_update` (data-modification). These break monotonicity or leave the
discrete tuple-independent model. They get a `WARNING` and fall back to
naive MC.

## Karp-Luby in one paragraph

Given a DNF `F = C_1 v ... v C_m` with disjoint sample spaces inside each `C_i`
under tuple-independent inputs:

1. Compute `p_i = Pr[C_i]` (for an AND-of-leaves clause, the product of leaf
   probabilities).
2. Let `S = sum(p_i)`. Note `Pr[F] <= S <= m * Pr[F]` by union bound and
   `Pr[F] >= max(p_i) >= S/m`.
3. For each of `N` rounds: sample a clause index `i` with probability `p_i/S`;
   sample a satisfying assignment of `C_i` (force its support to true; draw
   other leaves from their marginals); find the smallest index `j` such that
   the assignment satisfies `C_j`; **accept** the sample iff `j = i`.
4. Return `S * (#accepts / N)`.

This is unbiased; acceptance probability is `Pr[F] / S in [1/m, 1]`, so a
Chernoff bound gives `(eps, delta)`-approximation at
`N = O(m * log(1/delta) / eps^2)` samples -- *independent of* `Pr[F]`, the
defining property Karp-Luby buys over naive MC.

The KLM (1989) "self-adjusting" variant gives the same asymptotics with smaller
hidden constants. Bringmann-Friedrich (2008) and Meel-Shrotri-Vardi (2017)
sharpen the per-sample cost for structured `C_i`. The Vinodchandran-Meel
streaming-DNF line (2020+) reaches `O(n + m)` per (eps, delta)-guaranteed
estimate for weighted DNF, but at the cost of more involved data structures.

## When Karp-Luby wins, when it does not

Vs. **naive MC** for a fixed `(eps, delta)`:

| Regime                 | Naive MC samples       | Karp-Luby samples | KL wins? |
| ---------------------- | ---------------------- | ----------------- | -------- |
| `p = 1/2`, any `m`     | `O(1/eps^2)`           | `O(m/eps^2)`      | No       |
| `p = 0.05`, `m = 50`   | `~ 20 / eps^2`         | `50 / eps^2`      | Marginal |
| `p = 10^{-3}`, `m = 100` | `1000 / eps^2`       | `100 / eps^2`     | Yes      |
| `p = 10^{-6}`, `m = 100` | `10^6 / eps^2`       | `100 / eps^2`     | Strongly |
| `p ~ 1`, large `m`     | `O(1/eps^2)`           | `O(m/eps^2)`      | No       |

The crossover is at `m * p ~ 1`. The naive-MC factor `1/p` makes it unfit for
low-probability outputs; the Karp-Luby factor `m` makes it unfit for
high-probability outputs with many clauses. Per-sample cost is comparable:
both pay `O(circuit-size)` per sample in the regime ProvSQL cares about (the
naive method evaluates the whole circuit, Karp-Luby evaluates the membership
test which is a sweep through clauses).

Vs. **weightmc**: in-process; no external dependency; substantially faster on
DNF inputs (weightmc compiles to CNF via Tseytin and runs XOR-streamlined
SAT, which is at least linear in the Tseytin variable count but typically far
worse on hard CNF). On `(a)+(b)`-shaped circuits with hundreds of clauses,
Karp-Luby finishes in milliseconds where weightmc takes seconds to minutes.

Vs. **tree-decomposition**: exact, but exponential in treewidth. ProvSQL's
`MAX_TREEWIDTH = 10` already excludes a meaningful fraction of real provenance
DAGs (see [`bounded-treewidth-data.md`](bounded-treewidth-data.md): a fixed
two-atom self-join inflates treewidth to `Theta(|I|)`); Karp-Luby has no
treewidth dependence.

Vs. **external compilation** (d4 / c2d / dsharp / panini): exact, no treewidth
cap, but compile time can be the dominant cost on hard instances, and the
output d-DNNF can be huge. Karp-Luby's runtime is fixed by `m / eps^2` and
agnostic to the *internal* hardness of the DNF; it scales gracefully when the
exact compilers blow up.

Vs. **`inversion-free`** (safe-query path): when the certificate fires, the
safe-query route is exact and polynomial -- always strictly better than
Karp-Luby. Karp-Luby covers the cases the certificate **rejects**, which is
exactly the unsafe / non-hierarchical UCQ fragment.

## Proposed implementation

### Tier 1 -- minimum viable Karp-Luby

A new probability method named `'karp-luby'` (clearer to users than `'klm'` or
`'dnf-fpras'`; aliases at the SQL level fine).

1. **Shape detector**, somewhere alongside `safe_query_cert`. Returns true
   iff either (i) the root is an `AND`-only sub-circuit over `input` leaves,
   or (ii) the root is `OR` and every child is itself an `AND`-only
   sub-circuit over `input` leaves. (Equivalently: there is at most one
   level of `OR`, at the root, and no `OR` appears below it.) The detector
   also returns the list of clause sub-roots (top-level `OR` children, or
   the singleton root in case (i)) and, per clause, the *support set* --
   the set of leaves reachable from that clause's root through the
   AND-only stratum. The support set determines `Pr[C_i]` and the
   conditional sampler. Cross-clause leaf sharing is allowed and expected.
2. **Argument parser**, method-local helper. The existing
   `probability_evaluate(token, method, arg)` signature already accepts a
   method-specific string, so the schema is unchanged; the new method
   accepts a `key=value` list (comma-separated) with bare-integer shorthand
   for backward symmetry with `'monte-carlo'`. Grammar:

   - `samples=N` (or just `N` as a positive-integer string) -- fixed
     sample count; deterministic runtime.
   - `epsilon=E` (alias `eps=E`) -- relative-error target. Implies the
     adaptive path: `N` is computed from the KLM Chernoff bound
     `N = ceil(4 * (e - 2) * m * ln(2/delta) / eps^2)` once `m` is known
     from the shape detector, where `delta` defaults to `0.05`.
   - `delta=D` -- failure-probability target. Only meaningful together
     with `epsilon`.
   - `max_samples=N` -- hard cap on the adaptive-path sample count, so
     extreme `m` or tight `eps` cannot run the backend out of time.

   Rules: missing argument means `epsilon=0.1, delta=0.05` (adaptive
   default; the user reaches for Karp-Luby for its FPRAS guarantee, so the
   default should be guarantee-shaped, not effort-shaped). `samples` is
   mutually exclusive with `epsilon` / `delta`. `max_samples` combines
   with `epsilon` / `delta`. Unknown keys raise. Document the formula and
   the defaults in `doc/source/user/probabilities.rst`.

3. **`BooleanCircuit::karpLuby(g, samples)`** in `src/BooleanCircuit.cpp`:
   takes a resolved sample count (the dispatcher does the `(eps, delta) -> N`
   conversion above, so the C++ method stays simple). `mt19937_64` seeded
   as in `monteCarlo` (the existing `provsql.monte_carlo_seed` GUC reused
   -- `src/BooleanCircuit.cpp:60` -- so regression tests can pin
   reproducibility); compute `p_i`; sample clauses; sample-and-
   membership-check; return `S * accepts / N`.

4. **Dispatcher branch** in `src/probability_evaluate.cpp`: if `method ==
   "karp-luby"`, parse the arg per (2), run the shape detector. On
   shape-detector failure, emit a `provsql_warning` ("not a DNF-shaped
   circuit") and raise rather than silently falling back -- the user
   picked Karp-Luby for its guarantee. On success, resolve `N` (either
   from `samples=` or from `(eps, delta, m)`), apply `max_samples` if set,
   call `BooleanCircuit::karpLuby`.

5. **Tests** (`test/sql/probability_evaluate_karp_luby.sql` /
   `*.out`): cover (i) flat DNF; (ii) one-level-of-sharing DNF; (iii) clause
   subsumption (one clause implies another) where the order-based rejection
   keeps the count right; (iv) a non-DNF circuit and assert the error;
   (v) reproducibility under `provsql.monte_carlo_seed`; (vi) cross-check
   against `'tree-decomposition'` on small enough instances; (vii) the
   four `key=value` arg forms (`samples=`, bare integer, `eps=`,
   `eps=,delta=,max_samples=`); (viii) error on conflicting
   `samples=` + `eps=`. Pin the seed and use a wide tolerance against the
   seed-dependent variance.

6. **Docs**: short subsection in `doc/source/user/probabilities.rst`
   describing the method, the assumed shape, the rare-event regime, and
   the `key=value` arguments with the default `(eps=0.1, delta=0.05)`;
   and a paragraph in `doc/source/dev/probability-evaluation.rst` (under
   "Currently Supported Methods") describing the dispatch.

LOC estimate: ~300 lines C++ (sample loop + shape detector + arg parser) +
~80 lines tests + ~40 lines docs. The sample-loop and clause-membership
pieces are essentially the same shape as `monteCarlo` + `evaluate` already
in `BooleanCircuit.cpp`; the arg parser is the main new piece, ~50 lines
including the validation of conflicting keys.

### Tier 2 -- improvements once Tier 1 lands

- **KLM self-adjusting stopping rule**: replace the fixed-`N` loop with
  the 1989 KLM stopping rule (sample until *accept* count reaches a
  deterministic threshold, then return the unbiased ratio). Same
  `(eps, delta)` surface as Tier 1; smaller hidden constants in practice;
  fewer wasted samples on easy inputs where `Pr[F] / S` is close to `1`.
  The Tier 1 arg parser already carries the right inputs; the change is
  internal to `BooleanCircuit::karpLuby`.
- **Per-clause stratified sampling**: for clauses with very different
  `p_i`, the importance-sampling step (categorical draw of clause index)
  has variance dominated by the largest clauses. Standard stratification
  cuts the constant by a factor up to `m`. Worth the ~20 lines.
- **Surface the same `key=value` parser from `'monte-carlo'`**: once the
  parser exists for Karp-Luby, extending `'monte-carlo'` to accept
  `samples=N` (with the bare integer still meaning `samples=`) is a
  one-line dispatch change and improves uniformity. (Naive MC cannot offer
  the adaptive `(eps, delta)` path -- its sample complexity depends on
  `p`, which is what we are estimating -- so it stays `samples=`-only.)

### Tier 3 -- research direction (positive-DAG generalisation)

Move from regime (b) to regime (c): handle a fully general positive Boolean
DAG without requiring the root to be an `OR`. Two routes:

- **Refinement-tree decomposition**. Walk the DAG top-down splitting `OR`
  nodes into clauses; at each step the partial witness records the disjunct
  taken at every `OR` ancestor. Each leaf of the refinement tree is a
  conjunction of literals (or the constant FALSE) and gives one DNF clause.
  This *can* blow up exponentially, but in practice mid-sized positive DAGs
  produce manageable refinement trees, and the technique gives an
  on-the-fly enumeration -- no need to materialise the whole DNF.
- **Self-reducibility + sampling**. Adopt the Jerrum-Valiant-Vazirani
  technique used in the DNF-sampling literature: given an FPRAS for `#DNF`,
  one obtains a uniform (or weighted) sampler from satisfying assignments,
  which in turn supports estimating any positive-circuit probability via
  recursive decomposition. Heavier, but the asymptotically right answer.

Both routes are research-paper-shaped; landing Tier 1 first is what makes
them worth attempting (the rare-event need is then proven; the DNF
infrastructure is in place).

## Open questions

- **Which real ProvSQL workloads benefit most?** The probe pattern is:
  evaluate a UCQ over a probabilistic database whose source-tuple
  probabilities are skewed toward `0` (rare-event semantics). Candidate
  benchmarks: TPC-H with derived probabilities; the case-study examples
  shipped under `doc/source/user/casestudy*.rst`; the `MayBMS`-style worlds
  literature corpus. A small empirical run on representative outputs is the
  right way to *decide whether to ship Tier 1*. Without it, the case is
  theoretical and the rare-event regime may turn out to be a small slice of
  real workloads.
- **Interaction with the safe-query rewriter**. The `provsql.boolean_provenance`
  pass tightens circuits via `foldBooleanIdentities` and the hierarchical
  rewriter. A Karp-Luby method runs *after* that pass, on the rewritten
  circuit. Two questions: (i) does the rewriter ever turn a Tier-1 shape
  into a non-DNF shape (e.g. by lifting an OR under an AND in a way the
  detector rejects)? (ii) does it ever produce circuits where Karp-Luby
  *beats* the post-rewrite `independent` / `inversion-free` exact path
  because the rewriter could not certify the structure?
- **Weighted-DNF semantics under `set_prob`**. The current
  `provsql.set_prob` interface assigns probabilities at the *leaf* level
  only. Karp-Luby is naturally weighted -- nothing changes for tuple-
  independent inputs. But once correlated inputs land (see
  [`conditioning.md`](conditioning.md) and MarkoViews), the
  "sample-leaves-independently" step needs the conditional sampler from
  the correlated structure; the unbiasedness proof still holds but the
  per-sample cost depends on the correlation structure.
- **HAVING extensions**. The current HAVING route
  (`enumerate_valid_worlds` in `safe-query-followups.md`) explicitly emits
  a DNF -- "ORs them all into a DNF that is then handed to the downstream
  evaluator". A Karp-Luby branch on that emitted DNF is a natural use
  case, in particular for MIN / MAX whose current path emits `2^N`
  clauses. Whether the resulting Karp-Luby cost (factor `m = 2^N`!) beats
  the safe-query-followups closed-form proposals is *almost certainly no*
  for MIN / MAX, but maybe yes for the cases where the closed forms do
  not apply.

## Priorities

The honest read: Tier 1 is **cheap to land** (~300 LOC, isolated to
`BooleanCircuit.cpp` / `probability_evaluate.cpp`, no schema or storage
change) and **closes a real gap** (rare-event FPRAS, in-process, no external
tool). The case for shipping it depends on whether real ProvSQL workloads
exhibit the rare-event regime often enough -- which is an empirical question
worth a short probe before committing to Tier 1.

Suggested ordering:

1. **Empirical probe** (one or two afternoons): pick a handful of
   ProvSQL-typical UCQs over a TPC-H-style probabilistic instance with
   skewed-low source-tuple probabilities; tabulate output-tuple
   probabilities; observe what fraction of outputs fall in the `m * p << 1`
   regime where Karp-Luby is asymptotically necessary. If the fraction is
   negligible, **stop**: file the study under "settled, decided against,
   kept for the rationale". If the fraction is material, proceed.
2. **Tier 1** -- minimum-viable Karp-Luby, plus tests and docs.
3. **Tier 2** -- adaptive-`(eps, delta)` API and KLM variant; ship as
   incremental polish.
4. **Tier 3** -- general positive-DAG extension; research-shaped, not on a
   schedule.

## Settled (decided against, kept for rationale)

*(empty -- this is a new study; this section exists for parity with the
other TODO documents and will be populated as choices firm up)*
