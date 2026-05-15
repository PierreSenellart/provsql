# Safe-Query Optimisation for ProvSQL — Implementation Plan

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
- Compiled semirings for which no homomorphsim from Boolean functions
  exist: refuse
  with a clear error when the tag is set, naming the GUC as the cause.
- Custom semirings via `provenance_evaluate`: cannot be statically
  classified; emit a warning when the tag is set, and rely on the user
  to know what they are doing.

The tag is the safety mechanism for the user-facing promise. The GUC
on its own would let a user toggle Boolean mode, run a query, toggle
it back off, and then evaluate a non-absorptive semiring on the
resulting circuit with no signal that something is wrong. The tag
prevents that. (Or add to the custom semiring interface a Boolean)

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

### 7.2 Empirical questions we have not answered

- Whether `independentEvaluation` succeeds on every rewritten circuit
  in the in-scope class without throwing `CircuitException`. The
  expectation is yes by construction, but the construction goes
  through ProvSQL's existing aggregate-DISTINCT rewriter, whose
  detailed gate emission has not been audited for this purpose.
- Whether the BID path actually evaluates correctly without
  `rewriteMultivaluedGates` running. The dev docs state that
  `independentEvaluation` handles `mulinput` gates natively; this
  needs to be confirmed against the code and exercised by tests.
- Performance characterisation. We expect a large win on
  hierarchical-CQ workloads where the current default chain falls all
  the way through to `d4` — but the workload distribution where this
  actually matters in practice is not characterised. A benchmark
  comparing the new path against `tree-decomposition` and
  `compilation` on hierarchical CQs over realistic data should be part
  of v1.

### 7.3 Semiring inventory to audit

A parallel effore in the provenance-lean/ repository classifies semirings
according to existence of homomorphisms.

These classifications should be verified case by case rather than
trusted from this document.

## 8. Open design questions

- **Where exactly to slot the detector and rewriter in
  `process_query`.** Currently described as "between step 1 and step
  5." The interaction with the existing aggregate-DISTINCT rewrite
  (step 4) and unsupported-feature checks (step 6) needs to be
  inspected.
- **Tag storage mechanism.** Per-gate bit vs. side table. The bit is
  cheaper at lookup but costs an ABI bump.
- **Recursive rewrite of multi-level hierarchies.** The single-level
  case is straightforward. The recursive case needs care to handle
  variable scoping in the generated Query nodes correctly; this is the
  most likely source of implementation bugs.
- **Catalog-cleanup story when the `sql_drop` event trigger fires for
  a table whose gates are still referenced by other tables' tokens.**
  The current ProvSQL behaviour in this case has not been audited as
  part of this plan; the new metadata cleanup needs to be consistent
  with whatever ProvSQL already does (or doesn't) for the gate store.

## 9. Non-goals

- This is not a replacement for the existing fallback chain. The
  chain (`independent` → `interpretAsDD` → `tree-decomposition` →
  `compilation`) remains the default for everything not in scope.
- This is not a path toward handling all safe queries. It addresses
  the hierarchical subclass cleanly; the Möbius-requiring class is a
  separate, harder problem.
- This is not an optimisation that fires automatically for all users.
  It is gated on an explicit opt-in (the GUC), because the multiset
  semantics change is observable.
