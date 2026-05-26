# Implementation Plan: Inversion-Free UCQ → OBDD

## Context

`doc/TODO/safe-query-followups.md` (Tier 2) documents a known tractability
gap. The safe-query rewriter today lands **read-once** UCQs (hierarchical,
self-join-free) and short-circuits their probability evaluation. The next
rung on the Jha & Suciu (ICDT 2011) compilation ladder is `UCQ(OBDD)`,
characterised *exactly* as the **inversion-free** queries, with a linear-size
OBDD obtained from a **query-derived variable order** (Prop. 4.5: root
attribute first, then by quantifier depth, relation symbol last; width `2^g`
for `g` atoms).

The principled handle this buys us is **consistent-unification self-joins**:
a tractable corner the rewriter bails on today. The canonical witness is
`q = S(x,y),A(x,y),S(x,z),B(x,z)` (self-join on `S` through the root `x`).
The TODO records a benchmark showing the gap is real: on this witness with
`n×n` relations, *every* circuit-level method fails or crawls by `n=6`.
`independentEvaluation` rejects (not read-once), tree-decomposition throws
`Treewidth > 10` from `n=5`, `d4` times out, and even `panini-obdd` times out
at `n=4`. The reason is that inversion-freeness is a **query-level** property
and the poly OBDD needs the query-derived order, which a compiler seeing only
the Tseytin-CNF'd lineage cannot recover.

**Intended outcome (full scope):** (1) a query-level detector recognising
inversion-free `UCQ(OBDD)` over tuple-independent inputs, (2) a **tractability
certificate** attached at the rewritten root that routes the probability
dispatcher past the doomed tree-decomposition attempt, and (3) an **in-process
construction of a structured d-DNNF guided by the query hierarchy** so the
witness family, which times out today, evaluates in time linear in the lineage.
(The class is `UCQ(OBDD)`; the *construction* exploits decomposability rather
than emitting a literal linear OBDD — see §4.)

## Scope and soundness preconditions

The construction is a structured d-DNNF over **independent Bernoulli** variables;
`dDNNF::probabilityEvaluation` and the Prop. 4.5 `2^g`-width bound both assume
each input leaf is an independent event. Certification therefore requires
**all four** of:

1. **Hierarchical.** Inversion-free implies hierarchical; reuse the existing
   class-coverage check.
2. **Positional consistency.** Self-join occurrences agree on the class at
   every *bound* (shared) position; free columns may differ.
3. **Precedence acyclic.** The class-precedence graph `G_prec` is a DAG (no
   inversion).
4. **Tuple-independent (TID), with input-leaf annotations.** Every atom is
   classified `PROVSQL_TABLE_TID` (`classify_query.c:61`), **and** the
   provenance tokens it contributes — the gates we annotate as OBDD variables —
   are `gate_input` leaves. This is *not* restricted to base tables: a view
   that selects/projects from a single TID table qualifies, because
   selection/projection passes each row's input token through unchanged. What
   it excludes is token-*combining* derivation (join, duplicate-eliminating
   projection, aggregation), whose tokens are `plus`/`times` gates, not leaves.

Two things the fourth precondition rules out, both grounded in the fact that
ProvSQL keeps the full derivation history in the circuit store:

