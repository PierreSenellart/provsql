# Safe-Query Optimisation for ProvSQL — Implementation Plan

## 0. Implementation status (branch `safe_queries`)

**Landed**: GUC `provsql.boolean_provenance`; per-table metadata in the
mmap store with TID / BID / OPAQUE kind plus block-key columns; IPC +
SQL bindings + relcache-invalidated per-backend cache; planner-hook
integration; hierarchy detector (`find_hierarchical_root_atoms`) and
single-level rewriter (`rewrite_hierarchical_cq`) in
`src/safe_query.{c,h}` (extracted from `src/provsql.c`).

The rewriter currently accepts:

- Self-join-free hierarchical CQs with column pushdown for every
  fully-covered shared class (root + extras).
- Atom-local WHERE-qual pushdown (slice 3b): single-atom conjuncts
  pushed into the inner `SELECT DISTINCT` wrap before the union-find.
- Multi-level rewrite for partial-coverage shared classes (slice 3c):
  one big inner sub-Query when there's an outer atom; Choice A
  re-entry handles nesting.
- Multi-level + multi-fully-covered class: the inner sub-Query exposes
  every fully-covered class, not just the root.
- Disjoint multi-group: when every atom is touched by some partial-
  coverage class and the signatures partition cleanly (no bridges),
  one inner sub-Query per distinct signature, joined at the outer
  on the fully-covered classes.
- Single-atom head Vars on any atom position (outer-wrap, first-
  member grouped, non-first-member grouped); the new
  `safe_proj_slot.outer_attno` decouples the logical inner column
  from the in-list position.
- BID block-key alignment: BID atoms whose `block_key` columns are
  not all in the projection slots are refused (empty block_key
  refused unconditionally).
- Multi-component CQs: disconnected FROM (`q :- A(x), B(y)`) split
  into one inner sub-Query per connected component, Cartesian-
  product at the outer; covers both Var-carrying and all-constant
  targetLists (a synthetic `Const(1)` anchor folds Var-less
  components to one row each).
- UCQ via top-level UNION ALL and branch-disjoint UNION (DISTINCT) —
  no extra code needed; recursion into branches plus per-row
  `independent` evaluation suffices.

The FD-aware extensions (see the *Safe-Query Rewriter* dev-doc
section in `doc/source/dev/query-rewriting.rst` and inline comments
in `src/safe_query.c`) layer additional acceptance on top of the
base detector:

- Constant-selection elimination (`apply_constant_selection_fd_pass`):
  pre-pass that propagates `Var = Const` literals through the
  equijoin-closure union-find and drops the redundant equijoin
  conjuncts, so constant-pinned atoms route through the
  multi-component path and factor out at the top.
- Primary-key / NOT-NULL UNIQUE FDs: a separate per-backend cache
  (`provsql_lookup_relation_keys` in `src/provsql_utils.c`) scans
  `pg_constraint`, joins `pg_index`, and verifies NOT-NULL on every
  UNIQUE column.  The detector applies each FD once and tags
  non-key columns' classes as FD-determined inside the relation.
- Deterministic-relation transparency: relations with no `provsql`
  column and no metadata are tagged FD-determined on every class,
  modelling Gatterbauer & Suciu 2015's dissociation argument.
  Soundness guards on `pg_class.relkind = 'r'` and
  `has_superclass = false`.
- FD-aware atom-set hierarchicality (`fd_aware_mode`): when no
  single class touches every atom under the raw count but the
  FD-reduced atom-sets are pairwise nested-or-disjoint, the
  detector emits a per-atom local-root anchor (with a fallback for
  atoms whose every anchored class is FD-determined) and the
  rewriter produces a multi-anchor shape.
- PK-unifiable self-joins (`try_pk_self_join_unification`): a
  pre-pass before `is_safe_query_candidate` that collapses
  same-relid RTE groups whose PK / NOT-NULL UNIQUE columns are all
  equated through the union-find closure into a single survivor,
  renumbering Vars and RangeTblRefs through `safe_unify_remap_mutator`.
- Disjoint-constant self-joins (`try_disjoint_constant_self_join_split`):
  certifies same-relid groups whose `Var = Const` predicates on the
  same column have provably distinct literals (via `datumIsEqual`)
  in a `Bitmapset`; `is_safe_query_candidate` consults the set and
  skips the shared-relid bail for those relids.

