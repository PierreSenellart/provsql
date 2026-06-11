# Möbius-inversion route for safe UCQs

Plan for the last missing exact route of the Dalvi-Suciu dichotomy:
unions of conjunctive queries that are safe **only because** the
#P-hard terms of their inclusion-exclusion expansion carry a zero
Möbius value on the CNF lattice and cancel. Anchored on Dalvi &
Suciu, *The dichotomy of probabilistic inference for unions of
conjunctive queries* (JACM 59(6), 2012) and its algorithmic core,
Dalvi, Schnaitter & Suciu, *Computing query probability with
incidence algebras* (PODS 2010); the LiftR reformulation in Gribkoff,
Van den Broeck & Suciu (UAI 2014, arXiv:1405.3250); and Monet &
Olteanu, *Towards deterministic decomposable circuits for safe
queries* (AMW 2018, arXiv:1912.11098) for the lattice/Möbius
computations at scale and the circuit-size impossibility results.

This supersedes the *Möbius / inclusion-exclusion via Monet 2019's
construction* entry of
[`safe-query-followups.md`](safe-query-followups.md): that entry
deferred an **intensional** route (building Monet's d-D circuit,
arXiv:1912.11864) on practicality grounds that still stand. The
route planned here is the **extensional** lattice-walking algorithm
itself, packaged the way the joint-width route already is: a
planner-time analysis attaching a certificate, an execution-time
compiler producing a certified circuit over the gathered data, and a
linear evaluation -- so the only genuinely new evaluation primitive
is a signed linear combination at the root.

Design requirements (fixed):

1. fires only when nothing better is available -- the safe-query
   rewriter, the inversion-free certificate, and the joint-width
   (data + correlations) route all keep priority;
2. invoked directly from the planner hook, no manual intervention:
   `SELECT probability(provenance()) FROM (… UNION …) t` just works;
3. easily demonstrated -- the demo query is the canonical QW / q9,
   with a stats surface that shows the lattice and the cancelled
   hard element.

## The gap (why)

Current exact-route coverage of the safe-query landscape:

| route | class covered | mechanism |
|---|---|---|
| safe-query rewriter (`src/safe_query.c`) | hierarchical self-join-free CQs (+ FD/constant extensions) | plan-time rewrite to read-once circuits, `independent` evaluation |
| inversion-free certificate | safe queries needing no inclusion-exclusion cancellation | per-input markers + `inversion-free` method, O(S + N log N) |
| joint-width (`src/joint_width_query.c`) | **data**-dependent: bounded joint treewidth, correlations allowed | descriptor at plan time, d-DNNF compiled at execution, linear evaluation |
| compilation / sieve / possible-worlds | small circuits, regardless of safety | exponential in circuit parameters |

The missing class is "safe but inversion-needed". The canonical
witness, due to Dalvi-Suciu and named QW in UAI 2014 (q9 in Monet &
Olteanu), is built from the four hard-boundary CQs over relations
`R(·)`, `S1(·,·)`, `S2(·,·)`, `S3(·,·)`, `T(·)`:

    h0 = ∃x∃y R(x) ∧ S1(x,y)        h2 = ∃x∃y S2(x,y) ∧ S3(x,y)
    h1 = ∃x∃y S1(x,y) ∧ S2(x,y)     h3 = ∃x∃y S3(x,y) ∧ T(y)

    q9 = (h2 ∨ h3) ∧ (h0 ∨ h3) ∧ (h1 ∨ h3) ∧ (h0 ∨ h1 ∨ h2)

(as a UCQ: distribute into a union of 8 CQs, of which 5 survive
minimisation). Its C-lattice has 9 elements; writing each element as
the disjunction of the `hi` it contains, the Möbius values µ(u, 1̂)
are:

| element λ(u) | µ(u, 1̂) |
|---|---|
| q9 itself (top) | 1 |
| h2∨h3, h0∨h3, h1∨h3 (co-atoms) | -1 each |
| h0∨h2∨h3, h1∨h2∨h3, h0∨h1∨h3 | +1 each |
| h0∨h1∨h2 | -1 |
| h0∨h1∨h2∨h3 = H3 (bottom) | **0** |

so

    P(q9) = P(h2∨h3) + P(h0∨h3) + P(h1∨h3)
          - P(h0∨h2∨h3) - P(h1∨h2∨h3) - P(h0∨h1∨h3)
          + P(h0∨h1∨h2)

