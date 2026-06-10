# ProvSQL Joint-Width UCQ Compiler — Implementation Specification

**Status:** draft for implementation
**Target repo:** github.com/PierreSenellart/provsql (current master)
**Estimated scope:** ~3–5 kLOC C++ + SQL wrappers + tests, structured in 4 milestones

---

## 0. Goal and theoretical basis

Build an exact probability-evaluation path for **arbitrary UCQs — including the
queries that are #P-hard under the Dalvi–Suciu dichotomy (H₀ = R(x),S(x,y),T(y)
and the Hₖ family)** — that is tractable whenever the **joint treewidth of the
data and its correlation structure** is bounded. Correlated inputs (tuples whose
provenance tokens are internal gates over shared events: materialized tracked
views, `repair_key` BID blocks, user constraint circuits) are handled natively.

Theoretical basis, in order of authority:

1. **Amarilli, PhD thesis (tel-01345836), §4.2** — cc/pcc-instances; Def. 4.2.5
   (joint relational encoding & joint treewidth); Thm. 4.2.7 (linear-size
   bounded-width provenance circuit for GSO on bounded joint-width cc-instances);
   Prop. 4.2.9 (the ∃T gate-evaluation query rewriting); Thm. 4.2.10 (ra-linear
   probability); **Prop. 4.2.11** (#P-hardness of the fixed CQ
   ∃xyz A(x,y)∧B(y,z) even with tw(I)=1 and tw(C)=0 separately — this is why
   the *joint* graph is the only sound screen); §4.3.1 Def. 4.3.1 / Prop. 4.3.2
   (Occ/Cooc width for formula-annotated inputs, Gavril clique argument).
2. **Amarilli–Bourhis–Senellart, arXiv:1511.08723 Appendix D** — same upper
   bound via cc-encodings + circuit stitching (Thm. D.1, Lemmas D.1–D.4);
   message-passing evaluation (Thm. D.2).
3. **Amarilli–Capelli–Monet–Senellart, ToCS 2020** — already implemented in
   `dDNNFTreeDecompositionBuilder`; we reuse its evaluation backend and its
   (valuation, suspicious-set) bookkeeping idea.

**Design decision (important):** we do NOT implement the generic MSO/automaton
route. We implement a **UCQ-specialized homomorphism-type DP that runs directly
over a tree decomposition of the joint encoding**, emitting a certified d-D
(deterministic, decomposable circuit) by construction — the same architectural
pattern as `src/ReachabilityCompiler.{h,cpp}` (states partition worlds ⇒
deterministic ORs; each variable introduced at exactly one node ⇒ decomposable
ANDs), with the bag state enlarged to carry in-bag **gate valuations** alongside
**partial homomorphisms**. This is the operational form of the thesis's
∃T q″(T) ∧ q_wf(T) rewriting (Prop. 4.2.9): instead of guessing the set T of
true gates with a second-order variable, the DP enumerates per-bag valuations
of in-bag gates, with mutual exclusivity giving determinism for free.

---

## 1. Scope and non-goals

**Session precondition:** the entire path is gated on
`provsql.provenance = 'boolean'` (`provsql_boolean_provenance`,
`src/provsql.c:100`) — the same gate the safe-query rewriter uses. The DP's
deterministic ORs merge derivations, so the emitted circuit captures
Boolean provenance / world membership only: it is a **probability artifact**,
never stored as tuple provenance, and is meaningless for counting or other
semiring provenance classes (N[X] needs the separate multiplicity-aware
machinery of thesis Ch. 5 / arXiv:1511.08723 §5 — out of scope). On any
other provenance class, refuse with an error hinting at the GUC.

**In scope (v1):**
- Query class: UCQs, constant-free or with constants folded into unary
  predicates, self-joins allowed, no negation, no aggregation, Boolean (the
  multi-output extension is M4).
- Input tuples whose provenance tokens are: `gate_input` (TID),
  `gate_mulinput` under a MULVAR block key (BID), or **any internal gate**
  of the shared mmap circuit (correlated inputs — this is the new capability).