**Pinned bails** (regression-tested as deferred):

- The "bridge case" — a partial-coverage class spanning multiple
  signature groups in a hierarchical CQ (test (22) in
  `safe_query_pushdown.sql`).
- Branch-overlapping UNION (DISTINCT) where atoms are shared across
  branches (test (21)).

Further follow-ups (FD-induced nested rewrite, soft keys, view
descent for FD chases, data-safe plans, self-joins without PK or
constant rescue) are tracked in
[`safe-query-followups.md`](safe-query-followups.md) under
"Hierarchical-detector follow-ups".

Regression suite is 150 tests green; the new files
`safe_query_const_sel.sql`, `safe_query_pk_fd.sql`,
`safe_query_deterministic.sql`, `safe_query_self_join_pk.sql`,
`safe_query_fd_closure.sql`, and `safe_query_self_join_disjoint.sql`
cover the FD-aware paths.

## 1. Goal

For a restricted class of safe conjunctive queries, produce provenance
circuits whose probability is computable in linear time via the existing
(or a refined version of) `BooleanCircuit::independentEvaluation`
method, without invoking tree-decomposition or external knowledge
compilers. The optimisation operates by rewriting the input query to an
equivalent SQL form whose natural ProvSQL processing yields a read-once
circuit.

## 2. Scope

### In scope

- Self-join-free hierarchical conjunctive queries.
- TID (default `add_provenance`) and BID (`repair_key`) base tables.
- Independent-UCQs (unions of in-scope CQs whose branches share no
  relation symbols), via a top-level UNION.
- Semirings that factor through the Boolean semiring — i.e., the
  absorptive semirings (`a + a·b = a`). Idempotency alone is not
  sufficient (why-provenance is idempotent but not absorptive).
- Global / existential interrogations of the result. The rewrite
  changes the multiset structure of the output table; per-row
  provenance interrogations are not preserved even when the semiring
  is.

### Explicitly out of scope (this plan)

- Möbius-requiring safe UCQs (`q₉` and similar). Handling these would
  need Monet's fragmentation construction or a separate extensional
  pipeline; both are larger projects with their own risk profile.
- Self-joins. The hierarchical analysis with self-joins is more
  involved and unaddressed here.
- Inversion-free non-hierarchical UCQs. These would require an OBDD
  construction beyond the read-once factoring described below.
- Non-absorptive semirings (Counting `N`, How-provenance `N[X]`).
  These are not preserved by the rewrite, and the implementation must
  refuse evaluation rather than return silently wrong answers.
- Per-row provenance interrogations (per-row why-provenance,
  where-provenance, per-row how-provenance). The rewritten query
  returns a different multiset of rows, so these are no longer
  meaningful even for absorptive semirings.
- Aggregation queries (`SUM`, `COUNT`, etc.). The rewrite is
  multiset-changing and therefore unsound for queries whose result
  depends on multiplicities.

### Things deferred but not foreclosed

- `UPDATE`-provenance interaction.
- Recognising hierarchical structure in queries that need light
  normalisation (e.g., predicate pushdown) to expose it.
- A textual EXPLAIN-style hook showing which queries the optimisation
  fired on.

## 3. Theoretical basis

A self-join-free CQ is safe iff hierarchical (Dalvi–Suciu). For
hierarchical CQs the safe plan involves only separator-variable
factoring and independent-component decomposition — no Möbius
inversion. The corresponding lineage admits a read-once factoring in
which every input token appears exactly once. ProvSQL's
`independentEvaluation` evaluates such circuits exactly in one linear
pass: it computes `1 − ∏(1 − pᵢ)` for `plus` gates whose children have
disjoint variable sets, and `∏ pᵢ` for `times` gates.

The rewrite preserves the Boolean polynomial of the lineage of the
existential ("does the query have any answer?") via distributivity and
absorptivity. Therefore any semiring whose evaluation factors through
the Boolean semiring agrees on this global question before and after
the rewrite. The rewrite does *not* preserve the multiset of output
rows; per-row interrogations are different questions after rewriting.

## 4. Components