Every right-hand term is a safe disjunctive sentence (a separator
exists, recursively); the bottom H3 -- which is #P-hard -- has µ = 0
and never needs to be evaluated. Naive inclusion-exclusion would hit
H3; lifted conditioning provably gets stuck on this lattice (PODS'10
§7); the Möbius grouping is what makes the query PTIME.

No ProvSQL route handles q9 in PTIME today: the safe-query rewriter
is per-CQ and hierarchical, the query is not inversion-free (that is
the point), and on adversarial data the joint treewidth is unbounded
(q9 also has no poly-size OBDD/FBDD/dec-DNNF -- Beame, Li, Roy &
Suciu -- so generic compilation provably cannot stay polynomial).
No published system implements this step either: SlimShot's rule
table (PVLDB 2016) only has plain binary inclusion-exclusion and
would fail on QW; Monet & Olteanu computed lattices and Möbius
values for millions of queries, but offline, in Python. ProvSQL
would be the first system with the complete safe-UCQ algorithm
integrated in a query planner.

## Out of scope

- **Ranking / shattering** (Dalvi-Suciu §5: rewriting relations by
  attribute order types so that the homomorphism criterion and
  separator existence become exact). Required for within-disjunct
  self-joins, repeated variables inside an atom, and constants in
  full generality; it multiplies relations by order types and is the
  main practical annoyance of the published algorithm. v1 restricts
  to *reduced-form* UCQs (see Plan, gate G5); ranking is Increment 3
  and may never be needed by real workloads. Constants alone can
  come earlier via the constant-selection shattering idea already in
  `safe_query.c`.
- **Correlated / BID inputs.** Lifted inference is only sound under
  tuple independence; correlated tokens stay with the joint-width
  route or fall through to the generic pipeline. Not a restriction
  to lift later -- it is a soundness boundary.
- **Monet's intensional d-D construction** (arXiv:1912.11864):
  still deferred, same cost-benefit as recorded in
  [`safe-query-followups.md`](safe-query-followups.md). The present
  route makes it even less urgent, since the extensional algorithm
  covers all safe UCQs (of the supported reduced form), not just the
  fragment that paper handles.
- **Extensional SQL plans** (SlimShot-style: generate per-element
  safe-plan SQL with `exp(sum(log(1-p)))` aggregates and combine in
  SQL arithmetic). Rejected as the artifact: it bypasses the
  token/circuit contract (`probability()` operates on a token, and
  the token must remain a usable Boolean provenance for every other
  surface), duplicates probability plumbing into SQL, and has no
  precedent in the codebase, whereas the compile-at-execution
  certified-circuit design has the entire ucq_joint machinery as a
  template.

## Plan

### 1. Shared UCQ extraction

`provsql_joint_width_descriptor` (`src/joint_width_query.c`) already
recognises exactly the input shape this route needs: UNION-of-SFW or
single SFW under `provsql.boolean_provenance`, normalised to
disjuncts → atoms → variable positions, with per-answer heads
canonically numbered. Factor the recognition/normalisation part out
into a shared module (precedent: the `qc_` qual-classification split
out of `safe_query.c`), consumed by both the joint-width descriptor
builder and the new Möbius analyser. One UCQ representation, two
back-ends.

### 2. Query-side analysis (`src/mobius_query.c`)

All steps below are query-complexity only (the query is tiny; the
data is never touched). Caps where a step is exponential in the
query, with an `elog(DEBUG1)` when a cap fires -- no silent
truncation.

1. **Components.** Split each disjunct into connected components
   γ of its shared-variable graph (a disjunct like `R(x), T(y)`
   contributes two components).
2. **DNF → CNF.** Distribute components: Φ = ⋀_f ⋁_i γ_{i,f(i)}.
   Exponential in the number of disjuncts; cap at M ≤ 8 CNF
   conjuncts (Monet & Olteanu handled millions of 4-clause lattices,
   so this is generous).
