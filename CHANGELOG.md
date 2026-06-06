# Changelog

All notable changes to [ProvSQL](https://provsql.org/) are documented
in this file.  It mirrors the release-notes section of the website
([provsql.org/releases](https://provsql.org/releases/)) and is kept in
sync by the `release.sh` release-automation script.

## [1.9.0] - 2026-06-06

ProvSQL 1.9.0 brings broad support for subqueries outside FROM and for
outer joins, a redesigned probability engine built around a method
catalog with a cost-based chooser and user-requested guarantees, exact
probability evaluation for a wide range of HAVING aggregate
comparisons, SQL-faithful empty-group and NULL aggregation semantics,
native arithmetic on aggregation results, and a WebAssembly build that
powers the in-browser ProvSQL Playground.

#### Subqueries and outer joins

- **Subqueries outside FROM.** `EXISTS`/`NOT EXISTS`, `IN`/`NOT IN`,
  quantified comparisons (`op ANY` / `op ALL`, row `IN`), scalar
  subqueries, and `ARRAY(SELECT ...)`, correlated or not, are now
  internally decorrelated and rewritten with the proper (m-)semiring
  provenance: value bodies through `choose()` with an
  at-most-one-row gate, aggregate bodies (`count`, `sum`, `avg`,
  `min`, `max`) through the aggregate over the outer-join group,
  semijoins and antijoins through count-predicate lowerings (so
  `NOT IN` carries the same antijoin provenance as the equivalent
  `EXCEPT`). The subquery body may involve a single
  provenance-tracked relation or join several as a comma-separated
  FROM list; `SELECT DISTINCT` bodies, `ORDER BY ... LIMIT 1`
  (argmax) bodies, several coalesced subqueries, aggregate bodies
  compared against outer columns, and untracked outer FROMs are all
  handled. Bodies touching no tracked relation pass through to
  PostgreSQL untouched.
- **Outer joins.** `LEFT`, `RIGHT` and `FULL` joins are lowered to
  their matched and null-padded antijoin arms, capturing the
  non-monotone 0-match world; `EXCEPT` / `EXCEPT ALL` provenance is
  corrected to NOT-IN semantics.
- **SQL-faithful aggregate NULL handling.** `count(expr)`, `sum`,
  `min`, `max` and `avg` now ignore NULL inputs as SQL requires;
  `count(*)` still counts rows.

#### Probability evaluation

- **Method catalog and cost chooser.** Probability-method dispatch is
  rebuilt as a catalog of strategies under a unified cost-based
  chooser (a lazy uniform-cost search over feature-acquisition and
  method-execution costs, with calibrated constants and speculative
  execution that budgets a candidate at the next-best method's cost
  and escalates on overrun).
- **Guarantee-first evaluation.** Request `exact`, relative
  `(eps, delta)`, or additive `(eps, delta)` and let the chooser pick
  the cheapest admissible method; every approximate method shares one
  `key=value` argument grammar and emits a machine-readable
  approximation-guarantee NOTICE. The new `provsql.last_eval_method`
  GUC reports what actually ran, down to the resolved external tool
  (`compilation:d4`, `wmc:ganak`).
- **New methods.** `karp-luby` (the DNF FPRAS, with the
  Dagum-Karp-Luby-Ross self-adjusting stopping rule and stratified
  sampling), `stopping-rule` (whole-circuit relative-error FPRAS),
  `sieve` (exact inclusion-exclusion over monotone DNFs), and
  `d-tree` (anytime probability interval bounds after
  Olteanu-Huang-Koch, on arbitrary circuits, with the new
  `probability_bounds` SQL function).
- **Exact HAVING aggregate probabilities.** Closed-form and exact
  evaluators for `HAVING` comparisons over `COUNT`, `SUM`, `MIN`,
  `MAX` and `AVG`: safe (hierarchical) joins at arbitrary depth,
  cross-product joins, `repair_key` BID blocks, branch-spanning sums
  over UNION/EXCEPT contributors, numeric/float aggregates via
  decimal scale-to-integer, constant arithmetic folded into the
  threshold, and comparisons combining several aggregates resolved by
  possible-worlds enumeration. `COUNT(DISTINCT ...)` in HAVING is fixed, and aggregates that no
  exact arm covers fall back to an approximation-safe sampling route
  rather than erroring.

#### Aggregation semantics and arithmetic

- **Empty groups, SQL-faithfully.** Scalar (ungrouped) aggregations
  are now tagged in the circuit so true-on-empty predicates
  (`count(*) = 0`, `sum(x) IS NULL`…) evaluate correctly in every
  possible world; `count(col)` with NULLs, `HAVING ... IS [NOT] NULL`
  on groups with NULL-valued rows, and moments/support of `agg_token`
  `min`/`max` (conditioned on a non-empty group, so they stay finite)
  are all handled.
- **Arithmetic on aggregation results.** `agg_token` gains native
  `+ - * /`, unary `-`, and aggregate-vs-aggregate comparisons,
  building `gate_arith` circuits that carry the computed value (so
  query results still display `value (*)`), with comparisons over the
  results evaluated exactly where possible.

#### SQL surface and usability

- `add_provenance` and `create_provenance_mapping` are idempotent
  (NOTICE-and-return when already done).
- New `setup_search_path()` helper plus an installation-time advisory
  when `provsql` is missing from the default `search_path`; the
  `random_variable` to `uuid` cast is demoted to ASSIGNMENT so it no
  longer shadows operator resolution.
- The SQL `probability_benchmark` helper (added in 1.7.0) is removed;
  ProvSQL Studio's per-tool-timeout probability benchmark supersedes
  it.
- Sessions that run `CREATE EXTENSION provsql` themselves no longer
  end up with a stale constants cache that silently disabled the
  subquery rewrites until reconnect.
- `provenance()` inside a subquery expression now raises a clear
  error, and hand-made `provsql` columns are rejected instead of
  crashing.

#### ProvSQL in the browser

- The extension compiles to WebAssembly and runs inside
  [PGlite](https://pglite.dev/) entirely client-side: a single-process
  circuit store (`PROVSQL_INPROCESS_STORE`) replaces the background
  worker and shared memory, and subprocess-free builds
  (`PROVSQL_NO_SUBPROCESS`) fall back to the in-process compiler. This
  powers the **ProvSQL Playground**
  ([provsql.org/playground](https://provsql.org/playground/)): the
  full ProvSQL Studio, including its new notebook mode, with no
  installation. Reproducible build under `wasm/`, driven by
  `make wasm` / `make playground`.

#### Docker image

- The demonstration image now bundles the ganak and sharpsat-td
  weighted model counters and ships the tutorial and case-study
  databases pre-seeded (switchable from Studio's connection chip).

#### Robustness

- Fixed a heap overflow in the where-provenance column map (the
  long-standing flaky macOS crash), a backend segfault on NULL
  provenance-mapping values, and pathological-circuit recursion now
  errors cleanly via stack-depth guards instead of crashing the
  backend.

## [1.8.0] - 2026-05-27

ProvSQL 1.8.0 expands the knowledge-compilation backend with an
inversion-free compilation path and warm compiler servers, makes the
external-tool set fully data-driven through a registry, and fixes
several correctness and robustness issues.

#### Knowledge compilation

- **Inversion-free compilation.** ProvSQL now recognises the
  inversion-free class of queries (Jha & Suciu, ICDT 2011) and compiles
  their provenance directly to a structured d-DNNF in linear time,
  bypassing a general knowledge compiler for probability evaluation. The
  certified class covers non-integer order keys, deterministic
  (non-tracked) relations, and SPJ / nested-view flattening. It is
  exposed as the `inversion-free` probability method, taken automatically
  by the default method once the root is certified, and surfaced by the
  new SQL `annotate(uuid, text)` / `inversion_free_key(text, text, int)`
  and a transparent `annotation` provenance gate carrying the
  tractability certificate.

- **Warm knowledge-compiler servers (KCMCP).** External compilers and
  model counters can now run as long-lived server processes that ProvSQL
  reaches over a socket, avoiding per-call process startup. A tool
  registered with `kind = 'kcmcp'` is reached at a fixed `unix:` /
  `host:port` endpoint, or ProvSQL launches and supervises the server
  itself (`managed` mode) through a background worker and the new
  `provsql.kcmcp_server` setting. The framed wire protocol (compile and
  weighted model counting over DIMACS CNF, with cancellation, progress,
  and version / compression negotiation) is documented in the developer
  manual; `tdkc --kcmcp` is a reference server.

- **Automatic compiler selection.** `compilation()` can be called with no
  argument and picks the highest-preference compiler currently available
  on the backend; the compile fallback and the no-argument weighted
  model-counting path select tools the same way. d4 (d4v2) can compile
  directly from ProvSQL's native circuit (BC-S1.2) rather than a Tseytin
  CNF.

- **Stronger circuit simplification.** The on-load identity folding
  (`provsql.simplify_on_load`) now folds Boolean absorption and semiring
  identities to a joint fixpoint, alternating the two until the circuit
  stops changing rather than collapsing once at the end. This exposes
  absorptions that surface only after a single-wire collapse, so
  cross-product / high-degree self-join lineages (e.g. `SELECT DISTINCT 1
  FROM e a, e b`) whose sum-of-products would otherwise be exponential
  collapse to a single OR, making them tractable to evaluate.

#### External-tool registry

- **Registry.** External compilers, model counters and KCMCP servers are
  managed through a first-class registry exposed as the `provsql.tools`
  view, with superuser-only mutators `register_tool`, `unregister_tool`,
  `set_tool_enabled` and `set_tool_preference`. A tool is fully
  data-driven (operations, input/output formats, command template),
  so the probability dispatchers carry no per-tool code. Registrations
  persist in a `pg_dump`-carried configuration table and survive
  dump/restore.

- **Security.** `provsql.tool_search_path` is now superuser-only
  (`PGC_SUSET`): since it dictates where the server's OS user looks for
  tool executables, a non-superuser setting it would amount to arbitrary
  code execution as the server account.

#### Bug fixes

- **`possible-worlds` overflow.** The method used a 32-bit shift
  (`1 << n`) when enumerating worlds, which overflowed and returned a
  wrong probability for circuits with 32 or more inputs; the shift is now
  64-bit.

- **`WITH RECURSIVE`.** Fix a planner-hook crash on a recursive query
  when `provsql.active` is off, and an OID error when a recursive CTE is
  referenced in two `UNION` arms.

- **Where-provenance with NULLs.** Fix where-provenance on relations with
  NULL-valued columns, and give a clearer error for untraceable input
  gates.

- **Cancellation / timeout messages.** An interrupted probability
  computation now surfaces PostgreSQL's native cancellation / statement
  timeout message instead of a generic “Interrupted”.

#### Studio

- **Studio 1.4.0.** The companion ProvSQL Studio release adds a Tools
  panel managing the new registry, drives every tool picker from
  `provsql.tools`, and renders the inversion-free certificate as an
  **IF** badge.

## [1.7.1] - 2026-05-24

- Fix a bug where a backend that first planned a query before the
  provsql extension was fully available (e.g. a pooled connection
  predating `CREATE EXTENSION`, or one active during `ALTER EXTENSION …
  UPDATE`) could permanently stop tracking provenance, silently returning
  results without the provsql column for the rest of the session. Failed
  constant-cache lookups are no longer memoized, so affected backends
  recover automatically.

## [1.7.0] - 2026-05-24

Major release headlining two additions: a **knowledge-compilation
surface** that opens up ProvSQL's probability backend to a wide
range of external compilers and model counters with full
introspection of every intermediate artifact, and **provenance for
recursive queries** (`WITH RECURSIVE`) lowered transparently to a
provenance fixpoint on PostgreSQL 15+.

This is a pure SQL-surface release on the upgrade path: no new gate
types (the persisted `provenance_gate` enum is unchanged), no mmap
circuit-format change, and no in-place migration of existing tracked
relations. `ALTER EXTENSION provsql UPDATE` from 1.6.0 only installs
the new functions and replaces the body of the `circuit_subgraph`
introspection helper.

#### Knowledge compilation

A new family of functions exposes every stage of the pipeline behind
`probability_evaluate`, so a query's Boolean provenance can be
compiled, inspected, and round-tripped against external tools:

- **CNF / d-DNNF introspection.** `tseytin_cnf(token)` returns the
  exact DIMACS CNF (Tseytin transformation) the extension feeds to
  external counters, with optional weight lines and `c input`
  comments; `tseytin_cnf_mapping` / `tseytin_cnf_mapping_json` map
  each DIMACS variable back to its provenance input.
  `compile_to_ddnnf(token, compiler)` returns the compiled d-DNNF in
  the c2d / d4 `.nnf` interchange format (same variable numbering as
  the CNF), `compile_to_ddnnf_dot` renders it as GraphViz DOT, and
  `ddnnf_stats` reports its structural statistics (node / edge / gate
  counts, smoothness, depth, treewidth, compile time) as `jsonb`.
- **Tree-decomposition introspection.** `tree_decomposition_dot(token)`
  renders the min-fill decomposition used by the in-process compiler,
  annotated with its treewidth.
- **More compilers and counting backends.** New external d-DNNF
  compilers `d4v2` and **Panini** (KCBox) with target languages OBDD,
  OBDD[AND], and Decision-DNNF (R2-D2 / CCDD were evaluated and
  dropped: their kernelize nodes break d-DNNF decomposability). New
  weighted-model-counting backends **Ganak**, **SharpSAT-TD**, and
  **DPMC** join `weightmc` under the `wmc` umbrella. In-process
  production routes `tree-decomposition`, `interpret-as-dd`, and
  `default` round out the choices.
- **`provsql.fallback_compiler` GUC.** Selects the compiler `makeDD`
  falls back to after `interpretAsDD` and tree-decomposition both
  decline (default `d4`).
- **`tool_available(name)`.** Reports whether an external tool
  resolves on the backend's PATH, honouring `provsql.tool_search_path`
  and using the same resolver the compilers and counters consult, so
  the answer matches what a subsequent `probability_evaluate` would
  see.
- **`probability_benchmark(token)`.** Times every probability-
  evaluation method on one circuit (independent, possible-worlds,
  tree-decomposition, Monte-Carlo, each compiler, each WMC backend),
  capturing per-method errors so the comparison table is always
  complete.
- **Backend hardening.** The compiled d-DNNF is simplified after
  external compilation, gates are iterated in a deterministic order,
  WMC result-line parsing scans right-to-left to tolerate trailing
  diagnostics, and `circuit_subgraph` now reports the longest-path
  (canonical circuit) depth rather than the shortest-path distance.

#### Recursive queries (`WITH RECURSIVE`)

The planner hook now lowers a recursive CTE whose body touches
provenance-tracked relations into a naive bottom-up provenance
fixpoint (the `eval_recursive` driver), evaluated through ProvSQL's
normal rewriting so the recursive join yields `times`, the untracked
base yields `gate_one`, and the `UNION` yields the `plus` merge of
alternative derivations.

- On **acyclic** input the structural fixpoint is reached and the
  circuit is the universal provenance, sound for any semiring.
- On **cyclic** input under `provsql.boolean_provenance` (an
  absorptive setting) evaluation stops at the value-fixpoint bound,
  yielding a circuit sound for absorptive evaluation; otherwise a
  `max_iter` guard trips.
- Gated to PostgreSQL 15+ (the lowering uses `pg_get_querydef` and
  `pg_analyze_and_rewrite_fixedparams`); on earlier majors recursive
  CTEs over tracked relations keep raising the unsupported error.
- Unsupported recursion shapes are rejected cleanly: a recursive term
  carrying a set-returning function in its target list now raises the
  unsupported error instead of crashing the planner.

#### Correctness and robustness fixes

- **MMap worker out-of-bounds.** The background worker's
  `getChildren` handler took `&children[0]` on a leaf gate's empty
  child vector (undefined behaviour, fatal under a libstdc++
  assertions build); it now uses `children.data()`.
- **`foldSemiringIdentities` single-wire collapse.** Parents are
  rewired instead of duplicating a shared input leaf, fixing
  over-counting of non-read-once probabilities under
  `provsql.boolean_provenance`.
- **Safe-query PK-FD key collision.** PK-FD safe queries now group on
  the FD-determined value, so colliding keys stay read-once.
- **UNION mixing untracked and tracked branches.** The untracked
  branch synthesises `gate_one()` instead of raising.
- **`count_cmp` Poisson-binomial pre-pass** generalised to product /
  monus contributors with disjoint private leaves, not just single
  input leaves.
- **External-tool cancellation.** Tools run in their own process
  group and are `SIGKILL`ed on cancel, so `statement_timeout` is
  honoured; an RAII guard removes the temp directory on throw (no
  `/tmp` leak).

#### Continuous random variables

`expected` / `variance` / `moment` / `central_moment` / `support` /
`rv_sample` now also apply to `agg` and `semimod` gates, extending the
moment / sample / profile surface over aggregated random variables.

#### Window functions (documented limitation)

Window functions remain **unsupported**: they execute and carry tuple
provenance through, but the windowed computation gets no aggregate-
provenance semantics. `process_query` now emits a warning once per
rewritten query level that uses window functions over tracked
relations, and a top-level window function forces the
`classify_top_level` verdict to OPAQUE.

#### Documentation

New **knowledge-compilation** user-manual chapter walking the full
`SQL → circuit → CNF → d-DNNF → probability` pipeline, and **Case
Study 7** (peer-review assignment and knowledge compilation, with a
PG15+ recursive-reachability section) with a pg_regress fixture. A
case-study overview page surfaces the up-to-date feature-coverage
matrix.

#### Upgrade

`ALTER EXTENSION provsql UPDATE` from 1.6.0 installs the new functions
and replaces `circuit_subgraph`; no enum or mmap-format change, and no
metadata migration. The `extension_upgrade` regression canary now
exercises the 1.7.0 surface (`tseytin_cnf_mapping`, `tool_available`)
on the full 1.0.0 → 1.7.0 upgrade chain.

## [1.6.0] - 2026-05-17

Major release headlining the **safe-query rewriter for hierarchical
conjunctive queries** behind the new `provsql.boolean_provenance`
GUC. When the rewriter accepts a query, the resulting circuit is
read-once and `probability_evaluate(prov, 'independent')` runs in
linear time -- replacing the dDNNF / tree-decomposition / external
knowledge-compiler fallback on this class of queries. Six FD-aware
extensions broaden the rewriter's reach beyond the textbook
hierarchical-CQ class to constant-selection, PK FDs, deterministic
relations, PK-unifiable / disjoint-constant self-joins, and FD
closure on the union-find. A query-time TID / BID / OPAQUE
classifier surfaces the per-row independence kind of every result
through `provsql.classify_top_level`, and a TID / BID propagation
layer extends that classification through views, CTAS / `SELECT
INTO` / matview, and INNER / CROSS join syntax.

The gate ABI is extended (one new gate type appended, no
renumbering of older values); the mmap circuit format gains a
fifth per-database file (`provsql_table_info.mmap`) and the
record carries optional ancestor metadata. An
`ALTER EXTENSION provsql UPDATE` from 1.5.0 cleans up the
per-table provsql column shape (drops the auto-named UNIQUE
constraint, drops the DEFAULT, installs the new
`provenance_guard` trigger) but does NOT auto-seed
`provsql_table_info` / ancestor metadata for pre-existing tracked
relations; they land at OPAQUE-by-omission, which is the
conservative outcome (the safe-query rewriter then refuses to fire
on them rather than silently asserting an independence
classification the upgrade script cannot verify). To opt a
pre-existing relation in after the upgrade, the user runs
`set_table_info(t::regclass::oid, 'tid')` plus
`set_ancestors(t::regclass::oid, ARRAY[t::regclass::oid])` for a
TID relation, or `set_table_info(t::regclass::oid, 'bid',
ARRAY[<col_attnos>]::int2[])` plus the same `set_ancestors` for a
relation that was `repair_key`'d in 1.5.0.

#### Safe-query rewriter

A new opt-in pre-pass at the head of `process_query`, gated on
`provsql.boolean_provenance` (default `off`), recognises hierarchical
conjunctive queries (Dalvi & Suciu's “safe queries”) and emits a
factored Boolean circuit whose probability can be computed in
linear time by the existing `'independent'` method.

- **Detector** -- `find_hierarchical_root_atoms` in
  `src/safe_query.c` builds the variable-equivalence union-find,
  identifies the hierarchical root variable, and lays out the
  per-atom inner `SELECT DISTINCT` wraps plus the column-pushdown
  slots for every shared class.
- **Rewriter** -- `rewrite_hierarchical_cq` wraps each atom in
  its own inner `SELECT DISTINCT` Query and joins the wraps at
  the outer level. Each per-row provenance becomes a
  `gate_times` over `gate_plus` subterms, one plus per atom over
  the matching base rows -- a read-once formula by construction.
- **Marker** -- the new `gate_assumed_boolean` gate type (one
  append to the persisted `gate_type` enum) wraps every
  rewritten root so subsequent consumers know the circuit comes
  from a Boolean rewrite. Semirings that do not admit a
  homomorphism from Boolean functions refuse to run on
  `assumed_boolean` -rooted circuits.
- **Multi-component path** -- disconnected component splits
  `q :- A(x), B(y)` re-route through `rewrite_multi_component`,
  which Cartesian-joins one inner sub-Query per component and
  recurses each component through Choice A.
- **Partial-coverage groups** -- when some shared class touches
  only a subset of atoms, the detector folds those atoms into an
  inner `GROUP BY` sub-Query that aggregates the partial-coverage
  variables away ; the outer `Choice A` re-entry then handles the
  inner Query on its own.
- **Column pushdown** -- every fully-covered class is exposed as
  an extra slot in each atom's wrap targetList, so the rewriter
  handles arbitrary multi-class shapes (not just the canonical
  single-root flavour).
- **Atom-local WHERE pushdown** -- single-atom WHERE conjuncts
  ride into the corresponding atom's inner wrap before the
  `DISTINCT`; multi-atom conjuncts stay at the outer level.
- **BID block-key alignment** -- when an atom is BID, the
  rewriter verifies that every block-key column survives in the
  wrap's projection and refuses (rather than mis-factoring) if
  any key column is dropped.
- **Bridge case** -- bridging-connected groups (partial-coverage
  signatures that overlap on a single root atom) merge into a
  single inner group rather than failing the detector.
- **Transitive root joins** -- when the user writes
  `a.x = b.x AND a.x = c.x` but not `b.x = c.x`, the multi-
  component rewriter synthesises the missing intra-group
  equality so the inner wraps preserve per-x granularity.
- **Refuse unsound semirings** -- semirings that do not admit a
  homomorphism from Boolean functions (counting, formula…)
  raise on `assumed_boolean` -rooted circuits ; `boolean`,
  `boolexpr`, `which`, `why`, `how`, the tropical / Viterbi /
  Łukasiewicz / min-max / interval-union evaluators are allowed.

The rewriter is row-count-preserving for queries that have an
outer `GROUP BY` or top-level `DISTINCT` (the candidate gate
refuses otherwise). Two regression entry points exercise the
end-to-end behaviour: `test/sql/safe_query_*.sql` (twelve files
covering the rewriter, pushdown, semirings, FD-aware extensions,
self-joins, BID, view descent, INNER JOIN, ancestry-disjointness)
and `test/bench/safe_query_bench.sql` (26 hierarchical shapes
benchmarked OFF vs ON).

#### FD-aware extensions

Six extensions on top of the base hierarchical-CQ detector, each
recognising a query class the textbook check refuses but the
gate-level circuit is still read-once :

1. **Constant-selection elimination.** A `B.x = 2` conjunct pins
   the entire union-find class for `x` to a literal, drops the
   now-redundant cross-atom equijoins, and routes the pinned
   atom through the multi-component path.
2. **PK / NOT-NULL UNIQUE FDs.** A `PRIMARY KEY` (or
   NOT-NULL `UNIQUE`) on a relation lets the rewriter drop
   FD-determined columns from atom-set membership. The textbook
   H-query `R(x), S(x,y), T(y)` becomes hierarchical when `S`
   has a PK on `x`.
3. **Deterministic-relation transparency.** Relations without a
   `provsql` column carry probability-1 tuples and contribute
   nothing to the gate-level circuit ; the detector treats them
   as transparent, factoring queries like `A ⋈ DimP ⋈ DimC`
   that are hierarchical modulo the deterministic dimensions.
4. **FD closure on the union-find.** A triangle CQ with PKs on
   two endpoints (`R(a PK, b), S(b, c), T(c PK, a)`) has
   FD-reduced atom-sets that are pairwise nested-or-disjoint --
   the detector applies both PK FDs through the union-find
   closure and accepts the query via per-atom anchors.
5. **PK-unifiable self-joins.** Two RTEs over the same relation
   whose PK columns are pairwise equated through the union-find
   refer to the same tuple ; the rewriter collapses the
   duplicate RTEs into a single atom before the candidate gate's
   shared-relid bail.
6. **Disjoint-constant self-joins.** When two RTEs over the same
   relation carry mutually-exclusive `Var = Const` predicates
   (e.g., `r1.kind = 'A' AND r2.kind = 'B'`), the rewriter
   certifies the pair as disjoint and emits each as its own
   inner `DISTINCT` wrap.

#### Query-time TID / BID / OPAQUE classifier

A new `src/classify_query.{c,h}` exposes
`provsql_classify_query(Query *)` and the `provsql.classify_top_level`
GUC (default `off`). When enabled, the planner-hook emits a
per-query `NOTICE` of the form
`ProvSQL: query result is <kind> (sources: <relations>)` where
`<kind>` is one of TID / BID / OPAQUE. Scope includes :

- **Single source through `RTE_SUBQUERY` descent** -- view bodies
  inlined by PG's parser and inline `FROM (SELECT …)` subqueries
  contribute their underlying relation through the recursive
  walker.
- **UNION ALL of disjoint TID legs** -- a fully-`UNION ALL` tree
  of legs each TID over pairwise-disjoint relid sets promotes to
  TID with the cumulative source list.
- **Independent-TID joins** -- `n_meta >= 2` queries no longer
  collapse to OPAQUE. When every source is TID and the
  registered ancestor sets (see below) are pairwise disjoint,
  the result is promoted to TID.
- **BID projection preservation** -- `n_meta == 1` BID sources
  stay BID through `SELECT k FROM bid_t` (block-key column
  kept, resolved transitively through subquery TLEs) and
  downgrade to OPAQUE when the block-key column is dropped.
- **GROUP BY block-key promotion** -- `SELECT k FROM bid_t
  GROUP BY k` collapses to TID (one row per block, each row's
  provenance is its block's key token, an independent
  `gate_input`).
- **ANSI INNER / CROSS JoinExpr descent** -- the shape gate
  accepts `JoinExpr` fromlist entries when
  `jointype == JOIN_INNER`, recursing into both arms to reach
  the underlying `RangeTblRef`s. Outer joins (`LEFT` / `RIGHT`
  / `FULL`) stay OPAQUE because their NULL-padding rows break
  per-row independence.

Studio's `db.py` probes every view at schema-panel load with
`provsql.classify_top_level = on` set inside a SAVEPOINT-wrapped
transaction and surfaces the captured kind on the schema-panel
PROV pill.

#### TID / BID propagation through derived relations

The per-relation metadata that gates the safe-query rewriter now
flows through view / CTAS / matview / `SELECT INTO` derivations.

- **View descent in the safe-query rewriter.** A pre-pass at the
  head of `try_safe_query_rewrite` inlines simple `RTE_SUBQUERY`
  fromlist entries (PG-rewritten view bodies, inline `FROM`
  subqueries) into the outer rtable so the detector and
  rewriter see a flat list of base relations. A subquery is
  “simple” when it's a flat conjunctive `SELECT` with no
  kind-altering features, no LATERAL / security-barrier member
  RTEs, and a target list of plain base-level `Var`s. Fixed-
  point loop handles nested views.
- **`INNER` / `CROSS` JoinExpr flattening.** A second pre-pass,
  running before subquery inlining, dissolves every `INNER` /
  `CROSS` `JoinExpr` in any fromlist -- outer or recursively
  inside any `RTE_SUBQUERY` body -- into flat `RangeTblRef`s plus
  AND-merged `ON`-clauses. Refuses outer joins, aliased joins,
  and `USING` clauses (all three require resolving columns
  through `joinaliasvars` -- the same complexity PG's own
  subquery pull-up handles).
- **Per-table base-ancestor registry.** The existing
  `ProvenanceTableInfo` mmap record gains `ancestor_n` +
  `ancestors[64]` fields tracking the original base
  `add_provenance` / `repair_key` relations a derived table's
  atoms ultimately come from. Three IPC opcodes (`A` set, `a`
  get, `R` remove), a parallel `provsql_lookup_ancestry`
  backend cache, and SQL surface (`set_ancestors`,
  `remove_ancestors`, `get_ancestors`). `add_provenance` and
  `repair_key` auto-seed `{self}`.
- **CTAS / `SELECT INTO` / matview lineage hook.** A new
  `ProcessUtility_hook` intercepts every `CreateTableAsStmt`,
  classifies the inner Query, and (when the inner SELECT
  projects a `provsql` column from a tracked, non-OPAQUE
  source) populates the new relation's kind + transitive
  ancestor union and installs `provenance_guard` via SPI.
  Matviews skip the trigger install (PG forbids triggers on
  them ; matview content only changes through `REFRESH
  MATERIALIZED VIEW`, which re-runs the inner SELECT). BID
  block-key columns are aligned to their output `resno` via
  target-list walk ; demotes to TID if any key column is
  dropped.
- **Ancestry-based disjointness gate.** The safe-query
  candidate gate now rejects any pair of RTEs with different
  relids whose registered ancestor sets overlap -- catches the
  case the syntactic shared-relid bail misses (a CTAS-derived
  table joined with one of its source tables, or two CTAS-
  derived tables sharing a base ancestor through different
  relids).

#### HAVING-COUNT cmp pre-pass

A new probability-evaluation pre-pass handles `HAVING COUNT(*)
> k` -style cmps analytically via the Poisson-binomial DP rather
  than enumerating possible worlds. Gated on the hidden
  `provsql.cmp_probability_evaluation` GUC (default `on` ;
  unregistered name space, mostly intended for benchmark A/B).

#### Boolean-identity peephole

A new `foldBooleanIdentities` pass runs at circuit-load time when
`provsql.boolean_provenance` is on. Three rules :

- **Idempotence** : `gate_plus(x, x, …) → gate_plus(x, …)`.
- **Plus-with-one absorber** : a `gate_plus` whose children
  include the Boolean `one` collapses to `one`.
- **Absorption** : `gate_plus(x, gate_times(x, …)) → x` on the
  UNION-dominated-pair pattern.

The pass leaves a per-gate flag so consumers know which gates
came through the Boolean rewrite. It is layered on top of the
universal peephole pass introduced in 1.5.0 and gated on the
same `provsql.simplify_on_load` GUC.

#### Per-table metadata store

A new fifth mmap file per database (`provsql_table_info.mmap`)
stores per-relation TID / BID / OPAQUE classification plus
multi-column BID block-key columns and the new ancestor set.
SQL surface :

- `set_table_info(relid, kind, block_key)` -- set or upsert a
  relation's kind ; preserves the ancestor half.
- `remove_table_info(relid)` -- tombstone the whole record.
- `get_table_info(relid)` -- return `(kind, block_key)` or NULL.
- `set_ancestors(relid, ancestors)` -- set or upsert the
  ancestor half ; preserves the kind half.
- `remove_ancestors(relid)` -- clear the ancestor half.
- `get_ancestors(relid)` -- return `oid[]` or NULL.

A `cleanup_table_info` event trigger on `sql_drop` removes the
metadata when a tracked relation is dropped outside of
`remove_provenance`. `repair_key` now accepts multi-column
keys (`repair_key('t', 'k1, k2')`).

A per-backend cache (sorted-array, binary-searched) amortises
the IPC across repeated lookups in the planner hot path and is
invalidated through PostgreSQL's relcache invalidation
channel, so concurrent `add_provenance` / `repair_key` /
`remove_provenance` in other backends are reflected without
polling.

#### Studio companion release

ProvSQL Studio 1.2.0 ships in parallel on PyPI as
`provsql-studio==1.2.0` ; minimum required extension version is
1.6.0. The new Studio features include the Boolean-provenance
mode toggle (session-sticky across the Studio backend), the
TID / BID / OPAQUE result pill on the result-table `provsql`
column header, the per-relation TID / BID schema sub-pills, and
the schema-panel row-click `SELECT * FROM <rel>` insertion.

#### GUCs

- `provsql.boolean_provenance` (default `off`): opt-in
  safe-query rewriter. When `on`, every hierarchical CQ that
  matches the candidate gate is rewritten into a read-once
  Boolean circuit.
- `provsql.classify_top_level` (default `off`): emit a per-
  query `NOTICE` describing the result's TID / BID / OPAQUE
  kind plus the contributing tracked sources.
- `provsql.cmp_probability_evaluation` (default `on`, hidden):
  Poisson-binomial DP for HAVING-COUNT cmps.

#### Fixes

- Querying a view that projects the `provsql` column raised
  `no relation entry for relid N` on PostgreSQL 14 and 15.
  Those versions leave legacy `OLD` / `NEW` rule-placeholder
  RTEs (`RTE_RELATION`, relkind `v`, `inFromCl = false`) in
  any view body's range table; the planner-hook walker
  mistook them for real source relations and added Vars the
  planner had no `RelOptInfo` for. PG 16 removed the
  placeholders, masking the bug on newer servers. ProvSQL
  1.6.0 filters them in `get_provenance_attributes` and in
  the new safe-query subquery-inliner, so view-based
  provenance queries work uniformly across PG 10-18.

## [1.5.0] - 2026-05-14

Major release headlining first-class **continuous random-variable
columns** and a hybrid analytic + Monte Carlo evaluator. The gate
ABI is extended (three new gate types appended; no renumbering of
older values); the mmap circuit format is otherwise compatible
and an `ALTER EXTENSION provsql UPDATE` is sufficient.

### First-class random variables

A new `random_variable` type (a thin UUID wrapper,
binary-coercible with `uuid`) carries a probability distribution
per row.  Constructors live in the `provsql` schema:

- `provsql.normal(μ, σ)`, `provsql.uniform(a, b)`,
  `provsql.exponential(λ)`, `provsql.erlang(k, λ)` for the four
  continuous families;
- `provsql.categorical(probs, outcomes)` for discrete categorical
  random variables;
- `provsql.mixture(p, x, y)` (two overloads: shared Boolean gate
  vs ad-hoc Bernoulli probability) for probabilistic mixtures;
- `provsql.as_random(c)` for deterministic point-mass lifts.

Implicit casts from `integer`, `numeric`, and `double precision`
to `random_variable` make `WHERE reading > 2` work without an
explicit wrapper. Arithmetic operators `+ - * /` and unary `-`
build `gate_arith` over the operands; comparison operators
`< <= = <> >= >` are intercepted at planning time and rewritten
into `gate_cmp` calls conjoined into each row's provenance.

The new gate types `gate_rv`, `gate_arith`, `gate_mixture` are
appended to the `gate_type` enum (with a parallel append to the
SQL `provenance_gate` enum). `gate_value` gains a float8 mode
parsed via `extract_constant_double` in `having_semantics.cpp`.

### Hybrid analytic + Monte Carlo evaluation

A three-stage evaluator decides every probabilistic query
analytically where possible and falls back to Monte Carlo
otherwise:

- **`RangeCheck`** propagates support intervals through
  `gate_arith` and tests every `gate_cmp` against the propagated
  interval; decidable comparators collapse to Bernoulli leaves.
  A joint AND-conjunction pass intersects per-variable intervals
  across conjuncts before the decision.
- **`AnalyticEvaluator`** computes the closed-form CDF for any
  single-distribution `gate_cmp` (Normal via `erf`, Uniform by
  arithmetic, Exponential by `log1p`/`expm1`, Erlang via the
  regularised lower incomplete gamma).
- **`Expectation`** semiring runs analytical mean / variance /
  moments per distribution with structural-independence
  detection on `gate_arith TIMES` via a `FootprintCache`.

The **`HybridEvaluator`** simplifier folds family-preserving
combinations (normals close under linear combination; sums of
i.i.d. exponentials with the same rate fold to Erlang; the
affine shapes `-N`, `-U`, `c+N`, `c-N`, `N-c`, `c-U`, `U-c`,
`U+c` fold via a `MINUS → PLUS` canonicalisation plus a uniform
shift-closure rule; `c·X`-style shifts thread through mixtures
and categoricals; single-child arith roots and semiring
identities collapse; deterministic `gate_arith` subtrees are
folded to `gate_value` at load time). The **island decomposer**
splits multi-cmp queries into independent sub-problems on
shared base-RV footprints. Whole-circuit Monte Carlo remains as
the safety net for anything not analytically tractable.

EQ/NE comparators take an analytical path whenever both sides
have extractable Dirac mass-maps with disjoint random-leaf
footprints, resolving to a sum-product over the discrete masses;
the same shortcut also widens to `gate_arith` composites and to
Bernoulli mixtures whose continuous arm fully covers the
support, so equality checks against an outcome of a categorical
or a mixture resolve symbolically.

Symbolic prints in the simplifier use `std::to_chars`
shortest-roundtrip formatting, so folds like `2 * Exp(0.4)` now
print `exponential:0.2` instead of `0.20000000000000001`.

`provsql.simplify_on_load` (default `on`) runs the universal
peephole pass at load time, so every downstream consumer
(semiring evaluators, MC, `view_circuit`, PROV-XML export,
Studio) sees the simplified form.

### Conditional inference

The polymorphic moment dispatchers `expected` / `variance` /
`moment` / `central_moment` / `support` all accept an optional
`prov uuid DEFAULT gate_one()` argument; passing `provenance()`
from inside a tracked query conditions on the row's filter
event automatically. New companion C entry points
`rv_sample(token, n, prov)` (SRF over `float8`),
`rv_histogram(token, bins, prov)` (returning `jsonb`), and
`rv_analytical_curves(token, prov, n_points)` (SRF returning
`(x, pdf, cdf)` rows; mass-stems for discrete arms) expose
conditional samples, histograms, and closed-form PDF/CDF curves
for inspection and downstream analytics.

Closed-form truncated distributions cover Normal (Mills ratio),
Uniform (intersected support), Exponential (memorylessness on a
lower bound, finite-interval truncation via the lower incomplete
gamma), and Erlang (via the same regularised incomplete-gamma
machinery). The truncation pipeline also handles Bernoulli
mixtures (each arm truncated independently and the surviving mass
renormalised), categoricals (filtered outcomes plus rescaling),
and Diracs (kept or dropped against the conditioning event); a
universally-infeasible truncated subtree short-circuits to a
`NaN`-typed Dirac so downstream evaluators do not fire MC blindly.
On top of the moment fast paths, `rv_sample` and `rv_histogram`
take an inverse-CDF fast path on bare `gate_rv` conditional
events – Uniform / Exponential by memoryless inverse, Normal by
Beasley-Springer-Moro – bypassing MC entirely when the gate is a
single recognised distribution under a closed-form truncation.
Anything outside the closed-form table falls back to MC
rejection sampling at `provsql.rv_mc_samples`; a `NOTICE` (or,
for histograms / moments, an error) fires when fewer than the
requested `n` samples land within the budget.

### Aggregation over random variables

Three RV-returning aggregates: `sum`, `avg`, `product`
(over `random_variable`). They lower to a single `gate_arith`
root over per-row `gate_mixture` children produced by the new
`rv_aggregate_semimod` helper. `aggtype`-based dispatch lets the
planner-hook recognise RV-returning aggregates and wrap the
per-row argument before the SFUNC sees it; the FFUNC pulls the
provenance back out of each mixture's first child to build the
matching denominator (`AVG`) or to patch the multiplicative
identity into the else-branch (`PRODUCT`). The
`INITCOND = '{}'` convention lets each aggregate define its own
empty-group identity (`as_random(0)` for `SUM`, SQL `NULL` for
`AVG`, `as_random(1)` for `PRODUCT`).

`HAVING` clauses whose outcome collapses to a deterministic
scalar are supported natively, including the natural shape
`HAVING expected(avg(rv)) > 20` (and the analogous
`variance` / `moment` / `central_moment` over an RV
aggregate). The planner skips the HAVING-lift on such quals
and lets PostgreSQL filter the surviving groups directly, while
the per-group `gate_delta` wrapper is still emitted so the
provenance shape is unchanged. Quals that compute on
`agg_token` results (the historical HAVING surface) continue
to route through `having_Expr_to_provenance_cmp`.

### Studio companion release

ProvSQL Studio 1.1.0 ships in parallel on PyPI as
`provsql-studio==1.1.0`; minimum required extension version is
1.5.0. The new Studio features include the distribution-profile
panel (μ/σ², histogram, PDF/CDF toggle, wheel zoom) with a
closed-form analytical overlay drawn on top of the histogram
bars (terracotta SVG path for continuous arms, discs-on-stems
for Bernoulli mixtures / categoricals / Diracs, staircase
overlay in CDF mode), the `Sample` evaluator with conditional-MC
budget hints, the `Condition on` row-prov auto-preset,
simplified-circuit rendering driven by `provsql.simplify_on_load`,
Config-panel rows for `monte_carlo_seed`, `rv_mc_samples`,
`simplify_on_load`, and a footer that surfaces both the
extension and the Studio package versions (plus a new
`--version` CLI flag). See Studio release notes for details.

### Internal

- Unified `migrate_probabilistic_quals` classifier in
  `src/provsql.c` replaces the historical pair
  `migrate_aggtoken_quals_to_having` + `extract_rv_cmps_from_quals`;
  routes every qual through a `qual_class` enum (pure-RV,
  pure-agg, deterministic, plus mixed-error classes).
- `gate_agg` arm in `monteCarloRV::evalScalar` unlocks HAVING+RV
  under Monte Carlo.
- `gate_delta` is transparent to the rv_* event walker in
  `Sampler::evalBool` and `walkAndConjunctIntervals` so the
  δ-semiring algebra and the random-variable algebra compose
  cleanly.
- `getJointCircuit` in `MMappedCircuit.cpp` builds a multi-rooted
  BFS so shared `gate_rv` leaves between `input` and `prov` are
  loaded into a single `GenericCircuit` and consequently couple
  correctly under MC rejection sampling.
- `random_variable` is now a thin wrapper over `pg_uuid_t` with
  bare-UUID text I/O and a binary-coercible `WITHOUT FUNCTION`
  cast to / from `uuid`; the planner hook emits a `RelabelType`
  instead of a `FuncExpr`. The historical cached-scalar field
  has been removed.
- `runConstantFold` in the load-time simplifier pass folds any
  deterministic `gate_arith` subtree to a single `gate_value`
  (so e.g. `arith(NEG, value:c)` collapses to `value:-c` before
  `asRvVsConstCmp` looks at the cmp).
- `matchTruncatedSingleRv` (in `RangeCheck.h`) factors the
  closed-form single-RV shape detection used by
  `try_truncated_closed_form` / `try_truncated_closed_form_sample`
  / `rv_analytical_curves`, keeping the supported-shape set in
  sync across moments, sampling, and PDF/CDF curves.
- `HybridEvaluator::double_to_text` uses `std::to_chars` for
  shortest-roundtrip formatting of folded scalar coefficients.

### Bug fixes

- Backend segfault at `verbose_level >= 20` when deparsing an
  `EXCEPT`-rewritten tree. `transform_except_into_join` was
  leaving the synthesised `RTE_JOIN` with `NULL` `eref` /
  `joinaliasvars` / `joinleftcols` / `joinrightcols`; execution
  was fine (outer `Var`s reference the inputs directly) but the
  ruleutils deparser walks the rtable and crashed. All four
  fields are now populated on supported PostgreSQL versions
  (`joinleftcols` / `joinrightcols` / `joinmergedcols` are
  guarded for PG &lt; 13). New regression `verbose_setops`
  covers `EXCEPT` and non-`ALL` `UNION`.
- `READM` / `READB` in `provsql_mmap.c` now compare `read()`
  against `(ssize_t)sizeof(...)` so the size check no longer
  promotes to unsigned and masks short-read errors. File-local
  globals are marked `static` and the `-Wmissing-variable-declarations`
  / `-Wunused-result` warnings are clean.
- Tree-mutator / tree-walker callbacks in `src/provsql.c` now
  take `void *` (PostgreSQL's idiom) so clang's
  `-Wcast-function-type-strict` no longer fires at every
  `expression_tree_mutator` / `expression_tree_walker`
  call site; the dead `collect_rv_footprint` helper in
  `HybridEvaluator.cpp` is dropped (its job is done by
  `FootprintCache` in `Expectation.cpp`) and a bare `move()`
  in `where_provenance.cpp` is qualified as `std::move()`.
  Both gcc and clang now build clean.

### GUCs (user-facing)

- `provsql.monte_carlo_seed` (default `-1`): pinning seeds the
  MC sampler for reproducibility across runs and across the
  Bernoulli and continuous sampling paths.
- `provsql.rv_mc_samples` (default `10000`): sample budget for
  the analytical-evaluator MC fallback. Set to `0` to require
  analytical answers (the fallback then raises).
- `provsql.simplify_on_load` (default `on`): runs the universal
  peephole simplifier when circuits are loaded into memory.

`provsql.hybrid_evaluation` is debug-only (`GUC_NO_SHOW_ALL`);
end users have no reason to flip it.

### New documentation

- `doc/source/user/continuous-distributions.rst`: full user
  surface.
- `doc/source/user/casestudy6.rst`: *The City Air-Quality Sensor
  Network*, the first Studio-driven case study.
- `doc/source/dev/continuous-distributions.rst`: architecture
  companion.

### ABI / compatibility

- `gate_type` enum extended (`gate_rv`, `gate_arith`,
  `gate_mixture` appended; no renumbering).
- mmap format compatible with 1.4.0.
- `random_variable` text I/O is bare UUID; the type is binary-
  coercible with `uuid` (cast declared `WITHOUT FUNCTION`), so
  the on-disk and on-wire representations are identical to
  `uuid`. The struct is `pg_uuid_t`.
- `ALTER EXTENSION provsql UPDATE` is sufficient.

## [1.4.0] - 2026-05-09

Major release headlining the **ProvSQL Studio** companion (released
in parallel as `provsql-studio` 1.0.0 on PyPI) and a substantial
expansion of the compiled-semiring family.  The mmap circuit format
is unchanged from 1.3.0; an `ALTER EXTENSION provsql UPDATE` is
enough.

### ProvSQL Studio companion release

[**ProvSQL Studio**](https://provsql.org/docs/user/studio.html), a
self-contained Flask/JS web UI for provenance inspection, ships in
parallel as `pip install provsql-studio==1.0.0`.  Studio renders the
provenance DAG behind any UUID or `agg_token` cell, runs any
compiled semiring (or probability method or PROV-XML export) against
a pinned node, lights up the source rows of a Where-mode result via
hover, and prefils `add_provenance` / `create_provenance_mapping`
calls from the schema panel.  Studio's version stream is independent
of the extension's; the
[compatibility matrix](https://provsql.org/docs/user/studio.html#compatibility)
in the user guide records each Studio release's minimum required
extension version (1.0.0 ↔ extension 1.4.0+).  The Docker image
`inriavalda/provsql:1.4.0` bundles both, exposes Studio on port 8000,
and replaces the legacy Apache + `where_panel` PHP UI.

### New compiled semirings

Ten new `sr_*` evaluators land alongside the existing
`sr_formula` / `sr_counting` / `sr_why` / `sr_boolexpr` /
`sr_boolean` family.  All are dispatched through the
`provenance_evaluate_compiled` C++ path, so they evaluate in a
single circuit traversal and respect circuit caching.

- **`sr_how(token, mapping)`**: canonical `N[X]` polynomial
  provenance (how-provenance), the universal commutative-semiring
  carrier.

- **`sr_which(token, mapping)`**: which-provenance / lineage: the
  set of input labels that influence the result.

- **`sr_tropical(token, mapping)`**: tropical (min-plus) semiring
  on `float8`, returning the cost of the cheapest derivation.

- **`sr_viterbi(token, mapping)`**: Viterbi (max-times) semiring
  on `float8` `∈ [0, 1]`, returning the probability of the most
  likely derivation.

- **`sr_lukasiewicz(token, mapping)`**: Łukasiewicz fuzzy
  semiring: `+ = max`, `× = max(a + b − 1, 0)` on `float8`
  `∈ [0, 1]`, preserving crisp truth and avoiding the near-zero
  collapse of long product chains.

- **`sr_minmax(token, mapping, element_one)`** and
  **`sr_maxmin(token, mapping, element_one)`**: min-max / max-min
  m-semirings over a user-defined enum carrier (security-classification
  shape and trust/availability shape, respectively).  The third
  argument is a sample value of the carrier enum, used only for type
  inference.

- **PG14+: `sr_temporal(token, mapping)`,
  `sr_interval_num(token, mapping)`,
  `sr_interval_int(token, mapping)`**: interval-union m-semiring
  carriers over `tstzmultirange`,
  `nummultirange`, and `int4multirange`.  `sr_temporal` subsumes
  the old `union_tstzintervals_*` helpers (state functions,
  aggregates, monus) which have been removed; the user-facing
  `union_tstzintervals(token, mapping)` wrapper is now a thin
  `SELECT sr_temporal(...)` call retained for backward compatibility,
  and `timetravel`, `timeslice`, `history`, and `get_valid_time` now
  call `sr_temporal` directly.

- **`sr_boolexpr` signature change**: the provenance-mapping
  argument is now optional (`token2value regclass = NULL`).  When
  omitted, leaves are rendered as bare `x<id>` placeholders.
  Existing one-argument callers continue to work unchanged.

- **Paren elision in `sr_boolexpr` and `sr_formula` output**:
  redundant outer parentheses, parentheses around single-child
  subtrees, and parentheses around same-op nested subtrees are
  dropped at rendering time, so long expressions stay readable.
  The parsed expression is unchanged; only the textual form is
  shorter.  Callers that grep `sr_boolexpr` output for exact
  paren counts will need to adjust.

### Circuit introspection helpers

Two new SQL-level helpers expose the gate DAG so external tools can
walk a bounded slice without copying the entire circuit; Studio
uses them to render Circuit mode:

- **`circuit_subgraph(root UUID, max_depth INT DEFAULT 8)`**:
  returns `(node, parent, child_pos, gate_type, info1, info2, depth)`
  for the BFS-bounded subgraph rooted at `root`, joining
  `get_gate_type` / `get_children` / `get_infos` in a single
  recursive CTE and keeping every distinct DAG edge (a child reached
  from `k` parents within the bound contributes `k` rows; self-joins
  contribute one row per child position).

- **`resolve_input(uuid UUID)`**: returns
  `(relation regclass, row_data jsonb)` for the source row whose
  `provsql` column equals `uuid`, by enumerating every
  provenance-tracked relation.  Returns zero rows for non-input gates
  (`plus`, `times`, `agg`…).

### `agg_token` rendering and the `aggtoken_text_as_uuid` GUC

`agg_token` cells now have two render modes, controlled by a new
`provsql.aggtoken_text_as_uuid` GUC:

- **Off (default, unchanged)**: cells render as `value (*)`.

- **On (typical for UI layers like Studio)**: cells render as the
  underlying UUID, so callers can click through to the provenance
  circuit; the `value (*)` side is recovered via the new
  `agg_token_value_text(uuid)` helper, which returns
  `get_extra(token) || ' (*)'` when `token` resolves to an `agg`
  gate.

`agg_token_out` is consequently `STABLE` rather than `IMMUTABLE`
(the chosen output now depends on a session GUC).

### `provsql.tool_search_path` and external-tool robustness

A new `provsql.tool_search_path` GUC (colon-separated directories
prepended to `PATH` when invoking external tools) replaces the
previous "tool must be on the postmaster's `PATH`" assumption.  The
external-tool dispatch layer also gains:

- **Pre-flight tool lookup with structured error decoding**: calls
  fail fast with a clear message when a required tool is missing,
  instead of waiting for an opaque downstream error.

- **`statement_timeout` translation**: a `statement_timeout` that
  fires during d4 / c2d / dsharp / weightmc compilation now becomes
  a proper SQLSTATE 57014 (`query_canceled`) cancel rather than a
  raw subprocess kill.

- **SIGINT translation**: Ctrl-C during `find_external_tool`
  pre-flight is translated into a proper PostgreSQL cancel.

- **Private mkdtemp dir**: external tools now run in a per-call
  `mkdtemp` directory, closing a `/tmp` race on shared hosts.

### Bug fixes

- **`provenance_aggregate` UUID collision under concurrent
  aggregation.** `SUM(id)` and `AVG(id)` over the same children
  could collapse to a single agg gate, after which their
  concurrent `set_infos` calls would overwrite each other's
  aggregation operator (and `provsql_having` would read the wrong
  `agg_kind` under cross-backend contention).  The aggregate
  function OID is now folded into the gate UUID so the two queries
  produce distinct gates.

- **`CircuitCache` poisoning under concurrent gate creation.** A
  rare lost-write between two backends creating the same gate
  could leave the cache pointing at the wrong type / children
  pair.  Fixed; the cache's return-value contract is now aligned
  with what the callers expect (`src/CircuitCache.cpp`).

- **`CircuitCache` poisoning when calling `get_gate_type` before
  `get_children`.** A separate cache-coherence bug along the
  `get_gate_type` → `get_children` ordering has been fixed.

- **Where-provenance column position for multi-table joins.**
  PROJECT-gate column positions were computed against the wrong
  RTE for multi-table joins, causing empty locator sets on some
  query shapes.  Fixed.

- **1.2.3 → 1.3.0 upgrade WARNING text.** The recovery-instructions
  block raised by the storage-layout check used `%s` placeholders
  inside a `RAISE WARNING`, where the substitution marker is `%`
  (no type letter); the data-directory path was rendered with a
  stray `s` appended (`<datadir>s` instead of `<datadir>`). Fixed.
  The behaviour of the upgrade itself was unaffected.

### Documentation

- **Studio user-guide chapter** (`doc/source/user/studio.rst`): a
  full walkthrough of Where mode, Circuit mode, the eval strip, and
  the Config panel, plus a compatibility matrix and screenshots.
  Cross-links from the introduction, semiring, and probability
  pages.

- **Semirings chapter expansion**
  (`doc/source/user/semirings.rst`, +300 lines): new sections
  documenting `sr_how`, `sr_which`, `sr_tropical`, `sr_viterbi`,
  `sr_lukasiewicz`, `sr_minmax` / `sr_maxmin`, and the PG14+
  interval-union family, with a capability matrix summarising
  each semiring's identities and δ-handling.

- **Expanded case studies**: Case Study 1 gains a
  *Minimum Security Clearance* step that walks `sr_minmax` over a
  `classification_level` enum mapping; case study 4 has its
  temporal-DB code samples migrated from `union_tstzintervals` to
  `sr_temporal`; case studies 3 and 5 are realigned with the new
  paren-elided `sr_boolexpr` / `sr_formula` output.

- **Developer guide**: new Studio architecture chapter
  (`doc/source/dev/studio.rst`) covering the Flask app layout, the
  `/api/*` surface, the auto-prepare and `search_path` pinning
  strategy, and how Circuit mode walks `circuit_subgraph`.

- **Build-system chapter**: new "Studio releases" section in
  `doc/source/dev/build-system.rst` documenting Studio's
  independent version stream, Trusted Publishing on PyPI, the
  `studio-v*` tag workflow, and the hand-edited
  `studio/CHANGELOG.md` discipline.

### Infrastructure

- **Studio CI workflow** (`.github/workflows/studio.yml`): Python
  3.10 / 3.11 / 3.12 / 3.13 × PostgreSQL 14 / 15 / 16 matrix
  covering pytest, Playwright e2e, ruff, and a wheel-install smoke.

- **Studio release pipeline**
  (`.github/workflows/studio-release.yml`): tag-triggered
  (`studio-v*`), publishes to PyPI via Trusted Publishing,
  attaches sdist + wheel to a GitHub release, embeds
  the matching `studio/CHANGELOG.md` section in the release notes,
  and aborts loudly if the section is missing.

- **Docker image rework**: bundles Studio (PyPI install at image
  build time, contributor `STUDIO_SOURCE=` override for editable
  installs); adds the PGDG apt source so any `PSQL_VERSION` resolves
  (Debian bookworm only ships 15); collapses the apt layer; replaces
  Apache + `where_panel/` with `provsql-studio` on port 8000;
  parallelises `make` in the build.

- **`where_panel/` removed.**  The legacy PHP where-provenance UI is
  superseded by Studio's Where mode.

### Upgrade procedure

```sh
make install
```

In each database that uses ProvSQL:

```sql
ALTER EXTENSION provsql UPDATE;
```

The mmap circuit format is unchanged from 1.3.0; for users already
on 1.3.x no migration is required.  Users still on 1.2.x must run
`provsql_migrate_mmap` first to move the flat
`$PGDATA/provsql_*.mmap` files into the per-database layout
introduced in 1.3.0; see the [1.3.0 release notes](https://provsql.org/releases/v1.3.0/)
for the full procedure.

## [1.3.1] - 2026-05-04

A bug-fix release focused on `repair_key` / `mulinput` correctness,
plus a corrected upgrade path from 1.2.3 and documentation additions
(a fifth case study and expanded material in case studies 1 and 2).
No on-disk format change relative to 1.3.0; an
`ALTER EXTENSION provsql UPDATE` is enough.

### Upgrade-script corrections

- **`sql/upgrades/provsql--1.2.3--1.3.0.sql`** shipped with 1.3.0 only
  carried the per-database mmap migration warning and missed two
  groups of SQL-surface changes that had landed in
  `provsql.common.sql` / `provsql.14.sql` during the 1.3.0 dev cycle:
  the lazy-input-gate refactor of `add_provenance` / `repair_key`
  (commit `f670b7f`) and the schema-qualified
  `provsql.time_validity_view` references in `timetravel`,
  `timeslice`, `history`, and `get_valid_time` (commit `1f59032`).
  Users on 1.2.3 who ran `ALTER EXTENSION provsql UPDATE TO '1.3.0'`
  ended up with a stale set of function bodies. The script in 1.3.1
  has been corrected and now applies all the missing changes; users
  still on 1.2.3 reach a clean 1.3.0-equivalent SQL surface when they
  upgrade after 1.3.1.

- **`sql/upgrades/provsql--1.3.0--1.3.1.sql`** applies the same
  catch-up changes on the 1.3.0 → 1.3.1 path so that users **already
  on 1.3.0** (who came through the broken upgrade) are brought back
  in sync. Fresh installs of 1.3.0 also run this script, but the
  CREATE OR REPLACE statements match the source already on disk, so
  it is a no-op for them.

### Bug fixes

- **`probability_evaluate(..., 'tree-decomposition')` on circuits
  containing `mulinput` gates.** Input gates produced by `repair_key`
  could share an internal id when their UUIDs were never materialised
  in the d-DNNF builder, causing the probability to be wrong and to
  vary from one session to the next on identical data. The aliasing
  has been removed (`src/BooleanCircuit.cpp`,
  `src/dDNNFTreeDecompositionBuilder.cpp`); a regression test
  (`test/sql/treedec_mulinput.sql`) covers the affected query shapes.

- **Off-by-one in `BooleanCircuit::rewriteMultivaluedGates`.** The
  splitter that turns a `mulinput` into a chain of independent
  Bernoulli inputs produced non-deterministic probabilities under
  self-join + `GROUP BY` queries. Fixed; the four built-in evaluation
  methods (default, `'possible-worlds'`, `'tree-decomposition'`,
  `'monte-carlo'`) now agree on `mulinput`-bearing circuits.

- **Shapley and Banzhaf computation on `mulinput` circuits.**
  `shapley()`, `shapley_all_vars()`, `banzhaf()`, and
  `banzhaf_all_vars()` previously walked through `mulinput` gates and
  returned meaningless values. They now raise a clear error
  identifying the unsupported gate type.

### Documentation

- **New Case Study 5: The Wildlife Photo Archive.** A 30-photo /
  13-species / 63-detection synthetic dataset demonstrates the
  `VALUES` clause, `repair_key` and the `mulinput` gate (with the
  numerical effect of mutual exclusion made explicit via
  `sr_boolexpr` and `probability_evaluate`), probabilistic ranking
  versus naive confidence thresholding, `EXCEPT` with monus, common
  table expressions, and `expected()` aggregates. The case study is
  bundled (no external data download) and is part of the regression
  suite.

- **Case Study 1** gains three steps in the circuit-inspection
  section: a tree-decomposition probability variant in the benchmark
  step, an `sr_boolexpr` step on the Nairobi monus token, and a
  programmatic circuit-inspection step using `get_nb_gates`,
  `get_gate_type`, `get_children`, and `identify_token`.

- **Case Study 2** gains two steps: a bulk Shapley/Banzhaf step using
  `shapley_all_vars` / `banzhaf_all_vars` (contrasted with the
  per-variable cross-join from the existing Steps 13 and 14), and a
  step on arithmetic over aggregate results illustrating the
  `agg_token` cast warning.

- **Copy-to-clipboard buttons** on every documentation code block
  (`sphinx-copybutton`). A small JS shim
  (`doc/source/_static/copybutton-shim.js`) papers over an
  incompatibility between `sphinx-copybutton` 0.4.0 (the version in
  Ubuntu Noble's apt) and Sphinx 9.

### Infrastructure

- Release tarballs and CI workflows exclude the `studio/`
  subdirectory for future developments.

### Upgrade procedure

```sh
make install
```

In each database that uses ProvSQL:

```sql
ALTER EXTENSION provsql UPDATE;
```

The mmap circuit format is unchanged from 1.3.0; no migration is
required.

## [1.3.0] - 2026-05-04

### Breaking change: per-database circuit storage

Prior to 1.3.0, the provenance circuit was stored in four flat files at
the root of the PostgreSQL data directory (`$PGDATA/provsql_gates.mmap`,
`provsql_wires.mmap`, `provsql_mapping.mmap`, `provsql_extra.mmap`),
shared across all databases in the cluster. Starting with 1.3.0, each
database gets its own isolated set of files under
`$PGDATA/base/<db_oid>/`.

**Users upgrading from 1.2.x must migrate their circuit data before
upgrading.** The new `provsql_migrate_mmap` tool handles this. If the
migration is skipped, existing circuit data becomes inaccessible (new
provenance queries still work, but provenance computed under the old
version is lost). The upgrade script detects old flat files and raises a
WARNING with recovery instructions if they are still present.

#### Upgrade procedure

1. Install the new ProvSQL binaries:
   ```
   make install
   ```

2. Run the migration tool as the `postgres` user:
   ```
   provsql_migrate_mmap -D $PGDATA -c <connstr>
   ```
   The tool reads the old flat files, collects root UUIDs from each
   database's provenance-tracked tables, writes per-database files under
   `$PGDATA/base/<db_oid>/`, and deletes the old flat files on success.

3. Restart PostgreSQL.

4. In each database that uses ProvSQL:
   ```sql
   ALTER EXTENSION provsql UPDATE;
   ```

#### If you forgot step 2

If PostgreSQL has already been restarted with the new binaries before
migrating, some empty per-database files may have been created. To
recover:

1. Delete the empty per-database files:
   ```
   rm -f $PGDATA/base/*/provsql_*.mmap
   ```
2. Restart PostgreSQL.
3. Immediately run `provsql_migrate_mmap` before executing any
   provenance query.

### Lazy input gate creation

`add_provenance()` no longer eagerly writes an input gate to the circuit
for every existing row in the table at the time it is called. Gates are
now created on first reference during a query, at the cost of a small
overhead on the first query that touches each row. This significantly
reduces the overhead of provisioning large tables.

### Four case studies

Four worked examples have been added to the documentation and are
included as regression tests:

- **Case Study 1: The Intelligence Agency**: simple introductory
  example with Boolean and why-provenance.
- **Case Study 2: The Open Science Database**: comprehensive example
  covering why-provenance, where-provenance, custom semirings,
  probabilities, Shapley and Banzhaf values.
- **Case Study 3: Île-de-France Public Transit**: Boolean provenance
  and formula inspection over GTFS transit data.
- **Case Study 4: Government Ministers Over Time**: temporal provenance
  with `union_tstzintervals` and time-validity views.

### Bug fixes

- Fix GROUP BY provenance aggregation silently dropped when ORDER BY
  referenced the semiring result column.
- Fix d-DNNF tree decomposition: deduplicate OR gate children to prevent
  double-counting in probability evaluation.
- Fix NULL dereference and out-of-bounds crashes in where-provenance on
  views.
- Fix temporal functions (`time_filter`, `time_range`, `in_interval`) to
  use schema-qualified `provsql.time_validity_view`, preventing failures
  when `search_path` does not include the `provsql` schema.
- Fix `sr_boolean` evaluation when the provenance mapping uses integer
  values.
- Fix where-provenance PROJECT gate positions for provenance tables that
  are not the first RTE in a query, causing empty locator sets on some
  PostgreSQL versions.

## [1.2.3] - 2026-04-12

### PGXN improvements

- Prevent indexing of secondary documentation directories
  (`doc/source/`, `doc/tutorial/`, `doc/demo/`, `doc/aggregation/`,
  `doc/temporal_demo/`, `where_panel/`) on the PGXN distribution page
  via `no_index` in `META.json`.

- Document PGXN as an installation channel in the user guide, with
  a note that `pgxn install` does not configure
  `shared_preload_libraries`.

- Add a GitHub Actions workflow (`.github/workflows/pgxn.yml`) that
  automatically publishes releases to PGXN on version-tag pushes.

### Documentation and repository housekeeping

- Add `CODE_OF_CONDUCT.md`.

- Add architecture dataflow diagram to the website overview page.

- Replace `sudo` with generic "as a user with write access to the
  PostgreSQL directories" wording across installation and contribution
  documentation.

## [1.2.2] - 2026-04-11

### In-place extension upgrades

`ALTER EXTENSION provsql UPDATE` is now supported, starting with this
release.  A committed chain of upgrade scripts under `sql/upgrades/`
covers every previous release (1.0.0 → 1.1.0 → 1.2.0 → 1.2.1 → 1.2.2),
so users on any historical version can upgrade in place without
dropping and recreating the extension.  The persistent provenance
circuit (memory-mapped files) is preserved across the upgrade: the
on-disk format has been binary-stable since 1.0.0, and the relevant
headers (`src/MMappedCircuit.h`, `src/provsql_utils.h`) now carry
explicit warnings so future contributors don't break that guarantee
by accident.

A pg_regress regression test (`test/sql/extension_upgrade.sql`)
exercises the full chain end-to-end on every PostgreSQL version in
the CI matrix, installing the extension at 1.0.0 from a frozen
install-script fixture and walking it up to the current
`default_version`.  See the new "Extension Upgrades" section of the
developer guide for the workflow contributors should follow when
making SQL changes.

### Repository housekeeping and discoverability

- **`CHANGELOG.md`** at the repository root, mirroring the release
  notes published at
  [provsql.org/releases](https://provsql.org/releases/).  It is
  automatically kept in sync by `release.sh`.

- **GitHub issue and pull-request templates** under `.github/`.
  The bug-report form prompts for PostgreSQL version, ProvSQL
  version, OS, a minimal SQL reproducer, and optional verbose-mode
  output; the PR template carries a contributor checklist and links
  to the developer guide.

- **DockerHub** image-version badge added to the README
  ([`inriavalda/provsql`](https://hub.docker.com/r/inriavalda/provsql))
  and a prose pointer on the website overview page.

- **PGXN `META.json`** at the repository root, making ProvSQL ready
  for submission to the [PostgreSQL Extension Network](https://pgxn.org).
  Submission will happen once upstream approval lands; no change to
  the build or install flow in the meantime.

- **`CITATION.cff`** now carries the Zenodo concept DOI
  ([`10.5281/zenodo.19512786`](https://doi.org/10.5281/zenodo.19512786))
  and a Software Heritage archive URL in its `identifiers` block.

### Infrastructure

- `release.sh` learned to update `CITATION.cff`, `CHANGELOG.md`, and
  `META.json` in sync with `provsql.common.control` and
  `website/_data/releases.yml`, and to enforce the presence of an
  upgrade script (auto-generating a no-op when no SQL sources have
  changed since the previous tag).
- CI workflows now fetch git tags so `git describe` works inside
  the pgxn-tools containers, which unblocks the Makefile's
  dev-cycle upgrade-script generation.
- The four build / docs workflows' `paths-ignore` lists exclude
  `META.json`, `.github/ISSUE_TEMPLATE/**`, and
  `.github/pull_request_template.md`, so metadata-only edits do not
  trigger the full CI matrix any more.

### No SQL-level changes

There are **no changes to `sql/provsql.common.sql` or
`sql/provsql.14.sql`** in this release.  The SQL API, query
rewriter, semiring evaluators, and probability machinery are
unchanged from 1.2.1.  The upgrade script `1.2.1 → 1.2.2` is
accordingly an empty placeholder.

## [1.2.1] - 2026-04-11

Maintenance release headlining the new **developer guide** and laying
the groundwork for long-term archival and citation.

### Highlights

- **Developer guide** (14 chapters, ~3500 lines): PostgreSQL extension
  primer, architecture, query rewriting pipeline, memory management,
  where-provenance, data-modification tracking, aggregation semantics,
  semiring and probability evaluation (including the block-independent
  database model and the expected Shapley / Banzhaf algorithm from
  Karmakar et al., PODS 2024), coding conventions, testing, debugging,
  and the build system.  Cross-references to Lean 4 machine-checked
  proofs of the positive-fragment rewriting rules and the m-semiring
  axioms.  See the new Developer Guide tab in the documentation.

- **User guide updates**: expanded coverage of `expected()`, the
  `choose` aggregate, custom semiring evaluation, and diagnostic
  functions.  "Formula semiring" has been renamed to "symbolic
  representation" throughout.

- **`CITATION.cff`**: standard citation metadata at the repo root.
  GitHub now shows a **Cite this repository** button that emits
  BibTeX and APA for the ICDE 2026 paper.

- **Software Heritage** archival is active – the full repository
  history is continuously preserved at archive.softwareheritage.org.

- **Zenodo integration** enabled: starting with this release, every
  tagged version receives a persistent DOI
  ([10.5281/zenodo.19512786](https://doi.org/10.5281/zenodo.19512786)).

### Fixes

- `create_provenance_mapping_view` is now available on all supported
  PostgreSQL versions, not only PG 14+.

- External-tool tests (`c2d`, `d4`, `dsharp`, `minic2d`, `weightmc`,
  `view_circuit_multiple`) now skip cleanly when the tool is not
  installed, instead of being removed from the test schedule.

### Infrastructure

- Automated documentation coherence check runs in CI (validates every
  `:sqlfunc:` / `:cfunc:` / `:cfile:` / `:sqlfile:` reference resolves
  to a live Doxygen anchor).
- Mobile-friendly Doxygen and Sphinx output.
- CI speedups: concurrency groups, skip-on-tags, macOS `pg_isready`
  race fix.
- In-place extension upgrades via `ALTER EXTENSION provsql UPDATE` are
  supported starting with this release; upgrade scripts live under
  `sql/upgrades/` and the path is exercised by an automated CI test.

## [1.2.0] - 2026-04-10

This release focuses on providing broader and more consistent support
for SQL language features within provenance tracking.  Systematic
testing across a wide range of query patterns led to numerous bug
fixes, new feature support, and clearer error messages for unsupported
constructs.

### New Features

- **CTE support**: Non-recursive `WITH` clauses now fully track
  provenance.  Nested CTEs (CTE referencing another CTE) and CTEs
  inside `UNION`/`EXCEPT` branches are supported.  Recursive CTEs
  produce a clear error message.

- **`INSERT ... SELECT` provenance propagation**: When both source
  and target tables are provenance-tracked, `INSERT ... SELECT` now
  propagates source provenance to the inserted rows instead of
  assigning fresh tokens.  A warning is emitted when the target table
  lacks a `provsql` column.

- **Correct arithmetic and expressions on aggregate results from
  subqueries**: Explicit casts (`cnt::numeric`), arithmetic
  (`cnt + 1`), window functions (`SUM(cnt) OVER()`), and expressions
  (`COALESCE`, `GREATEST`, etc.) on aggregate results from subqueries
  now produce correct values with a warning, using the original
  aggregate return type from the provenance circuit.

- **UNION ALL with aggregate columns**: `UNION ALL` of queries
  returning aggregate results now works correctly.

### Bug Fixes

- Fixed crash when mixing `COUNT(DISTINCT ...)` with `provenance()` or
  `sr_formula(provenance(), ...)` in the same query.

- Fixed `COUNT(*)` returning NULL instead of `0 (*)` on empty results
  without `GROUP BY`.

- Fixed `provenance_cmp` function failing with "function
  uuid_ns_provsql() does not exist" when `provsql` was not in
  `search_path`.

### Improved Error Messages

- `provenance_evaluate` on unsupported gate types now reports the
  specific gate type and suggests using compiled semirings.

- Subquery errors now read "Subqueries (EXISTS, IN, scalar subquery)
  not supported" instead of the misleading "Subqueries in WHERE
  clause".

- Clear error messages for unsupported operations on aggregate
  results: `DISTINCT` on aggregates, `UNION`/`EXCEPT` (non-ALL) with
  aggregates, `ORDER BY`/`GROUP BY` on aggregate results from
  subqueries.

- Dropped redundant "by provsql" suffix from all error messages (the
  "ProvSQL:" prefix is already present).

### Documentation

- Updated supported/unsupported SQL features list with accurate
  coverage based on systematic testing.

- Added documentation for `INSERT ... SELECT` provenance propagation.

- Expanded aggregation documentation with examples of casts, window
  functions, `COALESCE`, and `GREATEST` on aggregate results.

- Added workaround guidance for unsupported features (use `LATERAL`
  for correlated subqueries, explicit cast for comparison on
  aggregates).

## [1.1.0] - 2026-04-09

### Support for arithmetic on aggregate results

Queries performing arithmetic on aggregate results (e.g.,
`SELECT COUNT(*)+1` or `SUM(id)*10`) are now supported.  Previously,
these queries produced incorrect results because the planner hook
replaced aggregate references with `agg_token` values without
adjusting surrounding operator type expectations.  This is handled by
adding implicit and assignment casts from `agg_token` to standard SQL
types (`numeric`, `double precision`, `integer`, `bigint`, `text`),
and by inserting appropriate type casts during query rewriting when
aggregate results are used inside operators or functions.  A warning
is emitted when provenance information is lost during such
conversions.

### Infrastructure improvements

- Versioned Docker image tagging (images are now tagged with the
  release version in addition to `latest`).
- Improved release process: post-release version bump is now
  automated, and release tarballs exclude non-essential files (CI
  workflows, release script, branding, Docker, and website assets).
- CI fixes for macOS and documentation builds.

## [1.0.0] - 2026-04-05

Initial official release of ProvSQL after 10 years of development.
ProvSQL is now fully documented and usable in production.