### 4.1 GUC: `provsql.boolean_provenance`

- New session-level GUC, default `off`.
- When `on`, the rewriter is permitted to apply transformations sound
  only under Boolean-equivalent semantics.
- The GUC is *necessary* but not sufficient — see the per-circuit tag
  in §4.5.

### 4.2 Per-table metadata in the mmap store

A new mmap-backed structure keyed by `Oid`:

```
struct ProvenanceTableInfo {
    bool      tid;          // true iff provenance tokens are independent leaves
    uint16_t  block_key_n;  // 0 when not a BID; otherwise number of block-key columns
    AttrNumber block_key[]; // block-key column numbers (length block_key_n)
};
```

Lives next to the gate store; uses the same `MMappedUUIDHashTable` /
`MMappedVector` conventions. Bumps the on-disk mmap ABI (handled via
the existing `provsql_migrate_mmap` mechanism).

**Maintenance points** (write sites):

| Event                                           | Effect                                           |
|-------------------------------------------------|--------------------------------------------------|
| `add_provenance(R)`                             | insert `{R → {tid=true, block_key=∅}}`           |
| `remove_provenance(R)`                          | delete entry                                     |
| `repair_key(R, cols)`                           | set `tid=false, block_key=cols`                  |
| `CREATE TABLE T AS SELECT … FROM <prov source>` | insert `{T → {tid=false, block_key=∅}}`          |
| `INSERT INTO T SELECT … FROM <prov source>`     | set `T.tid=false`                                |
| `UPDATE` under `provsql.update_provenance`      | set target's `tid=false` on first such update    |
| `DROP TABLE`                                    | delete entry (via `sql_drop` event trigger or   |
|                                                 | piggyback on the gate-cleanup the worker does)   |

`set_prob` is *not* a write site — it attaches probabilities to
existing UUIDs and does not introduce correlation.

**Per-backend cache:** the OID cache (`constants_t`) is extended with a
small per-relation entry, invalidated on the standard PostgreSQL
relation-cache invalidation path. Read-mostly steady state ⇒ O(1)
lookup at rewriting time. May consider forcing this to stay in-cache.

### 4.3 Hierarchy detector

A new early pass in `process_query`, slotted between step 1 (CTE
inlining) and step 5 (`get_provenance_attributes`). All of the
following must hold for the rewrite to fire; failure of any one bails
silently to the existing pipeline.

1. `provsql.boolean_provenance` is on.
2. The query is a self-join-free CQ. Concretely: no `RangeTblEntry`
   references the same `relid` twice; no aggregation; no window
   functions; no `DISTINCT ON`; no `LIMIT`/`OFFSET`; no set operations
   other than top-level `UNION` over branches with disjoint relation
   symbols. (Plain `DISTINCT` at the top level is allowed and is
   effectively a no-op for the rewrite.)
3. Every base relation referenced in the FROM list has an entry in the
   mmap metadata, and either `tid=true` or `tid=false` with a
   `block_key` that includes the separator variable's binding columns.
4. The atom set is hierarchical: there exists a variable `s` appearing
   in every atom (or, for the UNION case, in every atom of each
   branch). For multi-level hierarchies, the property must hold
   recursively on the residual queries after removing `s`.

Detection cost is linear in the number of atoms × number of variables,
and bails early. No SPI calls in the hot path beyond the metadata
cache lookup.

### 4.4 Query rewriter (pure `Query` → `Query`)

For a hierarchical CQ with separator `s`:

1. Partition the atoms by whether `s` is their only shared variable
   with the rest of the query.
2. For each atom `A(s, t₁, …, tₖ)` whose `tᵢ` are existentially bound
   below `s` (i.e., not shared with other atoms), wrap it in a
   subquery `SELECT DISTINCT <s-binding> FROM A`. Predicates on `A`'s
   non-`s` columns push into the subquery.
3. Recursively apply the rewrite to the inner subqueries when they
   themselves contain hierarchical structure (multi-level case).
4. Reassemble the FROM list with the wrapped subqueries and rebuild the
   join predicates on the `s`-binding columns.