- Exact probability of the Boolean UCQ; structural stats output.

**Out of scope (v1):** negation/FO (loses the absorbing-state collapse),
recursion (covered by `ReachabilityCompiler`), `gate_mixture`/continuous gates
(reject with a clear error), approximation fallbacks (the existing ladder
already covers those).

---

## 2. Pipeline overview

```
 user tuples (relation rows + provenance tokens, columnar arrays)
        │
        ▼
 [A] JointEncoding ── assemble joint graph:
        tuple-element vertices ∪ gate vertices
        + per-fact cliques  + fact↔gate (R⁺) cliques
        + circuit wire edges (ternary cliques per arity-2 gate)
        + BID block cliques
        │
        ├── degeneracyLowerBound screen  → reject early (fall through to ladder)
        ▼
 [B] TreeDecomposition(Graph, &elimination_bag)   (existing min-fill code)
        │  makeFriendly()
        ▼
 [C] UCQJointCompiler ── two-sweep DP over bags
        state = (gate valuation over in-bag gates,
                 partial-homomorphism set over in-bag elements,
                 per-disjunct satisfied bits w/ absorbing collapse)
        emits d-D gates directly (BooleanCircuit in d-D discipline)
        │
        ▼
 [D] dDNNF::probabilityEvaluation()   (existing)
        │
        ▼
 SQL: provsql.ucq_joint_evaluate(...) / ucq_joint_compile_stats(...)
```

---

## 3. Phase A — `JointEncoding` (new files `src/JointEncoding.{h,cpp}`)

### 3.1 Inputs

Mirror the columnar convention of `reachability_evaluate`
(`src/reachability_evaluate.cpp`, SQL wrappers in `sql/provsql.common.sql`):
the SQL layer hands the C++ layer parallel arrays describing the **facts**:

```cpp
struct FactRow {
  unsigned relation_id;                 // dense id of the relation symbol
  std::vector<unsigned long> elements;  // dense ids of domain elements
  pg_uuid_t token;                      // provenance gate (any gate type)
};
```

The SQL wrapper is responsible for: collecting the rows of each atom's
relation (post selection/constant filtering), mapping arbitrary SQL values
to dense element ids **with a shared dictionary across relations** (join
compatibility), and passing the UCQ structure (see §4.1).

### 3.2 Circuit slice extraction

From the set of distinct `token`s, walk the mmap circuit
(`MMappedCircuit::getGateType / getChildren / getProb / getInfos`,
see `src/MMappedCircuit.h`) to the **transitive closure of children down to
`gate_input` / `gate_mulinput` leaves**. Materialize the slice as a
`BooleanCircuit` (existing class), normalizing to arity ≤ 2 (chain rewrite,
as `Definition B.5` of arXiv:1511.08723 — fan-in >2 AND/OR becomes a balanced
binary tree of fresh gates). Record:

- `events`: the leaf set (each `gate_input` with its probability; each
  MULVAR block as one *block variable* with its outcome list — reuse the
  `EdgeBlock` pattern from `ReachabilityCompiler.cpp`).
- `fact_gate[F]`: the slice gate for each fact's token (ψ in thesis
  Def. 4.2.2).
- Reject gates outside {input, mulinput, and, or, not} with
  `JointCompilerException("unsupported gate type X")`.

Tokens that are constant-true (untracked relations) get `certain=true`
treatment exactly as `EdgeVariable.certain` does in the reachability compiler.

### 3.3 Joint graph construction

One `Graph` (the type consumed by
`TreeDecomposition(Graph, std::unordered_map<unsigned long, bag_t>*)`,
`src/TreeDecomposition.h:172`) over vertex ids partitioned as
`[0, n_elements)` for domain elements and `[n_elements, n_elements+n_gates)`
for slice gates. Edges (cf. thesis Def. 4.2.5 + the appendix's σ_Circuit
ternary convention — deliberately the *stronger* co-occurrence so every gate
is checkable in a single bag):