3. **C-lattice construction.** Elements are ϕ'_s = ⋁_{i∈s} ϕ'_i for
   s ⊆ [M], **collapsed up to logical equivalence**; meet = subset
   union. Equivalence and the simplification of each element are
   CQ-containment tests (homomorphism search; NP in query size,
   trivial at these sizes). The collapsing step is what creates the
   zero-Möbius cancellations -- it is the whole game. Prune to
   co-atomic elements: only meets of co-atoms can have µ(u, 1̂) ≠ 0
   (PODS'10 Prop. 3.4).
4. **Möbius values** µ(u, 1̂) by the standard incidence-algebra
   recursion, top-down (µ(1̂,1̂) = 1; µ(u,1̂) = -Σ_{u<w≤1̂} µ(w,1̂)).
5. **Safety check** for every µ ≠ 0 element, by the IndepStep /
   MobiusStep recursion on its disjunctive sentence:
   - minimise (core computation, homomorphisms);
   - split off variable-free components as an independent factor
     (PODS'10 Prop. 5.6);
   - **disjoint-symbols shortcut**: when disjuncts/components use
     pairwise-disjoint relation symbols, record a product/independent
     -union step instead of expanding inclusion-exclusion (PODS'10
     §6.2 "An Optimization" -- both are correct, the I/E form is
     exponentially larger);
   - find a **separator**: one variable per component such that the
     substituted sentences are pairwise tuple-independent. Use the
     maximal-variable criterion (Prop. 5.7): compute unification
     classes of variable positions (x, x' unify if they occur at the
     same position of atoms with the same symbol); a separator exists
     iff every component has a maximal variable, and the rank-1
     maximal variables *are* one -- no search. Root-variable
     existence alone is necessary but not sufficient;
   - recurse (MobiusStep) on the substituted sentence; FAIL if any
     reachable disjunctive sentence has no separator.
6. **Certificate.** On success, serialise a **Möbius certificate**
   (C-prefixed JSON, the `SafeCert` precedent): the µ ≠ 0 lattice
   elements with their integer coefficients, and per element the
   recursion trace -- component splits, separator variable choices,
   disjoint-symbol product steps, sub-lattices of inner MobiusSteps.
   The trace is what the execution-time compiler replays; no
   homomorphism test is ever run at execution time.

**Gates** (checked in this order, each falls through silently):

- G1: `provsql.mobius` GUC on (new bool, default on, mirroring
  `provsql.joint_width`) and boolean-provenance mode;
- G2: the query matches the shared UCQ extraction;
- G3: all source relations are TID (tuple-independent) -- reuse the
  `classify_query` TID fast path that the joint-width gather already
  trusts; BID and OPAQUE sources are rejected (soundness, see Out of
  scope);
- G4: caps respected (M ≤ 8 after CNF conversion);
- G5: reduced form -- no relation repeated within a disjunct, no
  repeated variable inside an atom, no constants (v1; see
  Increments);
- G6: the safety check of step 5 succeeds.

### 3. Planner integration ("only if nothing better")

The fall-through chain in `process_query` (`src/provsql.c`) already
encodes the required priority order; the Möbius analysis slots into
it without reordering anything:

1. `try_safe_query_rewrite` (provsql.c:12086) fires → early
   re-entry; the Möbius analyser is never consulted. Hierarchical
   CQs keep their read-once rewrite.
2. Inversion-free analysis (provsql.c:12116) attaches a certificate
   → gate the Möbius analysis on `inv_cert == NULL`, exactly like
   the existing joint-width gate. Inversion-free queries keep their
   O(S + N log N) path.
3. Joint-width descriptor build (provsql.c:4060, inside
   `make_provenance_expression`) -- **both** descriptors are
   attached when both shapes match (they recognise the same UCQ
   surface once step 1 of the Plan lands). The joint-width route
   keeps priority because it is strictly more general on the inputs
   it accepts (correlations) and its applicability (width ≤
   `provsql.joint_max_treewidth`) is only known at execution time.
4. **Runtime fallback point**: today, when the joint compilation
   exceeds the width cap, the circuit is skipped and the query
   degrades to plain semiring provenance. That exact spot becomes:
   if a Möbius certificate is present, invoke the Möbius compiler
   instead of degrading; only if neither applies, degrade as today.

This satisfies requirement (1) verbatim and keeps requirement (2)
for free: nothing new is user-visible, the planner hook does it all.

A note on (3): making the joint-width-vs-Möbius choice cost-based in
the method chooser (compare O(S·2^w) after a width probe against the
Möbius circuit size) is a possible follow-up, but strict priority
matches the stated requirement and is much simpler to reason about;
start there.

### 4. Execution-time compiler

A `mobius_compile` step (C++, sibling of `UCQJointCompiler`),
reusing the joint-width data gather (which already trusts
`get_table_info` and collects per-relation facts with their tokens):

- Replay the certificate's recursion trace over the gathered data:
  - an *independent-project* step at separator V becomes an OR over
    the active-domain values `a` of V, with children the recursively
    compiled circuits of ϕ[a/V] -- genuinely probabilistically
    independent children by the separator property, so the node gets
    an **independence certificate** (the same certified-island
    machinery the HAVING-certified work introduced and that the
    `independent` / `inversion-free` evaluators already consume in
    O(S));
  - a *component split* / disjoint-symbols step becomes an
    independent-AND, same certification;
  - a *variable-free atom* (fully substituted) becomes the input
    token of the corresponding fact, or constant FALSE if the fact
    is absent;
  - an inner *MobiusStep* contributes its own signed combination
    (see next point).
- **Memoise** subproblems keyed by (trace node, substitution of the
  stripped positions). This is what keeps the circuit polynomial:
  total size O(n^k · |trace|) for active domain n and maximal arity
  k, matching the algorithm's data complexity.
- The root (and each inner MobiusStep node) is a new combination
  gate `gate_mobius` carrying the integer Möbius coefficients:
  value = Σ_u µ_u · P(child_u). This signed linear combination is
  the **one new evaluation primitive**, and it is not a shortcut but
  a necessity: q9 has no poly-size FBDD/dec-DNNF and no poly-size
  d-SDNNF (Bova-Szeider), so any polynomial circuit artifact for
  this class must use negation/subtraction somewhere; putting it at
  dedicated combination gates with certified-independent islands
  below is the smallest such extension. (Storage: coefficients in
  the gate's `extra` blob, the `gate_arith` RV-style precedent.)
- The compiled root is wrapped as the per-row provenance token,
  exactly as `ucq_joint_provenance` does, so the token is created at
  execution time of the original query and `probability()` later
  evaluates it against the **current** probability catalog -- the
  circuit structure depends only on the data, never on the
  probabilities, so `set_prob` after the fact behaves as everywhere
  else.

### 5. Evaluation and observability

- Evaluation of a `gate_mobius`-rooted circuit is a linear sweep:
  certified-independent islands evaluate as today; at a
  `gate_mobius`, sum the children's probabilities with their signed
  coefficients. Clamp to [0,1]; warn (`elog(WARNING)`) if the
  pre-clamp value leaves [0,1] by more than fp tolerance -- a free
  sanity check, since the true value is a probability and a larger
  excursion means a compiler bug.
- Register a `"mobius"` method in the `MethodCatalog`
  (`src/probability_evaluate.cpp`): exact, in-default-chain,
  applicable only when the root carries a `gate_mobius`
  (a feature/inspection no more expensive than the existing
  DNF-shape walk), cost ~ kCost · S. With the gate-typed root,
  dispatch is in practice trivial, but the catalog entry gives
  by-name invocation, portfolio visibility, and a docs anchor.
- **Stats surface**, the demonstrability requirement: a
  `mobius_compile_stats` SRF in the spirit of
  `ucq_joint_compile_stats`, exposing at least: number of CNF
  conjuncts, lattice size before/after equivalence collapsing,
  number of µ ≠ 0 elements, number of **cancelled** elements and
  whether any cancelled element is hard (for q9: "1 element
  cancelled, µ = 0, hard"), circuit size, memo hit rate, and the
  probability. The cancelled-hard counter is the single number that
  makes the mechanism legible in a demo.

### 6. Tests and demonstration

Following the usual checkpoint discipline: working core + verified
numbers first, docs/case-study material after confirmation.

`test/sql/mobius_safe.sql`:

- **Warm-up** (lattice machinery, no cancellation needed):
  Φ₁ = R(x),S(x,y) ∨ S(x,y),T(y) ∨ R(x),T(y). Safe,
  non-hierarchical; its CNF is (γ_R ∨ γ_ST) ∧ (γ_T ∨ γ_RS) and the
  lattice bottom collapses by implication to γ_R ∨ γ_T -- safe, so
  this exercises CNF conversion, equivalence collapsing and
  minimisation without zero-Möbius cancellation:
  P = P(γ_R ∨ γ_ST) + P(γ_T ∨ γ_RS) - P(γ_R ∨ γ_T).
  Cross-check against `sieve` / `possible-worlds` on a tiny
  instance.
- **Flagship: q9 / QW** over R, S1, S2, S3, T.
  - Tiny instance: verify against brute force (`possible-worlds`).
  - Larger instance: random bipartite S-relations with a branch
    cross-product (the shape the joint-width bench uses to force
    joint width past the cap), so that *only* this route is exact
    and fast; verify on a structured sub-case with a computable
    closed form before claiming anything (bench-gotchas rule).
  - Check `mobius_compile_stats`: 9-element lattice, 8 elements
    with µ ≠ 0, 1 cancelled hard element.
- **Negative gating tests**, one per priority rule:
  - a hierarchical CQ → safe-query rewrite fires, no Möbius
    compile (stats SRF shows no firing);
  - an inversion-free UCQ → certificate route wins;
  - q9 on a low-joint-width instance → joint-width wins;
  - q9 over a BID table → route refuses (G3), query falls through
    to the generic pipeline with the usual approximate methods.
- Per-answer variant (Increment 2): grouped heads, head-pin like the
  joint-width single-sweep.

Docs (after the core is confirmed): method-catalogue entry for
`mobius`; a new row in the tractability grid table -- "safe UCQs
(incl. Möbius inversion): PTIME data complexity, O(n^k) factor" --
with `:ref:` link to a new mechanism section; literature pointers to
Dalvi-Suciu 2012 and Monet & Olteanu 2018 (including the FBDD lower
bound explaining why generic compilation cannot do this). A "the
query that is easy only because its hard part cancels" case study /
Studio notebook is a natural follow-up, deliberately out of the
critical path.

## Priorities

1. **Increment 1 -- the demonstrable core.** Shared UCQ extraction;
   lattice + Möbius + safety analysis with gates G1-G6; certificate;
   planner gating and the runtime fallback point; compiler with
   memoisation and `gate_mobius`; evaluation + clamp; stats SRF;
   warm-up + q9 tests + negative gating tests. Boolean queries only,
   reduced form, TID inputs. (New C symbols → postgres restart on
   deployment, as usual.)
2. **Increment 2 -- surface completeness.** Per-answer (free head
   variables) via the head-pin mechanism established by the
   joint-width single-sweep; constants via constant-selection-style
   shattering; docs row + method-catalogue entry; case-study
   material if the demo lands well.
3. **Increment 3 -- full generality (optional, research-flavoured).**
   Ranking/shattering for within-disjunct self-joins and repeated
   variables (the `R(x,y),R(y,x)` family); revisit only if a
   workload needs it. Cost-based joint-width-vs-Möbius arbitration
   in the chooser, only if strict priority proves wasteful in
   practice.

## Implementation observations

- **Use the C-lattice, not the D-lattice.** The dual (D-lattice)
  algorithm is provably incomplete: PODS'10 Ex. 5.8 gives a safe
  query whose D-lattice is the Boolean lattice 2^[3] with no zero
  Möbius values yet an unsafe element. All lattice work in step 3
  is on the CNF side.
- **Equivalence collapsing is the correctness crux.** Miss a
  collapse and a hard term fails to cancel (the safety check then
  FAILs -- annoying but sound); collapse two inequivalent elements
  and the *value* is silently wrong. Mitigations: the reduced-form
  restriction G5 (under which the homomorphism criterion is sound
  without ranking -- on unranked sentences with constants it is
  not, cf. the `R(x,a),S(a,z)` counterexample), brute-force
  cross-checks on small instances, and the [0,1] clamp warning.
- **Φ₁ needs no cancellation on the C-lattice** (its bottom is
  safe); q9/QW is the smallest standard example whose C-lattice has
  a hard zero-Möbius element. Hence both tests: Φ₁ would pass even
  with a broken cancellation step, q9 would not.
- **Avoid gratuitous inclusion-exclusion**: the disjoint-symbols
  product shortcut is not an optimisation nicety -- without it the
  trace (and circuit) for ordinary independent unions is
  exponentially larger than needed.
- **Why not reuse the d-DNNF evaluators wholesale**: the per-element
  islands are independence-certified (independent-OR is not
  deterministic-OR), so they go through the `independent`-style
  certified evaluation, not the d-DNNF determinism path; only the
  signed top combination is new.
- **Numeric note**: Möbius coefficients are small integers (bounded
  by the lattice structure; for q9 all are ±1), and the per-element
  probabilities are exact up to fp; cancellation-induced fp error is
  bounded by Σ|µ_u| · ulp and is negligible at these lattice sizes.
- **SlimShot precedent**: its engineering tricks (independent union
  via GROUP BY rather than outer-join cascades, product aggregates
  via `exp(sum(log p))`) are for the extensional-SQL artifact we
  rejected; nothing to reuse beyond validation that safe plans in
  PostgreSQL are practical.
- The Monet & Olteanu computation (all monotone Boolean functions on
  ≤ 7 variables, lattices + Möbius for millions of queries, weeks on
  40 CPUs *including generation*) is the empirical evidence that
  steps 2-4 are entirely cheap at realistic query sizes; the M ≤ 8
  cap will in practice never fire.