For BID tables: the separator must include the block-key columns;
otherwise the rewrite cannot exploit BID structure and the pass bails.
When it fires, the existing `mulinput` gates inside the block survive
through the rewrite — the resulting circuit has `gate_plus` over
`mulinput` children, which `independentEvaluation` handles natively.
`rewriteMultivaluedGates` does not need to run on this path.

For independent UCQs: the top-level `UNION` is preserved; each branch
is rewritten independently. The branches share no relation symbols by
the precondition, so the outer plus is over disjoint variable sets.

The output of this pass is a `Query` node. It is fed back into
`process_query` from step 2 onward, using the existing provenance
rewriter unchanged.

### 4.5 Circuit-level tag

The root gate emitted under this path is tagged "produced under
Boolean-equivalence rewrite." Mechanism options:

- A single bit in the gate metadata in mmap (cheap; requires ABI
  consideration).
- A side table in the mmap store mapping root UUID → tag (no ABI
  impact, marginally more lookup cost).

At evaluation time:

- `probability_evaluate`, `shapley`, `banzhaf`: tag is ignored
  (work with Boolean expressions).
- Compiled semirings for which no homomorphism from Boolean functions
  exists: refuse with a clear error when the tag is set, naming the
  GUC as the cause.
- Custom semirings via `provenance_evaluate`: the PL/pgSQL fallback
  has no `assumed_boolean` arm, so calling it on a tagged circuit
  errors with the generic "use compiled semirings instead" message.
  (The original intent was a one-shot warning that lets the user
  proceed under their own responsibility; that path is not
  implemented and a real warning hook would need to live closer to
  the dispatcher.)

The tag is the safety mechanism for the user-facing promise. The GUC
on its own would let a user toggle Boolean mode, run a query, toggle
it back off, and then evaluate a non-absorptive semiring on the
resulting circuit with no signal that something is wrong. The tag
prevents that.

## 5. Soundness conditions, restated

The rewrite is sound for a query `Q` and semiring `S` when **all** of:

1. `Q` is in the scope of §2.
2. All referenced tables have correct metadata in the mmap store
   (TID or BID-with-aligned-block-key).
3. The user is asking a global / existential question over the result.
4. `S` is absorptive, i.e., admits a homomorphism from the Boolean
   semiring.

The implementation enforces (1) and (2) via the detector, (4) via the
tag-check at evaluator dispatch. (3) cannot be enforced
mechanically — the user has to ask the right question. Documenting it
clearly is the best we can do.

## 6. Implementation footprint, summary

| Component                                  | Where                          | Approx. size            |
|--------------------------------------------|--------------------------------|-------------------------|
| GUC registration                           | `_PG_init`, GUC table          | trivial                 |
| Per-table mmap structure                   | new file, alongside gate store | small                   |
| Mmap write hooks                           | existing trigger functions     | small per site          |
| Cleanup event trigger                      | new SQL                        | trivial                 |
| Hierarchy detector                         | new step in `process_query`    | moderate                |
| Query→Query rewriter                       | new pass in `process_query`    | moderate                |
| Root-gate tag + evaluator checks           | `BooleanCircuit`, dispatch     | small                   |
| Documentation + tests                      | docs/, test/                   | non-trivial             |

No changes to `BooleanCircuit`'s evaluation methods, the dDNNF code,
the tree-decomposition code, or the external-tool integration.

## 7. Verification and testing

### 7.1 Correctness checks that should be implemented

- **Reference equivalence.** For every query in a new regression test
  set, compute the probability via the rewritten path and via
  `possible-worlds` on the original query at small scale, and compare.
- **Tag enforcement.** A test that toggles the GUC on, runs a
  hierarchical query, toggles it off, and verifies that
  non-absorptive-semiring evaluations on the resulting tokens fail
  with the expected error.
- **GUC-off non-regression.** The entire existing test suite must pass
  unchanged with the GUC off.

## 8. Non-goals

- This is not a replacement for the existing fallback chain. The
  chain (`independent` → `interpretAsDD` → `tree-decomposition` →
  `compilation`) remains the default for everything not in scope.
- This is not a path toward handling all safe queries. It addresses
  the hierarchical subclass cleanly; the Möbius-requiring class is a
  separate, harder problem.
- This is not an optimisation that fires automatically for all users.
  It is gated on an explicit opt-in (the GUC), because the multiset
  semantics change is observable.