1. **Fact cliques:** for each fact F = R(a₁…aₘ), clique over
   {a₁…aₘ} ∪ {fact_gate[F]}  (this is R⁺ as an (m+1)-ary fact).
2. **Gate cliques:** for each internal slice gate g with children g₁,g₂
   (arity-2 normalized), clique over {g, g₁, g₂}. NOT-gates: edge {g, g₁}.
3. **Block cliques:** for each MULVAR block, clique over the block's
   alternative gates plus every fact gate mapped to one of them (matches
   the `EdgeBlock::endpoints must share a bag` invariant of the
   reachability compiler).
4. Nothing else. In particular **no event–event edges beyond what the wire
   structure implies** — Prop. 4.3.2's Cooc cliques arise automatically here
   because shared events are real shared leaf vertices of the slice.

### 3.4 Width screen

Run `TreeDecomposition::degeneracyLowerBound(graph, …)` first; if the lower
bound exceeds `provsql.joint_max_treewidth` (new GUC, default =
`TreeDecomposition::MAX_TREEWIDTH` which is 10, `src/TreeDecomposition.h:61`),
throw `TreeDecompositionException` → SQL layer reports "joint width too large"
and the caller falls back to the standard ladder
(`MethodCatalog`, `src/probability_evaluate.cpp:1612-1616`).

**Why the screen must be on this graph and nothing weaker — encode this in a
comment and a test:** thesis Prop. 4.2.11 exhibits instances with tw(data)=1
and tw(circuit)=0 that are #P-hard; all hardness lives in the fact↔gate
mapping edges (rule 1 above with shared event leaves). Tests in §7.2 verify
the screen correctly assigns those instances high joint width.

### 3.5 TID fast path — data graph only (important subcase)

When every gathered token is a `gate_input` **and all tokens are pairwise
distinct**, the circuit contributes nothing: each slice gate would be a
pendant leaf hanging off its one fact. In that case:

- **Skip slice extraction entirely** (no mmap walk beyond one
  `getGateType`/`getProb` per token; the distinctness check is one hash-set
  pass over tokens).
- **Joint graph := Gaifman graph of the facts** — per-fact cliques over
  domain elements only, no gate vertices. The screen is then literally the
  data treewidth, which is the correct sound screen in this regime (no
  correlation edges exist for Prop. 4.2.11 to exploit).
- **DP state drops `gate_val`/`suspicious` wholesale:** each fact is itself
  an independent Bernoulli variable, introduced at its representative bag;
  presence feeds the hom extension directly. This is exactly the
  architecture of `ReachabilityCompiler` with facts in place of edges.
- BID joins the fast path with the `EdgeBlock`-style treatment: one
  categorical variable per block, block clique over the alternatives'
  facts' elements — still no general circuit machinery.

**The trap that defines the precondition — shared tokens:** two gathered
rows carrying the *same* `gate_input` token are perfectly correlated facts
(this arises with SQL self-joins, where both atoms scan the same relation
and the same fact feeds two atom arrays, and with duplicated provenance in
user tables). Treating the two occurrences as independent variables is
**silently wrong**, not slow. v1 policy: dedupe by token during gathering;
a token gating facts over identical element tuples is one variable serving
multiple atoms (fine for the hom DP — this covers the standard self-join
case); a `gate_input` token shared across *different* element tuples routes
to the general path (cf. `ReachabilityCompilerException` for the analogous
shared-token restriction). Follow-up (cheap): merge such duplicates into
one multi-fact variable with a clique over the union of elements, staying
on the data graph.

**Implementation note:** do not write a second code path. Make the gate
machinery a template/config dimension of `UCQJointCompiler` (the `Ops`
pattern of the reachability compiler), so the fast path is the compiler
with the gate components compiled out — M1 then *is* the fast path, and M2
adds the gate machinery around the same DP core.

---

## 4. Phase C — `UCQJointCompiler` (new files `src/UCQJointCompiler.{h,cpp}`)

### 4.1 Query representation

```cpp
struct Atom   { unsigned relation_id; std::vector<unsigned> vars; }; // vars index into the CQ's variable list
struct CQ     { unsigned n_vars; std::vector<Atom> atoms; };
struct UCQ    { std::vector<CQ> disjuncts; };
```