- **BID is out of scope.** Block-independent-disjoint relations
  (`PROVSQL_TABLE_BID`, the mutually-exclusive `gate_mulinput` blocks produced
  by `repair_key`) carry a disjointness constraint that an OBDD over
  independent variables cannot represent: `dDNNF::probabilityEvaluation` treats
  inputs as independent, and lowering mulinputs into a constrained Boolean
  encoding inflates the circuit and destroys the query-derived order, killing
  the `2^g` argument. This is **the key asymmetry with the read-once path**,
  which *does* admit BID via block-key alignment
  (`src/safe_query.c:1871-1913`, replicating a block's `gate_mulinput` across
  the wrap's DISTINCT). The inversion-free path is strictly narrower: TID only,
  self-join or not.
- **Token-combining derivation is out of scope, but selection/projection is
  not.** What must hold is that every annotated variable is a `gate_input`
  leaf, hence a genuine independent Bernoulli. A view that merely
  selects/projects from a single TID table preserves this — its rows still
  carry that table's input tokens — and is admitted. A relation whose tokens
  are `plus`/`times` circuits (a join, a DISTINCT or aggregation, a
  materialised query result) is not: it contributes no leaves, and two such
  atoms can share ancestry and be *correlated*, which the independent-variable
  model silently gets wrong. Conveniently, **one circuit-level check covers
  both this and the TID condition: the annotated gate must be `gate_input`** —
  which excludes `gate_mulinput` (BID) and every non-leaf gate at once. A
  **self-join of the same TID relation is fine**: distinct tuples are
  independent Bernoullis, and an identically-unified tuple is a single shared
  variable (handled by content-addressing / idempotence, below).

Jha–Suciu's `UCQ(OBDD)` is stated over tuple-independent databases, so this is
a faithful scoping, not a shortcut.

**Generalisation (beyond phase 1): extended TID.** The `gate_input` test is a
*sufficient* independence condition, not the true boundary. The construction
extends to **extended-TID** relations — each tuple annotated by a bounded-size
circuit over a *private* set of independent leaves, with distinct tuples having
**disjoint** leaf supports so they remain mutually independent. There the
annotated gate is the tuple's sub-circuit root rather than a leaf; the order
ranks the leaves *below* each annotated gate **consecutively** (their
internal order is free — only the per-tuple block placement matters for
Prop. 4.5), and the `2^g` width becomes `2^g · 2^b` for bounded block size `b`,
still linear in the tuple count. The independence obligation then reads
"annotated gates have pairwise-disjoint, bounded leaf supports" rather than
"annotated gate is `gate_input`". Phase 1 ships the leaf-only predicate; this
is the documented next widening of the admissible class (and `gate_mulinput`
stays excluded — BID is a *disjoint*, not independent, annotation).

## Validation (proof-of-concept spike)

Before building the detector, a throwaway standalone harness (the `tdkc`
`-DTDKC` source set plus a spike `main`; built and run, kept uncommitted)
validated the core bet on the `n×n` witness, against ProvSQL's own
`BooleanCircuit` and its tree-decomposition d-DNNF builder:

| n | #vars | tree-decomposition | structured query-order |
|---|------:|---|---|
| 2 | 12 | tw=3, 0.001s | match, ~10µs (also = brute force) |
| 6 | 108 | tw=7, 0.46s | match, ~10µs |
| 8 | 192 | tw=9, ~12–18s | match, ~10µs |
| 10 | 300 | blows up (tw > MAX=10) | ~10µs |
| 100 | 30000 | blows up | 0.32ms |

- **Correctness.** The structured value exactly matches tree-decomposition at
  every `n≤8` and brute force at `n=2`, over non-trivial probabilities
  (~`1e-5` … `0.6`).
- **The gain is real.** Tree-decomposition treewidth grows with `n` (3→9), its
  runtime is super-linear (~12–18s at `n=8`), and it exceeds
  `MAX_TREEWIDTH=10` at `n=10`; the structured computation is ~10µs through
  `n=8` and 0.32ms at `n=100` (30k variables) — linear, realising the bounded
  atom-frontier recursion (width `2^g`, `g=2`).
- **Lineage-shape lesson (load-bearing for the builder *and* the tests).** The
  faithful ProvSQL lineage is the **flat** `OR_{i,j,k} (t1_ij AND t2_ik)` with
  shared join-intermediates `t1_ij = S_ij∧A_ij`, `t2_ik = S_ik∧B_ik`; its
  treewidth grows with `n`. The **factored** `(OR_j t1_ij) ∧ (OR_k t2_ik)` is
  low-treewidth (`tw=2`) and tree-decomposition solves it instantly — the spike
  initially mis-built this and saw no blow-up. So a test that *hand-builds* the
  factored circuit would not exercise the hard case at all; re-discovering that
  factorisation from the flat lineage is precisely the approach's job, and the
  verification suite must drive the lineage through the **real planner**, never
  a hand-factored circuit.

This de-risks the construction half (see risk #1): the bounded-frontier
recursion is correct and linear. Still unbuilt and ahead: the detector
(query-level recognition + order recipe), the annotation-gate carrier, and
materialising the structured d-DNNF through `dDNNF::probabilityEvaluation` (the
spike computed the probability inline rather than emitting a d-DNNF).

**Future benchmark (read-once overlap).** Hierarchical self-join-free queries
over TID are *both* read-once and inversion-free, so both the existing
safe-query plan (`independentEvaluation` on the rewritten read-once lineage) and
the new structured-d-DNNF path can evaluate them. Eventually benchmark the two
head-to-head on that overlap: it tells us whether the structured path is
competitive where both apply, and therefore whether routing read-once-eligible
queries through it (instead of, or before, the read-once rewrite) is ever
worthwhile, or whether the two paths should stay strictly partitioned by class.

## Design anchors (verified against the code)

- **Probability model: independent inputs only.** The decision variables are
  the input gates of the `BooleanCircuit` (`bc.getInputs()`); each is an
  independent Bernoulli. `BooleanCircuit` already tracks multivalued inputs in
  a separate `mulinputs` set and several methods refuse on `hasMulInputs()`, so
  the TID precondition has a cheap runtime backstop (below).
- **Exact dispatch point.** `probability_evaluate_internal`
  (`src/probability_evaluate.cpp:93`) loads `GenericCircuit gc`, builds
  `BooleanCircuit c = getBooleanCircuit(gc, token, gate, gc_to_bc)`, then for
  `method==""` tries `independentEvaluation` and falls into `makeDD` →
  `interpretAsDD` → `TreeDecomposition` → `compilation("d4")`. The
  structured-d-DNNF attempt slots into the `independentEvaluation` catch,
  *before* `makeDD`: a new rung between `independent` and `tree-decomposition`,
  matching the TODO's ladder.
- **`paniniCompile` is the emission template.** `BooleanCircuit::paniniCompile`
  already turns OBDD decision nodes into a `dDNNF` of shape
  `OR(AND(v,hi), AND(¬v,lo))`, sets the root, and calls `simplify()`.
  `dDNNF::probabilityEvaluation()` (linear-time, `src/dDNNF.h:195`) then
  evaluates it unchanged. The structured-d-DNNF builder emits the same shape at
  its decision nodes, plus flat decomposable `AND` nodes at independence points.
- **Detector is variable-level today.** `find_hierarchical_root_atoms`
  (`src/safe_query.c:808`) builds a union-find over `(varno,varattno)` Vars and
  *bails* when a class binds two columns of one atom. Inversion detection needs
  the column-position information this collapses, so it is a **sibling pass**,
  not an in-place edit. The TID/derived checks reuse the per-relation
  `info.kind` lookup the candidate gate already performs
  (`src/safe_query.c:178`).
- **Carrier: a new transparent annotation gate whose UUID folds its `extra`.**
  ProvSQL gates are content-addressed: `provenance_times` derives its token as
  `uuid_generate_v5(uuid_ns_provsql(), concat('times', children))`
  (`sql/provsql.common.sql:737`) — purely `(operator, children)`. So a cert or
  order-key string parked on a `plus`/`times` (or on any wrapper following the
  children-only convention, including `assume_boolean`,
  `sql/provsql.common.sql:158`) cannot vary independently of the children: the
  dedup would merge gates that must stay distinct. The carrier is therefore a
  **single new gate type** whose creation helper hashes
  `uuid_generate_v5(ns, concat('annot', child_token, extra))` — content-
  addressing *preserved*, with `extra` promoted into the identity. This is the
  one deliberate, localised departure from the children-only convention; it
  lives entirely in that helper, and everything downstream is keyed by the
  explicit token (`circuit_cache` included), so no other code assumes
  `(type, children)` determines identity for this type.
- **The annotation gate is semiring-transparent and collapse-immune.** It is a
  single-child identity: `evaluate<S>` returns `evaluate<S>(child)` for every
  semiring (it carries no semantics, unlike `gate_assumed_boolean`, which is a
  Boolean-only poison pill at `src/GenericCircuit.hpp:117` and is therefore
  *not* reused here). `getBooleanCircuit` evaluates *through* it, so it mints no
  Boolean variable. Being neither `plus` nor `times`, it is never touched by
  the single-wire identity collapse in `foldSemiringIdentities`
  (`src/GenericCircuit.cpp:175-187`), so the cert/key survives load with **no
  special-case guard**.
- **`extra` reuse is collision-free.** Every existing `extra` consumer is a
  scalar/value-dimension leaf — `gate_value` (the only `extra` reader inside
  the generic `evaluate<S>`, `src/GenericCircuit.hpp:160`, plus HAVING
  pre-eval), `gate_rv`, and the synthetic categorical `gate_mulinput`. None is
  the annotation gate, and no evaluator reads `extra` for the annotation gate
  except the probability path we add.

## Recommended approach

### 1. Detector – `src/safe_query.c` (sibling pass)

Add `detect_inversion_free(constants, q, quals, /*out*/ recipe)` immediately
after `find_hierarchical_root_atoms`. Reuse the existing union-find closure
(`cls[]`, `vars_arr[]`), then apply the four preconditions above as a *sound
under-approximation* of inversion-freeness (documented as such, matching how
the rewriter under-approximates read-once today):

1. **Hierarchical** — reuse the class-coverage check. A non-hierarchical query
   is never inversion-free.
2. **Per-relation positional consistency.** For each relation symbol with ≥2
   occurrences (a self-join), build the per-occurrence column→class vector.
   *Reject* if two occurrences bind the **same class at different positions**
   (catches the path `R(x,y),R(y,z)`: `y` is at pos 2 in one occurrence, pos 1
   in the other). *Admit* when all occurrences agree on the class at every
   *bound* (shared) position (the witness: both `S` occurrences have `x` at
   pos 1; the differing free columns at pos 2 are fine — see §2 on why free
   positions are key-consistent).
3. **Global precedence acyclicity.** Build a directed graph `G_prec` over
   classes: for each atom `R(c_1..c_k)` add edges `class(c_i)→class(c_j)` for
   `i<j`. *Reject* if `G_prec` has a cycle (catches symmetric closure
   `R(x,y),R(y,x)`: `x→y` and `y→x`). The topological order of `G_prec` (root
   class first, since the root touches all atoms) is the **class-order seed**
   for Prop. 4.5.
4. **TID + input-leaf annotations.** *Reject* unless every atom is
   `info.kind == PROVSQL_TABLE_TID` *and* the tokens it contributes are
   `gate_input` leaves. A base TID table qualifies; so does a view that
   selects/projects from a single TID table (tokens pass through unchanged).
   *Reject* BID and OPAQUE atoms, and token-combining relations (join /
   DISTINCT / aggregation / materialised result) whose tokens are `plus`/`times`
   gates rather than leaves. The single operational test is "the gate we would
   annotate is a `gate_input`"; a self-join of one TID relation is admitted.
   The query-level metadata gates TID cheaply at plan time; leaf-ness is
   enforced concretely when the input markers are attached (§2): if the token
   that would be wrapped is not a `gate_input`, abort certification.

Returns a `SafeCert` recipe (success) or NULL. Relax the self-join rejection in
`is_safe_query_candidate` (`src/safe_query.c:141-157`) by adding an
`approved_inversion_free_relids` set, consulted exactly like the existing
`approved_self_join_relids` **but conjoined with TID-only**. The inversion-free
path does *not* call `rewrite_hierarchical_cq` (which would try to make it
read-once and fail): `try_safe_query_rewrite` branches, leaving the lineage
intact and attaching the cert + per-input order markers.

### 2. Query-derived order (Prop. 4.5) carried by per-input markers

The order is **data-dependent** (it references each tuple's attribute values),
so it cannot be a static permutation. The builder does not need a single global
total order (that was the OBDD framing); it needs, per indecomposable
component, *a Prop. 4.5-consistent order* — used to rank variables within the
component and to pick the next separator (§4) — derived from a sort key per
input leaf.

**Per-input key is a function of the tuple, not the derivation role.** The key
is built from the tuple's *physical column values* arranged by the recipe
(root-class value first, then the remaining classes in `G_prec` topological
order, relation rank last). Crucially it keys the secondary slots on the
*column value*, not the per-derivation class label. This is what makes the
consistent-unification self-join work: in the witness, an `S`-tuple `(a,b)`
used as `S(x,y)` carries `x=a,y=b`, and used as `S(x,z)` carries `x=a,z=b` —
the classes `y`/`z` differ, but the physical second column is one column with
value `b` in both. Positional consistency (detector step 2) guarantees the
*bound* positions agree, and free positions are single physical columns, so the
key is **single-valued per tuple**, independent of role.

**Markers carry the key, and content-addressing collates them for free.** At
lineage-build time, on the certified path, wrap each certified input leaf in an
annotation gate whose `extra` holds the order key (discriminator-prefixed,
e.g. `K…`). This wrap is also where the input-leaf precondition is *enforced*:
markers are only ever attached to `gate_input` gates, so if a certified atom's
contributed token turns out not to be a leaf (a `plus`/`times` from a
token-combining derivation, or a `gate_mulinput` from BID), certification is
aborted and the cert is not written — the query falls back to the normal chain. Because the annotation UUID folds `extra`
(`v5(ns, concat('annot', input_token, key))`), two uses of the same Boolean
variable produce the **same** marker token and dedup to one gate — exactly the
mechanism by which `times` gates dedup. So there is **one marker per Boolean
variable** by construction; the builder reads one key per variable and sorts.
No occurrence-collation pass, no per-variable conflict handling: a correctly
emitted key cannot produce two markers for one variable. (Whether the
column-determined key is a *valid* Prop. 4.5 order is the detector's job — see
risk #4.)

The builder obtains keys at evaluation time by walking `gc` for annotation
gates whose child is an input, reading `extra`, and mapping the wrapped input
to its Boolean variable via the `gc_to_bc` map it already receives at the
dispatch point. **No SPI, no side-table, no new IPC at evaluation time**; the
markers round-trip through mmap with the rest of the circuit.

### 3. Tractability certificate

New header `src/safe_query_cert.h` (shared C/C++ via `extern "C"`):

```c
typedef enum { CERT_NONE = 0, CERT_INVERSION_FREE = 1 } SafeCertKind;
typedef struct {
    SafeCertKind kind;
    int   nclasses, root_class, natoms;
    int  *class_topo_order;     /* order positions, root first */
    int  *atom_relation_rank;   /* relation-symbol tie-break per atom */
    int  *atom_col_class;       /* flattened column->class anchor map */
} SafeCert;
```

with serialise / parse helpers to and from a compact string.

**One gate type, two roles, self-disambiguated by `extra`.** The same
annotation gate carries the cert and the order keys; the `extra` payload's
discriminator prefix says which:

- **Root use** — wrap the per-row provenance root in an annotation gate whose
  `extra` is the serialised `SafeCert` recipe, prefixed `C…`. Token =
  `v5(ns, concat('annot', root_token, cert))`. The result token *is* this
  wrapper, so `gc.getExtra(gc_root)` returns the cert directly, and (not being
  `plus`/`times`) it survives load untouched.
- **Input use** — §2's per-input order-key markers, prefixed `K…`.

DAG position confirms the role independently (the root marker is `gc_root`;
input markers are the ones whose child is an input gate), so the two signals
agree and neither has to be guessed.

**Data flow.** The planner side stamps both via the existing `set_extra(uuid,
text)` SQL UDF (`sql/provsql.common.sql:180`, `src/provsql_mmap.c:339`) — or,
since the annotation UUID is content-derived from `(child, extra)`, via the
annotation-gate creation helper directly. The evaluation side reads
`gc.getExtra(gc_root)` in `probability_evaluate_internal`, parses the cert, and
acts on it at the dispatch point.

**Soundness.** `CERT_INVERSION_FREE` is written *only* by
`try_safe_query_rewrite` after all four preconditions pass; the C++ side never
infers it. `CERT_INVERSION_FREE` implies TID by construction. The
**probability result is order-independent**: a wrong variable order only risks
circuit blow-up (caught by the size guard), never a wrong answer. The only
soundness-critical claims are "this is TID" and "skip tree-decomposition"; the
latter at worst wastes time, and the former is backstopped at the builder
(`!hasMulInputs()`). Action is gated on a GUC defaulting **off** in phase 1 for
A/B validation.

### 4. In-process construction: a structured d-DNNF over the query hierarchy – new `src/StructuredDNNF.{h,cpp}`

We emit a d-DNNF regardless (the `paniniCompile` shape, evaluated by
`dDNNF::probabilityEvaluation`), and within the inversion-free class an OBDD is
not asymptotically better — a *linear* OBDD merely serialises independent
products into decision chains. So the builder is **not** a literal OBDD: it
produces a **structured d-DNNF whose vtree is the query hierarchy** (the class
topo order from the cert recipe). Two node kinds, placed by structure:

- **Decision nodes on separator (shared) variables.** Where independent
  components share variables — the consistent-unification self-join: in the
  witness the `y`-part `⋁_y S(a,y)∧A(a,y)` and `z`-part `⋁_z S(a,z)∧B(a,z)`
  share the `S(a,·)` tuples — Shannon-decide the shared variable first. The
  decision restores **determinism** (the two branches are mutually exclusive)
  *and* exposes the decomposition beneath it.
- **Decomposable AND on the residual.** Once the separator variables are fixed,
  the residual components are variable-disjoint (the `A`-part and `B`-part), so
  emit a single AND node and recurse independently — the structural win an OBDD
  cannot express.

`class StructuredDNNFBuilder` (declared `friend` of `dDNNF`). **Code reuse vs.
`dDNNFTreeDecompositionBuilder` is partial, and only at the emission layer.**
That builder also emits a *structured* d-DNNF, so its d-DNNF assembly substrate
is reusable — `dDNNF` node creation (AND/OR), wiring, root-setting,
`makeSmooth` / `simplify`, the `friend`-of-`dDNNF` pattern, and the
gate-id bookkeeping. But its **driver does not transfer**: it is a bag-DP whose
per-bag valuation enumeration is bounded by `MAX_TREEWIDTH+1`, and our target
class has *unbounded* treewidth (that is exactly why the tree-decomposition rung
times out on the witness — OBDD-width and treewidth are incomparable). So the
query hierarchy must **not** be passed to that builder as a tree decomposition;
doing so reproduces the treewidth blow-up the feature exists to avoid. Our
driver is a fresh top-down recursion bounded by the query structure
(separator decisions + decomposition), not by treewidth:

- **TID / leaf guard.** Throw `CircuitException` (fall through to the normal
  chain) if `bc.hasMulInputs()`, or if any `bc` variable lacks an order-key
  marker — backstops on the detector's TID + input-leaf precondition.
- **per-component recursion driven by the hierarchy** (below); the per-input
  marker keys (§2) order variables within a component and locate the next
  separator.
- emits the `paniniCompile` shape `OR(AND(pos,hi), AND(neg,lo))` at decision
  nodes and flat `AND` at decomposition points; finishes with `makeSmooth()`
  then `simplify()` so `probabilityEvaluation()` runs unchanged.
- **size guard** `C·|lineage|` for fixed query, throwing `CircuitException` on
  exceed, to fall back to the normal chain.

```
build(component):
    if component is constant:                       return terminal
    parts = independent_split(component)            # variable-disjoint sub-circuits
    if parts.size() > 1:                            # DECOMPOSABILITY
        return AND( build(p) for p in parts )       # flat product, recurse per part
    v = next_separator_variable(component, recipe)  # shared var, per hierarchy order
    hi = build(component | v=true);  lo = build(component | v=false)
    return OR( AND(v, hi), AND(NOT v, lo) )         # DETERMINISM from the decision
```

**Why this is polynomial without a memo.** The size bound falls out of the
explicit decomposition: each hierarchy level emits a bounded branching, and
there are `O(|lineage|)` components, so the d-DNNF is linear in the lineage for
fixed query. There is **no Shannon-expansion computed-table to key correctly**
— the old `2^g` frontier was an artifact of flattening into a linear order, and
it is replaced here by the structural recursion. (An optional unique-table can
share equal sub-d-DNNFs, but correctness and the size bound do not depend on
it.) The obligation that *was* the memo-key risk relocates to **determinism**:
each `OR` must have mutually exclusive children, which holds because both
branches come from a decision on the same separator variable — see risk #1.

### 5. Integration & guards

- New GUC `provsql.inversion_free` (bool, `PGC_USERSET`,
  `GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE`), modelled on
  `provsql.cmp_probability_evaluation`. **Default off in phase 1.**
- Dispatch block in `probability_evaluate_internal`, in the
  `independentEvaluation` catch before `makeDD`:
  `if (GUC on && method in {"",default} && cert.kind==INVERSION_FREE)`,
  build the structured d-DNNF, `result = dd.probabilityEvaluation()`, mark
  processed; on
  `CircuitException` (TID guard, size guard, malformed cert) fall through to
  the existing chain.
- Probability-path only: semiring evaluators ignore the annotation `extra` as
  inert metadata, and the annotation gate is identity for them.
- **Detection is decoupled from `provsql.boolean_provenance`.** Unlike the
  read-once rewriter — which *must* be gated because it rewrites the query and
  changes the produced circuit — detection only reads the query and attaches
  semiring-transparent markers, so it is not correctness-gated. The remaining
  reason to gate is **cost** (the analysis and the per-input markers are wasted
  if `probability_evaluate` is never called): `boolean_provenance` is a
  serviceable proxy for "this session is in the probability regime", so gating
  on it stays a defensible *performance* default — a knob, not a soundness
  requirement. Decide in phase 1 whether to run always or keep the cost-gate.

## Critical files

- `src/safe_query.c` – `detect_inversion_free` (4 preconditions incl.
  TID/base-independence), `G_prec` + topo sort; relax
  `is_safe_query_candidate` with `approved_inversion_free_relids` conjoined
  with TID; branch `try_safe_query_rewrite` to attach cert + input markers.
- `src/safe_query_cert.h` (new) – `SafeCert` + serialise / parse + `extra`
  discriminator prefixes.
- `src/provsql_utils.{h,c}` – new annotation gate type appended to the
  `provenance_gate` enum (append-only); `get_constants` OID lookup.
- `sql/provsql.common.sql` – annotation-gate creation helper hashing
  `v5(ns, concat('annot', child, extra))`; transparent handling reuses
  `set_extra`.
- `src/GenericCircuit.hpp` – `evaluate<S>` arm: annotation gate → identity over
  child. Likewise transparent arms in `WhereCircuit`, `MonteCarloSampler`,
  `DotCircuit`, `to_prov`, and `getBooleanCircuit` (the `assume_boolean`
  precedent enumerates the switch sites).
- `src/StructuredDNNF.h`, `src/StructuredDNNF.cpp` (new) –
  `StructuredDNNFBuilder` (separator decisions + decomposable AND, driven by the
  query hierarchy) with TID + size guards; add to Makefile object list.
- `src/dDNNF.h` – `friend StructuredDNNFBuilder` declaration.
- `src/probability_evaluate.cpp` – cert parse (read `gc.getExtra(gc_root)`),
  marker-key collection via `gc_to_bc`, dispatch block at ~line 281.
- `src/provsql.c` – new `provsql.inversion_free` GUC; stamp cert + markers
  on the certified path.

## Phased breakdown

1. **Detector (self-contained, reusable).** `detect_inversion_free` (all four
   preconditions), `G_prec` / topo, `safe_query_cert.h`; relax
   `is_safe_query_candidate`; branch `try_safe_query_rewrite`. Cert + markers
   produced but unused. Tests: rejection cases (non-hierarchical, inversion,
   BID, derived atom) + acceptance NOTICE.
2. **Annotation gate + plumbing.** Append the gate type; creation helper with
   `extra`-folded UUID; transparent arms in every gate-type switch; stamp cert
   on root and keys on inputs; parse + log in `probability_evaluate_internal`.
   *Riskiest step* (new persisted gate type; touches the load path).
3. **Structured-d-DNNF builder.** `src/StructuredDNNF.{h,cpp}`; `dDNNF` friend;
   Makefile; separator-decision + decomposable-AND recursion; TID + size guards;
   unit-test on hand-built `BooleanCircuit`s with known probabilities (assert
   determinism at OR nodes and decomposability at AND nodes).
4. **Integration.** GUC (default off); dispatch block with try / catch
   fallback; marker-key collection.
5. **Tests + flip default.** pg_regress suite; cross-checks; flip GUC after
   validation.

## Verification

New `test/sql/safe_query_inversion_free.sql` (+ expected output), registered in
`test/schedule.common` (per CLAUDE.md, never edit `test/schedule`), in the
style of the existing `safe_query_*` tests. **The witness lineage must be built
by the real planner** (actual `s`/`a_rel`/`b_rel` tables + the SQL query), never
a hand-factored circuit via `create_gate` — per the spike's lineage-shape
lesson, a hand-factored circuit is low-treewidth and would silently skip the
hard case the feature exists for.

1. **Witness acceptance + correctness.** Tables `s`, `a_rel`, `b_rel` with
   TID provenance; the `S(x,y),A(x,y),S(x,z),B(x,z)` query. On `n=2,3`
   cross-check default `probability_evaluate(token)` (structured-d-DNNF path)
   against
   `monte_carlo` (large sample, tolerance) and `possible_worlds` (exact, pin to
   high precision).
2. **Rejection.** Symmetric closure `R(x,y),R(y,x)`, path `R(x,y),R(y,z)`, a
   **BID** atom, and a **derived/view** atom: assert no cert attached
   (diagnostic NOTICE at `verbose_level>=5`, mirroring the CountCmp NOTICE), and
   confirm they still evaluate correctly via the fallback.
3. **Read-once regression.** A plain hierarchical self-join-free query yields
   identical probability (independent fires first); no regression of existing
   `safe_query_*` numbers. A hierarchical **BID** query still routes through the
   read-once path (the inversion-free branch must not steal it).
4. **Scaling smoke.** The `n×n` witness at `n=6` (108 leaves): completing where
   the `d4` path times out is the signal; cross-check the value against
   monte-carlo (or the complete-relation closed form).
5. **GUC off.** Witness still correct via the (slower) fallback.
6. **Semiring transparency.** Run a non-Boolean semiring (`sr_counting`,
   `sr_why`) on a certified query and confirm the annotation markers are inert
   (same result as with detection off) — the decoupling-from-`boolean_provenance`
   guarantee.
7. **Cert robustness.** Malformed cert / stray mulinput produces graceful
   fallback (C++ unit test if the harness supports it; otherwise a manual
   check).

Build / run loop (per `CLAUDE.local.md`):
`make -j$(nproc)` → `sudo make -C /home/pierre/git/software/provsql install`
→ `sudo pg_ctlcluster 18 main restart` → `make installcheck`.

## Riskiest / most uncertain design points

1. **Determinism + decomposition correctness (Part 4).** The structured-d-DNNF
   construction replaces the OBDD's atom-frontier memo with explicit structural
   recursion, so the *polynomial-size* worry is largely dissolved (size is
   `O(|lineage|)` by construction; the size guard still backstops). The
   proof-of-concept spike (see *Validation*) already confirms the bounded-
   frontier recursion is correct and linear on the witness — but it computed the
   probability *inline*; the residual risk is the *materialised* d-DNNF
   preserving determinism/decomposability node-by-node. The
   correctness obligation it inherits is twofold: every `AND` must be genuinely
   **decomposable** (children variable-disjoint — i.e. `independent_split` is
   sound, never grouping correlated parts), and every `OR` must be genuinely
   **deterministic** (children mutually exclusive — guaranteed only if the
   separator decision is on a variable shared by exactly the components being
   split). A bug here is *silently wrong probability*, not merely slow — the
   opposite failure mode from the old memo key, and the more dangerous one. Unit
   tests must assert decomposability and determinism node-by-node against
   independently computed probabilities; the `n×n` witness micro-benchmark stays
   as the linear-scaling check.
2. **Annotation gate type (Part 2/3).** A new persisted gate type is the
   invasive part: it must be appended to the `provenance_gate` enum
   (append-only; the upgrade script must `SELECT reset_constants_cache()` at
   release time), given a transparent arm in *every* gate-type switch, and its
   `extra`-folded UUID derivation must be the *only* place that departs from the
   children-only content-addressing convention. Forced by the fact that a
   children-only token cannot carry per-occurrence / per-query `extra`.
3. **Per-input key construction (Part 2).** The key must be a single value per
   Boolean variable (role-independent, column-based) so content-addressing
   merges a variable's markers into one. The witness shows such a key exists
   (free positions are single physical columns); the obligation is constructing
   it so it is globally comparable across *different* relations (root value
   shared; free-column slots laid out consistently) — i.e. that it is a total
   order. This is intertwined with risk #4.
4. **Inversion-definition completeness.** The four-part test is a sound
   under-approximation, not the full Jha–Suciu characterisation; it admits the
   witness and rejects the named bad shapes but may reject richer inversion-free
   queries. Document as such; verify against the paper that the
   column-determined key is a Prop. 4.5-consistent total order over the whole
   certified class before claiming "exactly `UCQ(OBDD)`".