Constants are pre-filtered by the SQL layer (selection pushed down), so atoms
contain only variables. Self-joins: two atoms may share `relation_id`.

### 4.1b Variable classification — the parameter that actually matters

The exponential parameter of the DP is **not** `n_vars`. Preprocess each CQ
to classify variables:

1. **Pinned:** output (free) variables — per answer they behave as
   constants — and variables equated to constants (already eliminated by
   selection pushdown).
2. **Determined:** compute the closure of the pinned set under the
   functional dependencies / keys of the input relations (iterate to
   fixpoint: a variable y is determined if some atom R(…x̄…y…) has an
   FD x̄→y with x̄ ⊆ determined∪pinned). Determined variables never
   multiply the state: their bag-local image is a *derived annotation* of
   the partial map (a function of the other coordinates plus the unique
   witnessing fact), carried alongside but not enumerated.
3. **Enumerating:** the rest — existential join variables not in the
   determination closure. Only these get coordinates in the hom code.

**Soundness of using catalog constraints:** FDs and keys are anti-monotone
(preserved by subinstances), so a constraint that holds on the underlying
instance I holds in every possible world ν(J) — declared `PRIMARY KEY` /
`UNIQUE` constraints read from `pg_constraint` are therefore sound, as are
BID block keys (per world, at most one alternative per block). The SQL
wrapper passes the FD set explicitly; the C++ layer does the closure.

**Where this knowledge is actually needed — screen, not compiler.** Under
the lazy discipline of §4.3b the runtime collapse of determined variables is
*automatic*: an FD x̄→y holding on I means that across all worlds the
y-image of a realized partial map is a fixed function of its x̄-images
(subinstances inherit the FD), so the realized hom-sets — and hence the
reached state count — are identical whether or not y is treated as a
coordinate; lazy enumeration only ever creates fact-witnessed maps. The
compiler therefore does not need the FD to run well; carrying an undetected
determined coordinate costs only constant factors (interning-key size).
What ahead-of-time knowledge buys is the **admission decision and
reporting**: the static `n_enumerating` shown in stats, the screen that
decides whether to attempt the joint path, and honest rejection messages.
A missed FD can only make the prediction pessimistic, never the run slow.

**Two sources for the determination set, both used:**

1. *Catalog:* `pg_constraint` keys/uniques (free, sound, often incomplete).
2. *Dynamic analysis of the actual data:* verify a candidate FD x̄→y by one
   hash-aggregation pass over the gathered fact arrays. This is **sound
   here even though it is not a schema constraint**, because the
   tractability argument only requires the FD to hold on this specific I —
   an instance-level certificate, in exactly the same epistemic category as
   measuring the instance's joint treewidth, which is the foundation of the
   whole method. Note it is checked on the *post-selection* gathered facts,
   where additional FDs frequently hold that fail on the base table.
   Make discovery **goal-directed**: only test FDs whose success would
   change the admission decision (variables that would bring
   `n_enumerating` under the threshold), so the cost stays a few linear
   passes.

**Screen policy:** if static `n_enumerating` ≤ threshold → compile; else run
goal-directed dynamic FD discovery and recount; else reject — or, under an
opt-in `provsql.joint_optimistic` GUC, attempt compilation anyway with the
`joint_max_states` cap as the real guard, since the cap (not the static
count) is the true safety net.

A second, automatic reduction applies at runtime: a variable occupies state
only while **live** (it has unmatched incident atoms and its image lies on
the current separator); fully-discharged variables collapse to DONE (§4.3).
So the worst-case exponent is `max over bags of simultaneously-live
enumerating variables ≤ #enumerating`, and the static `#enumerating` count
is what the planner/stats should report. For H₀ this is 2; for Hₖ it is
k+1 statically but typically far fewer live at once. Document in the user
docs that queries with ≤4–5 enumerating variables are the design target.

### 4.2 Variable/world model

The **world variables are the events** (slice leaves): independent Bernoulli
`gate_input`s, plus one categorical variable per MULVAR block. Each event is
**introduced at exactly one bag** — use the `elimination_bag` map returned by
the `TreeDecomposition(Graph,…)` constructor to assign every vertex (element
or gate) its elimination bag; an event is introduced at its elimination bag.
This preserves the decomposability invariant verbatim from
`ReachabilityCompiler.cpp` (each variable's gate children appear under exactly
one node's emission).

### 4.3 Bag state

```cpp
struct State {
  // (1) Valuation of the in-bag *gate* vertices, as implied by the
  //     events introduced in the subtree, with a "suspicious" subset for
  //     gates whose value is asserted but whose defining children are not
  //     all introduced yet — same mechanism as
  //     dDNNFTreeDecompositionBuilder's valuation_t / suspicious_t
  //     (flat_map/flat_set, bounded by bag size ≤ k+1).
  flat_map<local_gate_t, bool> gate_val;
  flat_set<local_gate_t>       suspicious;

  // (2) Homomorphism types: the set of partial maps h : vars(qᵢ) ⇀ bag
  //     elements ∪ {DONE} such that h is realized by the facts whose gates
  //     are true in the processed subtree, restricted to the bag interface.
  //     REPRESENTATION IS LAZY AND MANDATORY-LAZY (see §4.3b): a hom-set is
  //     a sorted small-vector of codes over the ENUMERATING variables only
  //     (§4.1b), hash-consed/interned so identical sets share one id; the
  //     State stores the interned id. Determined-variable images ride along
  //     as derived annotations of a code, never as enumerated coordinates.
  //     The code space ( ≤ (k+3)^{#enumerating} per disjunct ) is an a
  //     priori bound only — it is NEVER allocated or iterated; only codes
  //     realized by actual facts ever exist.
  //     DONE marks a variable mapped to an already-forgotten element whose
  //     incident atom obligations are all discharged.
  interned_homset_id hom;

  // (3) Per-disjunct satisfied bit, ABSORBING: when any disjunct's full
  //     match completes, collapse the entire state to SAT (drop hom and
  //     gate_val except interface gates still needed by parent bags).
  //     This is the CoverOps::final_collapse trick
  //     (ReachabilityCompiler.cpp:471ff) and is the main state-pruning lever.
  bool sat;
};
```

State-space cap: maintain a per-bag table size counter; if any bag's table
exceeds `provsql.joint_max_states` (GUC, default 1<<16), throw
`JointCompilerException` → fall through to the ladder. This mirrors the
reachability compiler's documented failure mode.

### 4.3b Laziness discipline (hard requirement)

Nothing whose size is a function of the *a priori* state space may ever be
materialized. Concretely:

- **Tables** are `unordered_map<State, gate_t, Hash>` keyed by reached
  states only (same shape as the reachability compiler's `Table`); states
  are created exclusively as successors of reached states under actual
  transitions.
- **Hom-sets are interned** (hash-consing): a global per-compilation pool
  maps the canonical sorted code-vector to a small id; equality and hashing
  of `State` then cost O(1) on the hom component. Interning also dedups
  downstream d-D gate emission (identical (valuation, hom, sat) states at a
  bag reuse one gate).
- **No precomputed transition or composition tables.** Join-composition of
  two hom-set ids and fact-extension of a hom-set are computed on demand and
  **memoized** in per-bag (or global, keyed by separator signature) hash
  maps — memoization replaces precomputation; entries exist only for
  encountered pairs.
- **Fact indexing is demand-driven:** facts are bucketed by representative
  bag once (linear pass over `elimination_bag`), and within a bag by
  (relation_id, projection onto in-bag elements) so the extension step
  touches only facts compatible with the code being extended — never a scan
  of the relation.
- **Why this works:** the realized-state count is governed by data sparsity
  (only fact-witnessed partial maps occur), the absorbing `sat` collapse,
  DONE-projection of dead variables, and determined-variable collapse
  (§4.1b) — typically orders of magnitude below the bound. The
  `max_states` stats column is the ground truth; the cap GUC is the safety
  net.

### 4.4 Transitions (bottom-up sweep)

Process bags in `makeFriendly()` post-order. At each bag:

- **Introduce events** eliminated at this bag: branch the state per outcome
  (true/false for Bernoulli; one branch per block outcome), and emit, for the
  branch, an AND over the corresponding literal gates (existing
  `BooleanCircuit` gates: the input itself or its negation via `gate_not`;
  for blocks, the `mulinput` literal — copy the handling from
  `ReachabilityCompiler`'s block code).
- **Confirm gates:** a gate is *confirmable* once both children's values are
  fixed in `gate_val` (guaranteed co-located by graph rule 3.3.2). On
  confirmation, check consistency with the asserted value; inconsistent
  branches are dropped (this is exactly the builder's suspicious-discharge).
  Confirmed `fact_gate`s flip *fact presence*.
- **Extend homomorphisms:** when a fact F (all of whose elements are in the
  bag, by graph rule 3.3.1, at F's representative bag — precompute the
  fact→bag assignment from `elimination_bag` of its clique's
  earliest-eliminated vertex, as documented at `TreeDecomposition.h:160-171`)
  becomes present, extend every compatible partial map of every disjunct
  containing an atom over relation F.relation_id; close `hom` under these
  extensions. Full maps set `sat` (absorbing collapse).
- **Forget elements** not in the parent bag: any partial map using a
  forgotten element either has all that variable's atom obligations met
  (remap to DONE) or is dropped. Forget gates not in the parent bag:
  must be non-suspicious (else drop branch).
- **Join states across children:** prefix/suffix products as in the
  reachability compiler (keeps node-arity linear); `gate_val` must agree on
  shared gates, `hom` ids compose via the memoized on-demand composition of
  §4.3b, `sat` ORs.

Every emitted OR is over states that **partition** the worlds of introduced
events (each world deterministically yields one (gate_val, hom, sat) tuple) ⇒
deterministic. Every emitted AND combines child gates over **disjoint**
introduced-event sets ⇒ decomposable. Put this argument in the file-header
doc comment in the style of `ReachabilityCompiler.h`'s.

### 4.5 Top-down sweep and root

Same above/below split as the reachability compiler (two explicit-stack
sweeps, `CHECK_FOR_INTERRUPTS()` in the hot loops, `#ifdef TDKC` harness
guard — copy the pattern from `ReachabilityCompiler.cpp:64-76`). The final
answer gate: deterministic OR over root states with `sat=true`, each ANDed
with its above-state gate. Feed to `dDNNF::probabilityEvaluation()`.

(Implementation shortcut allowed for M1: single bottom-up sweep with the
root = decomposition root and the query Boolean — the top-down sweep is only
needed for the multi-output extension M4 and for gate sharing across outputs.)

---

## 5. Phase D — SQL surface (edit `sql/provsql.common.sql`, new
`src/ucq_joint_evaluate.cpp`)

Functions (PG_FUNCTION_INFO_V1, modeled byte-for-byte on
`reachability_evaluate` / `reachability_compile_stats`):

```sql
-- relations: per-atom arrays of (row elements..., token); query: UCQ structure
provsql.ucq_joint_evaluate(query jsonb, facts ...columnar...) RETURNS double precision
provsql.ucq_joint_compile_stats(...) RETURNS TABLE(
  probability double precision,
  joint_treewidth int,        -- width of the decomposition found
  data_treewidth_lb int,      -- degeneracy LB of data-only graph (diagnostics)
  circuit_treewidth_lb int,   -- degeneracy LB of slice-only graph (diagnostics)
  n_bags int, max_states bigint, dd_size bigint)
```

Plus a convenience plpgsql wrapper that takes relation names + a UCQ spec and
gathers the columnar arrays (the reachability wrappers at
`sql/provsql.common.sql:4848ff` show the gathering pattern, including dense
vertex-id assignment). The three width columns in `compile_stats` exist
precisely to demonstrate Prop. 4.2.11 empirically: the adversarial family has
small data/circuit widths and large joint width.

**Integration with the ladder (M3):** register a `JointWidthMethod` in
`MethodCatalog` (`probability_evaluate.cpp:1612`) that is only *applicable*
when the evaluation context carries a joint-encoding sidecar (i.e., invoked
through the new SQL path, not on bare lineage — bare lineage has already lost
the data structure). Do not attempt query-recognition in the planner in v1;
the existing `classify_query` NOTICE can mention the function when it
classifies a top-level SELECT as OPAQUE over correlated sources.

### 5b. Dispatch discipline — never preempt cheaper exact paths

The joint compiler is the *last* exact resort with a guarantee, not a
default. Dispatch is two-dimensional — input class × query class — and the
input classifier already exists (`provsql_classify_query`,
`src/classify_query.h`: certified TID / BID / OPAQUE per source relation):

| inputs | query | route |
|---|---|---|
| TID/BID | hierarchical (safe) | existing safe-query read-once rewrite (`try_safe_query_rewrite`) — joint path **must not run** |
| TID/BID | inversion-free | existing `StructuredDNNF` order path — joint path **must not run** |
| TID/BID | unsafe (H₀, Hₖ, …) | joint compiler candidate; for pure TID the slice gates are pendant leaf vertices, so the joint screen degenerates to ≈ data treewidth |
| OPAQUE (correlated) | **any — including safe** | query-side safety is *inapplicable*: the Dalvi–Suciu dichotomy presumes tuple-independent inputs, and a hierarchical query over correlated inputs is not tractable by lifted inference. Try the cheap circuit-level rungs first (`IndependentMethod` — read-once/disconnection test is near-free), then the joint compiler. |

Mechanically: the safety / inversion-free analyses run first exactly as
today (they are planner-time and cheap); the `JointWidthMethod` registers
with a cost estimate derived from the degeneracy lower bound so
`chooseAndRun`'s lazy cost-ordering (`ProbabilityMethod.h`'s
feature-acquisition design) keeps it behind `IndependentMethod` and the
existing rungs, and its applicability predicate requires (a) the boolean
provenance gate, (b) the sidecar, (c) — for TID/BID inputs — that the
safe-query and inversion-free certifiers have *declined*. Add a regression
test (§7): a hierarchical query (R(x),S(x,y)) over TID inputs must be
answered with the joint compiler never invoked (assert via stats counter /
NOTICE absence); same query over a correlated (view-derived) R must route
to the joint compiler and match the brute-force oracle.

---

## 6. Files to create / touch

```
src/JointEncoding.h / .cpp        (Phase A; ~500 LOC)
src/UCQJointCompiler.h / .cpp     (Phase C; ~1500 LOC; model headers on
                                   ReachabilityCompiler.h's doc style)
src/ucq_joint_evaluate.cpp        (Phase D glue; ~400 LOC)
sql/provsql.common.sql            (wrappers; + upgrade script in sql/upgrades)
test/...                          (regression SQL + TDKC harness cases)
Makefile.internal                 (add objects; ensure -DTDKC harness builds
                                   UCQJointCompiler standalone like the
                                   tree-decomposition harness does)
```

Reuse, do not duplicate: `BooleanCircuit`, `dDNNF`,
`TreeDecomposition(Graph,…)` + `degeneracyLowerBound` + `makeFriendly`,
`flat_map/flat_set`, the MULVAR/`EdgeBlock` handling, interrupt guards.

---

## 7. Tests and benchmarks

### 7.1 Correctness oracles
For every test instance ≤ ~20 events: brute-force possible-worlds enumeration
(direct, in the test harness) must match `ucq_joint_evaluate` to 1e-9; also
cross-check against the existing ladder where it terminates
(`probability_evaluate` with method `monte-carlo` for statistical agreement,
`compilation` where d4 is available).

### 7.2 The thesis Prop. 4.2.11 family (mandatory)
Encode monotone 2-DNF χ = ⋁ᵢ (c¹ᵢ ∧ c²ᵢ): I = {A(aᵢ,bᵢ), B(bᵢ,cᵢ)},
circuit = bare inputs, ψ as in the thesis, q = ∃xyz A(x,y)∧B(y,z).
- (a) χ with *disjoint* variables per conjunct → joint width O(1); compiler
  must run linear and match closed form 1 − ∏ᵢ(1 − p(c¹ᵢ)p(c²ᵢ)).
- (b) χ = incidence of an expander / random 3-regular graph → degeneracy
  screen must reject (this is the #P-hard regime; *assert rejection*, and
  assert data_treewidth_lb ≤ 1, circuit_treewidth_lb = 0 in stats — the
  proof that separate screens are unsound).
- (c) χ = incidence of a bounded-treewidth graph (cycle, grid of width 2)
  → accepted; verify against brute force.

### 7.3 H₀ on treelike data, TID and correlated
- H₀ = R(x),S(x,y),T(y) with S a path/cycle/k-tree (k ≤ 6), TID tokens:
  compare against the existing tree-decomposition method; record sizes —
  expectation: joint compiler decomposes a graph of |dom|+|facts| vertices
  vs. the lineage circuit's larger gate count, with equal probabilities.
- Same, but R populated as `SELECT … FROM tracked_view` so R's tokens are
  internal AND/OR gates over S's and base events — *the correlated case no
  current exact method handles soundly with a width guarantee*. Brute-force
  oracle on small instances.
- Hₖ chains for k = 1..3, self-join variant R(x),S(x,y),R(y).

### 7.4 Scaling
Path-shaped joint instances n = 10³..10⁶: assert measured time is
near-linear and `max_states` is flat in n; record against
`reachability_compile_stats`-style baselines.

---

## 8. Milestones / acceptance

- **M1 (core = the §3.5 fast path):** Phases A–C, single-sweep, TID inputs
  with distinct tokens, data graph only, no gate machinery, CQ only.
  Accept: 7.1 green on H₀/Hₖ treelike, 7.2(a,c) green, 7.2(b) rejects.
- **M2 (correlations):** internal-gate tokens + MULVAR blocks; suspicious-set
  machinery complete. Accept: 7.3 correlated case green. *This is the
  headline milestone.*
- **M3 (surface):** UCQ (multi-disjunct + self-joins), SQL wrappers, stats,
  GUCs, ladder registration, docs page mirroring the reachability docs.
- **M4 (stretch):** free first-order variables — shared d-D with one root per
  answer (the top-down sweep), following `reachability_materialize`'s
  shared-gate pattern; Shapley/expectation pass-through (free once the d-D
  inputs are events — note in docs that attribution is to *events*, which is
  the semantically correct unit under correlations).

## 9. Known design risks (decide early, document in code)

1. **Hom-code width:** the parameter is `#enumerating` variables (§4.1b) —
   join attributes that are projected away, not constant-bound, and outside
   the FD/key determination closure — and at runtime only the live subset
   on the separator. The lazy representation (§4.3b) means the
   `(k+3)^{#enumerating}` figure is a bound, not an allocation; the design
   target is queries with #enumerating ≤ 4–5 at k ≤ 10, validated by the
   `max_states` column on the §7 benchmarks. Add to `compile_stats` the
   static `n_enumerating` per disjunct and the peak live-variable count, and
   add a §7 case showing an FD/key (e.g., S(x,y) with key x) collapsing a
   variable: state counts must match the query with y removed.
2. **Ternary gate cliques inflate joint width** vs. binary wires (§3.3 rule
   2). Accepted for v1 (simplifies confirmation); if benchmarks show it
   matters, switch to incremental AND/OR accumulation in the state (partial
   monotone aggregates), which restores binary edges.
3. **Element dictionary across relations** must be value-based (join
   semantics), not per-relation. Get this right in the SQL wrapper first.
4. **Slice explosion:** a view over the whole database drags in a huge
   circuit slice. The degeneracy screen handles soundness; add a slice-size
   GUC for early abort on memory grounds.
5. Do not regress the existing ladder: the new method must be strictly
   opt-in via the new functions until M3 review.
