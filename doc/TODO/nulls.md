# NULL Support in ProvSQL: Semantics, Current State, and Recommendations

*Analysis document, July 2026. Based on ProvSQL 1.11.0-dev (PostgreSQL 18),
code inspection of the rewriter and evaluators, and live experiments on a
scratch database (transcript in Appendix A). Not a commitment to implement;
a map of the terrain.*

## 1. Executive summary

ProvSQL's implicit position on NULLs is: “NULL data values are PostgreSQL's
business; provenance only combines the tokens of rows that survive
evaluation.” This delegation is correct more often than one might fear,
because for a large fragment of SQL the three-valued logic (3VL) of NULLs is
observationally equivalent to two-valued logic. But it breaks down exactly
where the formal-semantics literature says it must:

- **`NOT IN` over a subquery containing NULLs** returns rows, with non-zero
  provenance, that SQL excludes. The rewriting changes deterministic query
  answers, and probabilistic evaluation on the resulting circuits computes
  probabilities for the wrong query.
- **`EXCEPT` / `EXCEPT ALL`** fail to remove NULL rows because the monus
  antijoin matches with `=` instead of `IS NOT DISTINCT FROM`, while SQL set
  operations treat NULLs as identical. (Orthogonally, `EXCEPT ALL`
  multiplicities deliberately follow ProvSQL's documented bag difference
  [SMS25], not SQL's `max(0, m − n)` — see §2; the NULL-matching bug and
  its fix are the same under either reading, only test expectations
  differ.)
- **Comparisons involving a NULL `random_variable`** are silently treated as
  certainly true (probability 1) instead of unknown (row absent).
- **`HAVING <agg> <op> <const>` on a NULL aggregate** (all-NULL group)
  returns the group with a NULL provenance token, which under ProvSQL's own
  convention (NULL token ≡ semiring 1) asserts the group certainly passes.
- One reproducible **backend crash**: `provsql.aggregation_evaluate` with a
  NULL token (reachable from `avg` over an all-NULL group) segfaults, because
  `src/aggregation_evaluate.c` never checks `PG_ARGISNULL(0)`. Low practical
  impact: the function is deprecated (already removed from the
  documentation) and slated for removal.

Conversely, a substantial amount of NULL handling is already *right*, and
some of it is sophisticated: grouping and set-operation deduplication use
PostgreSQL's native NULL-identical grouping; `count(col)` versus `count(*)`
is rewritten correctly; the aggregation gates skip NULL inputs exactly as SQL
does; `HAVING <agg> IS [NOT] NULL` is compiled into a possible-worlds-correct
gate construction; the LEFT JOIN lowering pads with NULLs and uses monus
correctly on its supported shape; `NOT EXISTS` is correct under NULLs (for a
principled reason, not by luck; see §5.2).

The headline conceptual point: Guagliardo and Libkin's flagship example of
three “equivalent” difference idioms that diverge under NULLs is realized
*inside ProvSQL* as three queries that all get the **same** provenance,
while their SQL answers are pairwise different (§4.3, V1/V2). The fix is not
a new algebra: every counterexample found is expressible with existing gates.
The bugs are in the SQL-to-circuit translation, specifically in the equality
predicates used by the negative-fragment rewrites.

Recommendations are phased (§6): P0 crash/robustness guards; P1 restore
result-set fidelity (EXCEPT matching, NOT IN lift, HAVING-NULL and RV-NULL
tokens); P2 write down the NULL semantics as a specification and make the
NULL-token convention self-consistent; P3 differential and possible-worlds
testing; P4 research directions where ProvSQL could go beyond the state of
the art (missing values as random variables, certain/possible provenance
pairs).

## 2. Background: what SQL's NULL semantics actually is

### 2.1 Three-valued logic and where it bites

The reference formal treatment is Guagliardo and Libkin's semantics of basic
SQL [GL17], experimentally validated against PostgreSQL and Oracle, and its
Coq mechanizations by Ricciotti and Cheney [RC22] (NullSQL, with lateral
joins) and Benzaken and Contejean's SQLCoq. The essentials:

- Atomic predicates over a NULL operand evaluate to **unknown** (u); truth
  values propagate through AND/OR/NOT by Kleene logic; `WHERE` and `HAVING`
  keep only rows/groups evaluating to **t** (f and u are conflated).
- `x IN Q` is the *disjunction* of equalities with every element of Q, so it
  can be u without being t; `EXISTS Q` is only ever t or f (it asks for
  non-emptiness of the rows where the condition is t). Hence `NOT IN` and
  `NOT EXISTS` genuinely differ: negation swaps t and f but *fixes* u, and u
  is then filtered at the top.
- **Set operations and duplicate elimination switch to syntactic equality**:
  `UNION`/`INTERSECT`/`EXCEPT`, `DISTINCT`, and `GROUP BY` treat two NULLs as
  the same value. SQL is thus deliberately inconsistent: `NULL = NULL` is u
  in a predicate, but `{NULL} EXCEPT {NULL}` is empty.
- Aggregates skip NULL inputs; `count(*)` counts rows, `count(col)` counts
  non-NULL values; `sum`/`avg`/`min`/`max` over an empty or all-NULL group
  yield NULL, `count` yields 0.
- Bag semantics: `EXCEPT ALL` has multiplicity `max(0, m − n)` where m, n
  count *syntactically equal* tuples on the two sides.

One SQL behavior is deliberately **not** a conformance target for ProvSQL:
`EXCEPT ALL` multiplicities. As documented in the benchmark paper [SMS25],
ProvSQL's multiset difference maps t to 0 whenever t occurs at all in the
right operand, and to its full left-hand multiplicity otherwise — not to
`max(0, m − n)`. The reason is tractability: SQL's semantics has no
canonical per-output-row provenance (which left copy is cancelled by which
right copy is an arbitrary pairing), and computing provenance for it is
intractable, whereas the chosen semantics is definable tuple-wise. The two
coincide after duplicate elimination, so plain `EXCEPT` remains a full
SQL-conformance target; throughout this document, `EXCEPT ALL` is measured
against the documented semantics instead. One NULL-specific subtlety:
[SMS25] identifies this difference with SQL's `NOT IN`, which is accurate
only in the NULL-free setting it works in. Under NULLs the two split:
`NOT IN` u-poisons (one NULL on the right removes *every* left row — Q1
below), while the difference's tuple matching must stay syntactic so that
its deduplicated form keeps coinciding with `EXCEPT` (Q3 below). So with
NULLs, ProvSQL's difference sides with Q3, and the `NOT IN` lift is a
genuinely separate construct (recommendation 5).

The canonical example ([GL17], Example 1; also opening [RC22]): with
`R = {1, NULL}` and `S = {NULL}`, the three natural ways to write `R − S`
give three different answers:

| Query | Answer |
|---|---|
| `Q1`: `SELECT R.a FROM R WHERE R.a NOT IN (SELECT S.a FROM S)` | `∅` |
| `Q2`: `SELECT R.a FROM R WHERE NOT EXISTS (SELECT * FROM S WHERE S.a = R.a)` | `{1, NULL}` |
| `Q3`: `SELECT a FROM R EXCEPT SELECT a FROM S` | `{1}` |

Any provenance semantics that assigns these three queries the same
annotations is wrong for at least two of them. (ProvSQL currently does
exactly that; §4.3.)

A second, more comforting result of [GL17] (formally re-proved in [RC22],
including with lateral joins): **3VL adds no expressive power**. Every basic
SQL query has an equivalent that can be evaluated under ordinary two-valued
logic, obtained by compiling NULL-awareness into the conditions (`IS NULL`
guards, `IS NOT DISTINCT FROM`, etc.). This matters for ProvSQL: it
guarantees that a *provenance-correct* rewriting always exists within
ordinary 2VL machinery; there is no need for three-valued gates in the
circuit model to capture SQL's semantics on a fixed instance.

### 2.2 Provenance with NULLs in the literature

The K-relation framework (Green, Karvounarakis, Tannen 2007) and its
extensions used by ProvSQL, m-semirings with monus (Geerts, Poggi 2010) and
aggregation via semimodules and the δ operator (Amsterdamer, Deutch, Tannen
2011), are all defined over NULL-free instances. The one formal treatment
that combines semiring provenance with SQL-style NULLs is Benzaken,
Cohen-Boulakia, Contejean, Keller, Zucchini [BCCKZ21]: a Coq-formalized
K-algebra over the Datacert/SQLCoq semantics, covering the positive algebra
plus a top-level aggregate (no difference, no HAVING), for queries with
NULLs and correlated subqueries, with an adequacy theorem for K = ℕ (bag
semantics). Its design decision is the important one:

> **Selection rule of [BCCKZ21]:** ⟦σ_f(q)⟧(t) = ⟦q⟧(t) if f evaluates to
> **t** on tuple t under SQL's 3VL, and **0** otherwise.

That is: NULLs live in the *data*, predicates are evaluated three-valued on
that data, and the annotation of any tuple whose condition is u or f is the
semiring zero. Provenance does not try to represent “maybe”; unknown means
absent, exactly as in SQL's answer. Aggregation is handled with δ and ⊛
(semimodule) gates, the same algebra ProvSQL implements. This is the natural
specification for ProvSQL's general semiring provenance (§3.2).

What none of these frameworks does is treat the NULL itself as *uncertain
data*. That is the province of incomplete-information theory: Codd tables,
v-tables and c-tables (Imieliński, Lipski 1984), certain answers, and
Libkin's line of work on what SQL's 3VL actually computes relative to
certain answers (e.g. “SQL's Three-Valued Logic and Certain Answers”, TODS
2016: SQL evaluation is neither sound nor complete for certain answers in
general, though variants can be made sound). Practical systems that combine
annotations with incompleteness include Uncertainty-Annotated Databases
(Feng, Huber, Glavic, Kennedy, SIGMOD 2019) and attribute-level AU-DBs
(Feng et al., SIGMOD 2021), which maintain *under- and over-approximations*
(certain / possible) of answers through arbitrary queries. These are the
reference points for the research directions in §6 (P4).

## 3. What provenance for NULL constructs should mean

Three distinct questions hide under “NULL support”, and they have answers of
very different maturity.

### 3.1 Question 1: fixed instance, general (m-)semiring

*What is the provenance of a query over a database that contains NULLs,
where the NULLs are just values?*

Here the answer is settled by [BCCKZ21] for the positive fragment and
extends mechanically to the rest: evaluate every condition with SQL's 3VL on
the actual data; a tuple's annotation is the usual semiring combination over
the derivations whose conditions are **t**; conditions evaluating to u
contribute zero, exactly like f. Set-operation and grouping steps use
syntactic (NULL-identical) equality when matching tuples, because that is
what SQL's ε (duplicate elimination) and bag difference do. The antijoin
monus circuit `aᵢ ⊗ (𝟙 ⊖ ⊕ⱼ bⱼ)` then captures *exactly* ProvSQL's
documented bag difference (§2, [SMS25]) — *provided the matching is
syntactic*: per left copy its counting-semiring value is `max(0, 1 − n)`,
i.e. the copy survives iff no right row matches, and after ε it coincides
with SQL's `EXCEPT`. It does not, and deliberately will not, capture SQL's
`EXCEPT ALL` multiplicity `max(0, m − n)`: that semantics assigns no
canonical provenance to an individual output copy (the pairing of
cancelled copies is arbitrary), so the possible-worlds criterion of §3.2
is not even well-posed for it. The deliberate choice is what makes bag
difference provenance-definable per row at all.
Aggregates skip NULLs in the semimodule combination; `count(*)` does not.

Nothing in this requires changing the gate vocabulary. It only requires that
the rewriter's own auxiliary predicates (antijoin conditions, sublink
lifts) be NULL-aware in the same way SQL's operators are, which by the
2VL-compilation theorem is always expressible (`IS NOT DISTINCT FROM`,
`IS NULL` guards).

One caveat specific to m-semirings: the identification “annotation 0 ≡
absent tuple” (ProvSQL's design position) is semantically robust
for the K-relation operations, but the *user-visible* zero depends on the
semiring: a monus expression may be 0 in the Boolean semiring yet non-zero
in another m-semiring. ProvSQL's choice to keep such rows visible and defer
evaluation is therefore the right default; filtering is an optimization to
apply only when the annotation is *uniformly* zero (a `gate_zero` root, or a
statically empty disjunction).

### 3.2 Question 2: Boolean provenance and probabilistic evaluation

*What must the Boolean provenance of a tuple be, when the instance contains
NULLs?*

Here there is an unambiguous specification, because Boolean provenance has
an extensional definition. Possible worlds are sub-instances: each base
tuple is present or absent; the *values inside tuples, including NULLs, are
fixed*. Then:

> For every world W (valuation ν of the tuple variables), tuple t must
> satisfy: t ∈ Q(W) under SQL's own 3VL semantics **iff** ν satisfies the
> Boolean provenance of t.

This is well defined for the whole supported fragment, including the
non-monotone parts, and it is what probability, Shapley/Banzhaf, and the
knowledge-compilation pipeline implicitly assume. Every deviation found in
§4.3 is a violation of this criterion, measurable as a wrong probability
(§4.3 gives the numbers: e.g. 0.5 instead of 0.25 for the `NOT IN` answers).

Note what this specification does *not* say: it does not ask ProvSQL to
capture uncertainty about the NULL's value. `NULL` in a world is still
`NULL`; `x = NULL` is u in every world. Value-level uncertainty is a
different feature (§6 P4).

### 3.3 Question 3: NULL as incomplete information

*Should NULL be treated as an unknown value ranging over the domain (Codd
semantics), with provenance/probability quantifying over its possible
values?*

This is the ambitious reading: NULLs become existential variables (labeled
nulls), instances become c-tables, and the natural outputs are
certain/possible answer pairs, or, probabilistically, NULL as a random
variable with a prior. No deployed provenance system does this today;
UA-DB/AU-DB come closest with order-theoretic bounds rather than exact
provenance. This is a research opportunity, not a gap to “fix” (§6 P4), and
it interacts remarkably well with infrastructure ProvSQL already has: the
continuous random-variable subsystem is precisely machinery for “this cell's
value is a distribution”, and `gate_cmp` already reifies comparisons whose
truth is world-dependent.

It is worth being explicit that Questions 1–2 and Question 3 assign
*different semantics to the same symbol*. SQL's NULL conflates “value
exists but unknown”, “not applicable”, and “no rows” (empty-group
aggregates). Any document/specification (P2) should state which reading
ProvSQL implements (today: strictly the SQL-operational one, Questions 1–2).

## 4. Current state of ProvSQL

### 4.1 The (previously implicit) design rules

These rules are design decisions, not accidents of implementation; they
were implicit in the code and are stated here explicitly:

- **R-A (zero rows).** A row with annotation 0 is equivalent to an absent
  row. Rewritten queries may return rows that vanilla SQL would not, carrying
  provenance that evaluates to 0 (e.g. `IN` sublinks with no match, HAVING
  groups failing the predicate). They should be filtered when that is cheap;
  otherwise visible-but-zero is acceptable, since deciding zero-ness can
  require evaluating the provenance (and is semiring-dependent).
- **R-B (NULL token ≡ 1).** A NULL provenance token is interpreted as the
  semiring's multiplicative identity. This is what makes joins between
  tracked and untracked tables work (`provenance_times` drops NULL tokens),
  and what `provenance_evaluate*` does for missing/NULL mapping entries.
- **R-C (delegation).** Deterministic WHERE/JOIN predicates are never
  inspected; PostgreSQL evaluates them (3VL included) and the rewriter
  combines tokens of surviving rows only. (`migrate_probabilistic_quals`
  explicitly leaves `QUAL_DETERMINISTIC` conjuncts alone,
  `src/provsql.c:11010`.)
- **R-D (missing probability ≡ certain).** An input gate never passed to
  `set_prob` has probability 1.0 (`src/MMappedCircuit.h:79`);
  `get_prob` reports NULL for it, but `probability_evaluate` treats it as
  certain.

R-A and R-C are sound (R-C precisely implements the [BCCKZ21] selection rule
for the deterministic fragment). R-B is a reasonable convention but is
currently *not applied consistently*, and one of its current applications is
a bug under the convention itself (§4.3 V3/V4, §4.5). R-D is defensible but
is a NULL-shaped trap for users (silence turns “I forgot to set a
probability” into “certain”).

### 4.2 What is already correct

Verified live and in code (file references from the current tree):

- **WHERE 3VL delegation.** `WHERE b > 5` drops NULL-b rows; their tokens
  simply do not appear. Matches §3.1 exactly.
- **Grouping and duplicate elimination.** `DISTINCT` is rewritten to
  `GROUP BY` and set-operation dedup to an outer `GROUP BY`
  (`rewrite_non_all_into_external_group_by`, `src/provsql.c:6380`), both
  using PostgreSQL's native grouping equality, which is NULL-identical, so
  NULL keys collapse into one group with a ⊕ of all contributing tokens.
  Verified: `DISTINCT a` over two NULL-keyed rows yields `r5 ⊕ r6`;
  `gr UNION gs` merges the NULLs from both sides into `gr2 ⊕ gs1`. This
  coincides with SQL's syntactic-equality semantics for ε. Correct.
- **`count(col)` vs `count(*)`.** The rewriter replaces `count(expr)`'s
  argument by `CASE WHEN expr IS NOT NULL THEN 1 ELSE 0 END`
  (`src/provsql.c:2751-2795`); verified live: `count(b)` produces
  `(1 * r1), (0 * r2), …` semimod entries; `count(*)` produces all 1s.
- **Aggregation gates skip NULLs.** `provenance_semimod` returns NULL for a
  NULL aggregated value (no gate), and `provenance_aggregate` strips those
  placeholders (`array_remove(tokens, NULL)`), so `sum(b)` over
  `{10, NULL, 30, 30, 40, NULL}` yields the circuit `r1*10+r3*30+r4*30+r5*40`
  (read via `sr_formula(sum(b), …)`, the modern compiled readout) with r2/r6
  correctly absent from the semimodule combination, while `count(b)` shows
  them with weight 0 (`…+r2*0+…+r6*0`). The C++ aggregators (`src/Aggregation.cpp`)
  mirror this with a `ValueType::NONE` skip, and the MC sampler documents
  the NaN-as-NULL convention for undefined worlds
  (`src/MonteCarloSampler.cpp:344-368`).
- **`HAVING <agg> IS [NOT] NULL`** is compiled into gate constructions that
  are correct across possible worlds (`having_NullTest_to_provenance`,
  `src/provsql.c:3301-3410`). Verified live: for the group whose rows are
  `r5` (value 40) and `r6` (value NULL), `HAVING sum(b) IS NULL` yields
  `δ(r6) ⊗ (𝟙 ⊖ r5)`, i.e. “some row of the group exists and no
  non-NULL-valued row does”, which is exactly when a world's `sum` is NULL.
  This is the most NULL-sophisticated piece of the codebase and the model
  for how the remaining gaps should be fixed.
- **LEFT JOIN lowering** (`lower_outer_joins`, `src/provsql.c:8112`), on its
  supported shape (two tracked arms, no outer Var references), produces the
  matched arm plus a NULL-padded antijoin arm with monus; verified live:
  `(1,10,1)` annotated `r1 ⊗ s1` and `(1,10,NULL)` annotated
  `r1 ⊖ (r1 ⊗ s1)`. Both rows visible is R-A at work (in each world exactly
  one of them is present). NULL join keys never match, which is correct in
  every world since `NULL = x` is u. The `count(col)` rewrite above is what
  makes COUNT-over-LEFT-JOIN (“the COUNT bug” of the deforestation
  literature) come out right on padded rows.
- **Where-provenance of NULL cells.** A base NULL cell traces to itself (the
  base cell), like any copied value; a projected position that has no source
  yields the empty witness set (`src/WhereCircuit.cpp:178-183`,
  `src/where_provenance.cpp:44-69`). Reasonable Buneman-style behavior.
- **Guard rails elsewhere.** `INTERSECT` fails with an explicit error rather
  than silently mis-evaluating; `provsql.categorical` rejects NULL array
  elements with an actionable message; the RV constructors are STRICT, so
  `provsql.normal(NULL, 1)` is NULL; `expected(NULL)` is NULL; the sublink
  decorrelation code carefully filters NULL-padded antijoin keys
  (`src/provsql.c:9124-9131, 10278-10279`); `add_provenance` backfills NULL
  tokens with fresh UUIDs.

### 4.3 Semantic violations

Each verified live (Appendix A); “expected” means SQL semantics per §2, and,
for probabilities, the possible-worlds criterion of §3.2. Test data:
`gr = {1, NULL}` (tokens gr1, gr2), `gs = {NULL}` (token gs1), all
probabilities 0.5.

**V1. `NOT IN` with NULLs in the subquery (result-set change + wrong
provenance).**
`SELECT a FROM gr WHERE a NOT IN (SELECT a FROM gs)` returns `∅` in vanilla
PostgreSQL; rewritten it returns *both* rows, annotated `gr1` and `gr2`,
each with probability 0.5. Correct Boolean provenance: tuple 1 is an answer
in world W iff gr1 ∈ W and gs1 ∉ W (if gs1 present, `1 NOT IN {NULL}` is u),
i.e. `gr1 ∧ ¬gs1`, probability 0.25; same shape for gr2. This is not
excused by R-A: the annotations are non-zero on rows that SQL excludes.
Root cause: the sublink decorrelation lifts `NOT IN` into an antijoin/monus
over per-column `=` matches, i.e. it implements the “NULL as ordinary
constant” 2VL semantics, conflating `NOT IN` with `NOT EXISTS`. Dually,
`a IN (SELECT …)` over a non-matching or NULL subquery returns rows
annotated `𝟘` — that direction is consistent with R-A (u and f both map to
zero), only the negation is broken, because monus flips f to t but must not
flip u.

**V2. `EXCEPT` / `EXCEPT ALL` with NULL rows (result-set change + wrong
provenance).**
`SELECT a FROM gr EXCEPT SELECT a FROM gs` is `{1}` in vanilla PostgreSQL;
rewritten it returns `{1, NULL}` with the NULL row annotated plain `gr2`
(probability 0.5; correct: `gr2 ∧ ¬gs1`, 0.25, and the row should be absent
on the deterministic instance). Bag variant equally affected, with the
caveat that its conformance target is ProvSQL's documented difference
(§2), not SQL's `max(0, m − n)`: on a left side with two NULL rows and a
right side with one, SQL removes one NULL row, the documented semantics
removes both, and the current rewrite removes neither (verified). The
NULL-matching bug is identical under both readings; only the expected
outputs of bag-variant tests differ.
Root cause is localized and mechanical: `transform_except_into_join`
(`src/provsql.c:7266-7437`) builds the antijoin ON-condition with
`find_equality_operator` (plain `=`, line 7294). SQL's `EXCEPT` matches
syntactically; the condition should be per-column `IS NOT DISTINCT FROM`.
Note the two halves of the same rewrite currently disagree: the outer
dedup GROUP BY treats NULLs as identical (correct), the antijoin match
treats them as never-equal (wrong).

**V3. Lifted RV comparisons with a NULL operand (silently certain).**
`SELECT id FROM m WHERE v > NULL::random_variable` (and joins where one
side's `random_variable` cell is NULL) keeps every row with the comparison
contributing nothing: measured probability 1. Mechanism: the planner lifts
the comparator to `provenance_cmp(...)`, which is STRICT, so a NULL operand
yields a NULL cmp token; `provenance_times` then drops NULL tokens (R-B:
NULL ≡ 𝟙), so the predicate has become “certainly true”. Under SQL 3VL the
comparison is u and the row must be absent (annotation 0). By ProvSQL's own
R-B convention, this is a bug with a clear fix: the *lift itself* must map a
NULL operand to `gate_zero` (or drop the row), never let STRICTness
manufacture a NULL token. Also verified as a baseline: `m1.v < m1.v`
correctly gets probability 0; `v IS NULL` on an RV column works (NullTest is
deterministic, R-C).

**V4. `HAVING <agg> <op> <const>` on a NULL aggregate (NULL token leaks).**
`SELECT a, sum(b) FROM r GROUP BY a HAVING sum(b) > 5`: the all-NULL group
(a = 2, sum NULL, SQL: filtered as u) is returned with `provsql = NULL`.
Under R-B that reads as “certainly passes”; the correct annotation is
`gate_zero`: in *no* world does this group have a non-NULL sum, so the
comparison is never t. (In general the cmp construction must additionally
require “at least one non-NULL-valued row present” in each world it counts
as passing, which is the same δ/monus pattern `having_NullTest_to_provenance`
already implements for `IS NULL`.) This should be made consistent with the
NULL≡1 convention, i.e. result rows should never carry NULL tokens whose
intended meaning is “zero”.

**V5. Outer joins outside the lowered shape (silent mis-tracking).**
`lower_outer_joins` handles only two-relation LEFT/RIGHT/FULL joins between
tracked arms with no outer Var references to the join RTE. Any other outer
join falls through `get_provenance_attributes` (`src/provsql.c:2122-2132`),
where LEFT/RIGHT/FULL are accepted with “nothing to do”, i.e. treated as
inner joins: the NULL-padded rows get the ⊗ of whatever tokens are in scope,
with no warning. Unlike V1–V4 this is not NULL-*triggered* but it is
NULL-*manifested*: the mishandled rows are exactly the padded ones. It
should at minimum be detected and refused/warned like semi/anti joins are.

**V6. Unset probabilities are silently certain (R-D).**
Not a semantics bug: unset = 1.0 is the intended, load-bearing convention.
In typical deployments most tokens never receive a
probability — deterministic tables joined with a few probabilistic ones —
so certainty must be the default, and a strict/erroring mode would break
the common case; none should be added. The only residual awkwardness is
the NULL-shaped API mismatch (`get_prob` returns NULL for “never set”
where `probability_evaluate` uses 1.0), which a documentation paragraph
stating the convention resolves.

### 4.4 Robustness defects (crash / error-instead-of-NULL)

- **B1 (crash, reproducible; deprecated surface).**
  `provsql.aggregation_evaluate` with a NULL token segfaults the backend:
  `src/aggregation_evaluate.c:53` guards `PG_ARGISNULL(1)`–`(9)` but never
  argument 0 before dereferencing it. Reached from ordinary SQL:
  `aggregation_formula(avg(b), …)` where the group's inputs are all NULL
  (avg returns SQL NULL). Two crash-restarts observed during this analysis.
  The function is deprecated (removed from the documentation, removal
  planned), so the practical fix is deletion rather than a guard; until it
  is deleted it remains a superuser-independent crash vector. The modern
  readout, `sr_formula` on the aggregate value, is unaffected: it is STRICT,
  so the same all-NULL-group case yields SQL NULL. Note the regression tests
  (`test/sql/aggregation.sql`, `test/sql/agg_distinct.sql`) still exercise
  the deprecated function through their `aggregation_formula` helper and
  will need porting to `sr_formula` as part of the removal.
- **B2 (latent).** `provenance_evaluate_compiled`
  (`src/provenance_evaluate_compiled.cpp:361-381`) also lacks a
  `PG_ARGISNULL(0)` guard; currently shielded because every SQL-level caller
  (`sr_*`) is STRICT. One future non-STRICT wrapper away from B1.
- **B3.** The `agg_token` cast functions (`src/agg_token.c:134-147`) parse
  the embedded value string unconditionally; an empty/degenerate `val`
  raises a type-input error rather than yielding NULL.
- **B4.** `where_provenance`'s `parse_array`
  (`src/where_provenance.cpp:64`) NULL-guards only the first component of a
  `{first,second}` pair; `stoi` on a literal `"NULL"` second component would
  throw.
- **B5.** `create_gate` treats a NULL children *array* as “no children” but
  does not strip NULL *elements* (`src/provsql_mmap.c:360-363`); callers
  must pre-filter, which is an invariant worth asserting instead.

### 4.5 An internal inconsistency in the NULL-token convention (R-B)

R-B says “NULL token ≡ semiring 1”. The SQL-level combinators actually
implement “NULL ≡ the neutral element of whatever operation is at hand”:

| Combinator | NULL treatment | Effective reading |
|---|---|---|
| `provenance_times` | filtered out | NULL ≡ 1 (matches R-B) |
| `provenance_plus` | filtered out | NULL ≡ **0** |
| `provenance_monus(x, NULL)` | returns x | NULL ≡ **0** |
| `provenance_monus(NULL, y)` | exception | n/a |
| `provenance_delta` | returns `gate_one` | NULL ≡ 1 |

Under a strict R-B, `plus(x, NULL)` ought to be certain (1 ⊕ x), and
`monus(x, NULL)` ought to be `x ⊖ 1 = 0`. The current behaviors are the
*useful* ones for the situations that actually produce NULL tokens today
(untracked tables in ⊗; V4's NULL leaking into a later ⊕; antijoin
non-matches in ⊖), but that is exactly the problem: the convention is
being used as “NULL means whatever makes the common case work”. Once V3/V4
stop manufacturing NULL tokens for “zero” situations, the remaining
legitimate meaning of a NULL token is “untracked/certain” (≡ 1), and
`provenance_plus`/`monus` should either be made consistent with it or, better,
the system should enforce the invariant that NULL tokens never reach ⊕/⊖
(they arise only in ⊗-with-untracked contexts), and error loudly otherwise.
This is a specification decision to record in P2.

## 5. Implications

### 5.1 For the transparency contract

ProvSQL's core promise is that rewriting does not change query answers
(modulo the provenance column, and modulo R-A's zero-annotated extras).
V1 and V2 break the promise on *deterministic* databases, in the exact
corner (negation + NULLs) that [GL17] identify as the place where “queries
that database theory would want us to view as equivalent are hardly
equivalent in real life”. These should be treated with the severity of
wrong-result bugs, independently of any provenance considerations.

### 5.2 Why the damage is confined to the negative fragment (and why that's
exactly where m-semirings live)

There is a clean reason `NOT EXISTS`, WHERE-predicates, and joins are all
correct under R-C while `NOT IN`/`EXCEPT` are not. For a condition θ used
*positively* (keep rows where θ is t), conflating u with f is precisely
SQL's own top-level rule, so 2VL evaluation “NULL never equals anything”
agrees with 3VL. `EXISTS` only asks which rows of the subquery are t, so it
composes; hence `NOT EXISTS` is also right (verified live: Q2 gets exactly
the correct per-world provenance). The divergences appear in exactly two
places:

1. where SQL *switches equality* (set operations, DISTINCT, GROUP BY use
   syntactic NULL-identical matching): handled correctly for grouping,
   incorrectly for the EXCEPT antijoin (V2);
2. where a u escapes through a *negation that the rewriter itself
   introduces* (the `NOT IN` monus lift, V1; the STRICT-cmp NULL swallow,
   V3; the HAVING NULL comparison, V4).

Both places are inside ProvSQL-generated predicates, not user predicates.
So the fix surface is small and does not touch R-C: make ProvSQL's *own*
matching predicates NULL-aware (syntactic where SQL is syntactic; u-to-zero
where SQL filters u). The m-semiring algebra is untouched; monus remains the
right gate; only the expressions feeding it change. This is the practical
payoff of the [GL17]/[RC22] 2VL-compilation theorem.

### 5.3 For Boolean provenance and everything downstream

Probability, Shapley/Banzhaf, knowledge compilation, the safe-query/OBDD
paths, and Monte Carlo all consume the Boolean abstraction of the circuit.
A circuit wrong under §3.2's criterion poisons all of them equally (the 0.5
vs 0.25 measurements in §4.3). There is no additional NULL work to do
*inside* those evaluators: BooleanCircuit has no NULL concept, correctly.
The only NULL-specific risk downstream is B-class robustness (NaN/NULL
conventions in the MC/expectation layer, which are deliberate and
documented in code) and R-D's silent certainty.

### 5.4 For aggregation provenance

The aggregation layer is in good shape semantically (§4.2), with two
documented deviations from SQL that should be kept but written down:
empty-group `sum`/`count` evaluate to 0 (the semimodule's additive identity)
rather than SQL's NULL for `sum`, in the *evaluated worlds* of the MC/HAVING
machinery (`src/MonteCarloSampler.cpp:352-363`); and `min`/`max`/`avg` of
empty worlds are NaN, skipped by moment estimators. These are per-world
conventions, not visible in the SQL answer of the original query, but they
are user-observable through `expected(...)`/HAVING under MC, and belong in
the P2 specification. The V4 fix belongs to this layer too: the group-level
cmp construction needs the “group's aggregate is non-NULL in this world”
factor, for which `having_NullTest_to_provenance` already provides the
template.

## 6. Recommendations

Ordered by phase; each item lists effort (S/M/L) and what it buys.

### P0. Robustness (no semantic decisions needed)

1. **B1** (S): remove `aggregation_evaluate` (deprecation already under
   way); if any release ships before the removal, a one-line
   `PG_ARGISNULL(0)` guard closes the crash in the meantime.
2. **Audit all non-STRICT C entry points** (S): B2 (`provenance_evaluate_compiled`),
   B5 (`create_gate` NULL elements → error), plus a sweep of
   `PG_FUNCTION_INFO_V1` functions for argument-0 guards. Cheap insurance;
   the pattern “protected only by a STRICT wrapper” is one refactor away
   from a crash.
3. **B3/B4** (S): make `agg_token` casts return NULL on unparseable `val`;
   guard the second `parse_array` component.

### P1. Restore result-set and provenance fidelity (the real fixes)

4. **EXCEPT matching** (S/M): in `transform_except_into_join`, build the
   antijoin condition with `IS NOT DISTINCT FROM` (i.e. the negator of the
   `DistinctExpr`), per column, instead of `=`. This single change makes
   `EXCEPT` agree with SQL on NULL rows, makes `EXCEPT ALL` agree with its
   documented semantics (§2: every matching left copy removed — SQL's
   `max(0, m − n)` is deliberately a non-goal), *and* makes the monus
   circuit correct per §§3.1–3.2 (the match disjunction then ranges over
   syntactically equal right rows). Add pg_regress coverage with NULL rows
   on both sides, set and bag variants; the bag-variant expected outputs
   must be authored against the documented semantics, not by copying
   vanilla PostgreSQL output.
5. **NOT IN lift** (M): make the sublink rewrite distinguish `NOT IN` from
   `NOT EXISTS`. The 3VL-correct removal condition for outer row x against
   subquery Q is the disjunction over rows s of Q of
   `(x IS NOT DISTINCT FROM s) OR s IS NULL OR x IS NULL`
   (any of these makes `x NOT IN Q` non-t in a world containing s). In
   circuit terms: `⊗row_token ⊗ (𝟙 ⊖ ⊕_{s matching that condition} token(s))`.
   Note this collapses to the current rewrite when neither side is nullable,
   suggesting a planner-time nullability test (`attnotnull`, expression
   analysis) to keep the common path unchanged. Interim option (S): detect a
   nullable `NOT IN` and refuse with an explicit error, as for INTERSECT;
   silent wrong answers are worse than a missing feature.
6. **RV cmp NULL operands** (S): in the comparator lift, emit `gate_zero`
   (or a planner-time constant-false qual) when either operand is a NULL
   constant, and make `provenance_cmp`'s runtime path yield `gate_zero`
   rather than NULL when a NULL RV cell flows in at execution time. Never
   let STRICTness produce a NULL token meaning “false”.
7. **HAVING cmp on NULL aggregates** (M): emit `gate_zero` for the
   group-passes annotation when the aggregate is NULL on the instance, and
   fold the “some non-NULL-valued row present” factor into the world
   enumeration of `having_semantics` so per-world NULL aggregates count as
   not passing. Reuse the Kn/Kz split from `having_NullTest_to_provenance`.
8. **Outer-join fallthrough** (S): in `get_provenance_attributes`, refuse
   (or at least `provsql_warning`) LEFT/RIGHT/FULL RTE_JOINs that survived
   `lower_outer_joins`, symmetrically with the semi/anti error. Silent
   inner-join treatment of an outer join is the worst of the options.
9. **Zero-filtering** (per R-A), two tiers:
   - *Static* (S): where the rewriter knows the annotation is `gate_zero`
     (e.g. an `IN` lift whose match set is provably empty), drop the row.
   - *Dynamic, user-invoked* (M): a function to force provenance
     evaluation and filter 0-annotated rows,

     ```sql
     provsql.nonzero(token uuid,
                     semiring text DEFAULT NULL,   -- NULL = true zero
                     mapping regclass DEFAULT NULL)
     ```

     called explicitly by the user (`WHERE provsql.nonzero(provenance())`
     or around a subquery). Deliberately **no GUC / no transparent mode**:
     either surface requires a manual step anyway, the per-row evaluation
     cost is a real tradeoff, and an explicit call keeps that tradeoff
     visible and under the user's control instead of a session mode that
     silently changes result sets.

     **Default: true zero.** Since “is this annotation 0” is
     semiring-relative, the only intent-neutral default is zero-ness in
     the **free m-semiring**, whose terms are exactly ProvSQL's circuits
     and which is universal for m-semiring homomorphisms: the circuit is 0
     there iff it evaluates to 0 in *every* m-semiring under *every* leaf
     valuation. Filtering on this can never contradict any downstream
     semiring evaluation. Operationally this is the word problem “term ≡ 0
     modulo the m-semiring axioms” (semiring axioms + Amer's monus axioms
     (A1) `a+(b∸a)=b+(a∸b)`, (A2) `(a∸b)∸c=a∸(b+c)`, (A3) `a∸a=0`,
     (A4) `0∸a=0`). Its decidability appears to be **open**: [GP10]
     construct the free m-semiring abstractly (Birkhoff) with no normal
     forms and no word-problem discussion, and [Suc24] settles only the
     adjacent question, proving that the identities valid in
     (ℕ, +, ·, 0, 1, ∸) are co-r.e.-complete, hence undecidable and not
     finitely axiomatizable (the encoding is literally a zero-test:
     F ≠ G over ℕ iff `1 ∸ ((F∸G)+(G∸F)) = 0` holds there). Derivability
     from the axioms is r.e. by proof search; no more is known. For the
     feature none of this matters: the structural zero-propagation rules
     (`gate_zero`, `0 ⊗ x`, `0 ⊕ 0`, `x ⊖ x`, `δ(0)`, …, essentially the
     `simplify_on_load` peephole folds) are a sound linear-time
     under-approximation, and that suffices — keeping a row whose
     zero-ness cannot be proved is acceptable under R-A; dropping is only
     allowed on proof. Cheap *non-zero certificates* exist if ever useful
     for diagnostics: the free m-semiring surjects onto ℕ[X] with
     monomial-wise truncated minus and onto Bool(X) with `a ∧ ¬b`, both
     easy to evaluate, and t ≠ 0 in either image proves t is not
     universally zero.

     Related caution from [Suc24] for any future circuit-level
     simplification: rules must use only identities derivable from the
     m-semiring axioms. Bag-sound identities such as (A5)
     `(b ∸ a) ⊗ c = (b ⊗ c) ∸ (a ⊗ c)` fail in some m-semirings, and the
     full set of bag-sound identities is not finitely axiomatizable, so
     “it holds in ℕ” is never a justification for a peephole rule.

     **Named-semiring strengthenings.** An explicit `semiring` argument
     evaluates the circuit in that (m-)semiring (unmapped leaves default
     to 1_K, consistent with the mapping-NULL ≡ one() convention) and
     tests against its zero:
     - `'boolean'`, no mapping (all leaves true): “present in the vanilla
       SQL answer on this instance”, the deterministic membership test —
       linear time, no knowledge compilation. This, not the true-zero
       default, is the mode that restores vanilla result sets (e.g. it is
       what filters the LEFT JOIN antijoin arm `r1 ⊖ (r1 ⊗ s1)`, which is
       not universally zero).
     - `'counting'`, all leaves 1: bag multiplicity; needed for `… ALL`
       variants where Boolean and ℕ zero-ness diverge
       (`(x ⊕ x) ⊖ x` is 0 in 𝔹, 1 in ℕ). Caveat: the rewrite spreads a
       group's multiplicity across per-row tokens, so multiplicity is a
       group-level notion; the per-row filter is sound but “which copy
       survives” is not defined row-by-row.
     - other m-semirings via the `sr_*` dispatcher (e.g. tropical:
       nonzero = derivable at finite cost) — K-relation support in K, with
       no vanilla-SQL reading.
     - Related but separate mode: `probability_evaluate(token) > 0`
       (“possibly present” across worlds) — a different quantifier, not a
       semiring zero test; worth exposing alongside, not inside, nonzero.

     Circuits containing ⊖/δ restrict the named-semiring form to
     m-semirings defining them, the same restriction the `sr_*` evaluators
     enforce. Cost model to document: one circuit walk per candidate row,
     amortized by the per-backend gate cache for shared subcircuits.
     Worth a dedicated design note before implementing.

### P2. Specification and documentation

10. **Write the NULL semantics down** (M): a user-doc chapter and a short
    dev-doc section stating: R-A (zero rows visible and why), R-B and its
    scope after P1 (NULL token = untracked/certain, never “false”; ⊕/⊖
    behavior), R-C, R-D, the empty-group per-world conventions (§5.4), the
    supported-fragment table annotated with NULL behavior per construct,
    the deliberate `EXCEPT ALL` bag-difference semantics (§2 — public in
    [SMS25], but it belongs in the user docs, not only in the paper), and
    the deliberate INTERSECT/refusal list. Much of §4 of this document can
    be reused. The [BCCKZ21] selection rule and the §3.2 possible-worlds
    criterion should be stated as the normative semantics.
11. **Resolve the R-B inconsistency** (S/M): after P1 items 6–7 remove the
    “NULL means zero” producers, decide and enforce: NULL tokens are only
    legal as ⊗ operands (untracked sources); `provenance_plus` and
    `provenance_monus` raise (or WARN once) on NULL inputs instead of
    silently guessing. This turns future V3/V4-class bugs into loud errors.

### P3. Testing

12. **pg_regress `null_semantics.sql`** (S/M): the Appendix A battery as a
    test file: G&L Q1/Q2/Q3, EXCEPT [ALL] with NULLs (bag expectations per
    the documented difference semantics, §2), IN/NOT IN, NULL
    grouping keys, count/sum/avg over NULLs, HAVING IS [NOT] NULL and cmp,
    LEFT JOIN padding, RV-NULL comparisons, probabilities with hand-computed
    expected values (the 0.25s of §4.3).
13. **Differential harness** (M): randomized [GL17]-style validation, run
    occasionally rather than in CI: generate random queries over small
    NULL-containing instances; compare `provsql.active = off` vs `on` modulo
    rows whose Boolean provenance evaluates to false; and compare
    `probability_evaluate` against brute-force world enumeration
    (`possible_worlds` already exists) on instances small enough to
    enumerate. This is the tool that would have caught V1–V4 and will catch
    their relatives (e.g. `NOT IN` under `boolean_provenance`
    optimizations, update-provenance interactions) without hand-writing
    every case.

### P4. Beyond fidelity: the lifting ladder

The overall design space is best described as **one mandatory baseline plus
an opt-in lifting axis**, not as two competing options:

- The **baseline** is SQL-conformant NULL semantics (P0–P3): NULLs are
  values, predicates over them follow 3VL, provenance annotates the tuples
  SQL returns (the [BCCKZ21] rule, the §3.2 possible-worlds criterion).
  This is not optional relative to what follows: every lifted semantics
  states its guarantees relative to per-world *SQL* evaluation, so a
  translation layer that conflates `NOT IN` with `NOT EXISTS` poisons the
  lifted layer's certain/possible classifications too. And since lifting
  can only ever be opt-in (per column or per query), ordinary NULLs keep
  flowing through the system and need the baseline as the default.
- The **lifting axis** reinterprets (selected) NULLs as ProvSQL-managed
  unknown values, in the c-table tradition. It is a ladder, not a single
  feature; the rungs below are ordered by increasing semantic ambition and
  implementation cost.

One architectural warning applies to every rung ≥ 2: a lifted unknown
*forks from SQL exactly at SQL's syntactic-equality corners*. A lifted
unknown is not NULL for `IS NULL`/`COALESCE` (it has a value, merely
unknown), and two lifted unknowns are not identical for
`GROUP BY`/`DISTINCT`/set operations: grouping becomes *conditional*,
whereas SQL collapses NULLs into one group. Those are precisely the places
where rule R-C (delegate evaluation to the PostgreSQL executor) stops being
available, which is why lifting is architecturally more expensive than “the
same rewriting with a different leaf gate”: it changes which operators can
be delegated at all.

14. **Rung 1: certain/possible provenance pairs** (best value-to-cost).
    NULLs stay SQL NULLs; no quantification over their values. Each
    NULL-induced u decision is resolved both ways, yielding two circuits
    per tuple: an under-approximation (worlds where t is *certainly* an
    answer under 3VL) and an over-approximation (worlds where it might be,
    if unknown comparisons resolved favorably). This is the provenance
    analogue of UA-DBs' bounds and of the certain-answer approximations of
    Libkin et al.; upper minus lower is exactly “answers you got because of
    a NULL”, a compelling explanation feature for Studio. Machinery cost is
    a small multiple of the baseline: the over-approximation is the naive
    rewrite with favorable (`IS NOT DISTINCT FROM`-style) matching, the
    under-approximation is the P1-corrected one, and R-C remains available
    for both (each half is an ordinary 2VL rewrite). Paper-plus-feature.
15. **Rung 2: independent unknowns, with distributions (imputation).**
    Codd-style lifting: each selected NULL cell becomes a fresh unknown,
    with no sharing between cells. Comparisons over it become reified
    unknown-truth-value leaves, which is structurally what `gate_cmp`
    already does for random variables; giving the unknown a prior
    (`provsql.null_as(col, provsql.normal(μ,σ))` or a per-column policy)
    makes everything computable with the existing mixture/`gate_cmp`/MC/
    analytic machinery. This subsumes probabilistic imputation and is a
    feature no comparable system offers. Sharp edges: the delegation
    breakdown above (lifted cells in grouping keys, set operations, or
    `IS NULL` tests need dedicated rewrites or an explicit refusal), and
    the default question (SQL filters u rows; RV-style lifting keeps them
    with a `gate_cmp`): lifting must be an explicit opt-in that documents
    this switch. Distinctive-feature territory.
16. **Rung 3: shared (named) unknowns: c-tables proper.** The same unknown
    occurring in several cells, making `x = x` certain and enabling
    provenance over incomplete databases in the Imieliński–Lipski sense.
    This adds a genuinely new leaf kind (equality atoms between unknowns
    over an infinite domain), makes certain answers coNP-hard in general,
    and raises open questions for the knowledge-compilation pipeline
    (treewidth of condition circuits, interaction with the safe-query
    rewriter). A research program; worth a feasibility note before any
    commitment, and only after rungs 1–2 exist to build on.
17. **Mechanized grounding** (orthogonal to the ladder). [BCCKZ21]
    formalizes exactly ProvSQL's algebra (K-relations + δ/semimodule
    aggregates) over exactly the right NULL semantics (Datacert's 3VL), for
    the positive fragment; the monus extension and the §3.2 criterion would
    be a natural joint formalization target, with ProvSQL as the executable
    counterpart. Rung 1 would also be a clean formalization object (two
    K-algebras and an ordering between them).

### Suggested order

P0 items are an afternoon and remove a crash. Of P1, item 4 (EXCEPT) has
the best cost/benefit and item 5's interim refusal is cheap; items 6–8
close the remaining silent-wrong-answer holes. P2 should accompany P1 in
the same release notes. Item 13 is the highest-leverage medium-term
investment. On the P4 ladder: rung 1 first (it reuses both the naive and
the corrected rewrites, so it falls out of P1 almost for free and yields
the most user-visible value); rung 2 when a concrete imputation use case
appears; rung 3 only as a research undertaking.

## Appendix A. Experiment transcript (condensed)

Environment: PostgreSQL 18, ProvSQL 1.11.0-dev, database `nulltest`
(kept in place). Tables: `r(a,b) = {(1,10),(2,NULL),(3,30),(3,30),
(NULL,40),(NULL,NULL)}` tokens r1…r6; `gr(a) = {1, NULL}` tokens gr1, gr2;
`gs(a) = {NULL}` token gs1; `s(a) = {1, NULL}` tokens s1, s2; `m(id, v)` with
v = `normal(0,1)` (id 1) and NULL (id 2). All probabilities 0.5 where set.

| # | Query (rewritten) | Vanilla result | ProvSQL result (annotation) | Verdict |
|---|---|---|---|---|
| 1 | `r WHERE b > 5` | 4 rows | same, r1/r3/r4/r5 | OK (3VL delegation) |
| 2 | `r WHERE b IS NULL` | r2, r6 rows | same | OK |
| 3 | `gr WHERE a NOT IN (SELECT a FROM gs)` | ∅ | `{1: gr1, NULL: gr2}`, P = 0.5 each | **V1** (expected: absent; per-world `gri ∧ ¬gs1`, P = 0.25) |
| 4 | `gr WHERE NOT EXISTS (… s.a = gr.a)` | {1, NULL} | same, gr1/gr2, P = 0.5 | OK (correct per world) |
| 5 | `gr EXCEPT gs` | {1} | `{1: gr1, NULL: gr2}`, P = 0.5 | **V2** (NULL row: expected absent / `gr2 ∧ ¬gs1`) |
| 6 | `r.a EXCEPT ALL gr.a` | {2,3,3,NULL} | 6 rows incl. both NULLs (r5, r6) and `1` (monus gate) | **V2** bag variant; `1`-row is R-A-acceptable, NULL rows are not. Post-fix target is `{2,3,3}` (documented difference, §2), not SQL's `{2,3,3,NULL}` |
| 7 | `gr WHERE a IN (SELECT a FROM gs)` | ∅ | both rows annotated 𝟘 | OK per R-A |
| 8 | `SELECT DISTINCT a FROM r` | 4 rows | NULL group = `r5 ⊕ r6` | OK (syntactic grouping) |
| 9 | `gr UNION gs` | {1, NULL} | NULL = `gr2 ⊕ gs1` | OK |
| 10 | `gr INTERSECT gs` | {NULL} | error “Set operations other than UNION and EXCEPT not supported” | OK (explicit refusal) |
| 11 | `sum(b) GROUP BY a` (`sr_formula`) | – | `r1*10`; NULL for a=2; `r3*30+r4*30`; `r5*40` | OK (NULL inputs skipped) |
| 12 | `count(b)` vs `count(*)` (`sr_formula`) | 4 / 6 | `…+r2*0+…+r6*0` vs all weight 1 | OK |
| 13 | `aggregation_formula(avg(b),…)`, group all-NULL | – | **backend segfault** | **B1** (repro: `aggregation_evaluate(NULL,…)`; deprecated function, removal planned) |
| 14 | `HAVING sum(b) IS NULL` | group a=2 only | all groups, e.g. a=NULL: `δ(r6) ⊗ (𝟙 ⊖ r5)` | OK (per-world correct; extras are R-A) |
| 15 | `HAVING sum(b) > 5` | groups 1,3,NULL | + group a=2 with **NULL token** | **V4** (expected `gate_zero`) |
| 16 | `r LEFT JOIN s ON r.a = s.a` | 6 rows | 7 rows: `r1 ⊗ s1` and `r1 ⊖ (r1 ⊗ s1)` both visible | OK per R-A; NULL keys never match: OK |
| 17 | `m WHERE v > NULL::random_variable` | ∅ | both rows, cmp dropped, P = 1 | **V3** (expected: absent / 0) |
| 18 | `m1.v < m2.v` join with NULL rv | – | (1,1): P = 0 ✓; every pair involving NULL: P = 1 | **V3** |
| 19 | `provsql.normal(NULL,1)`, `expected(NULL)`, `categorical` with NULL element | – | NULL, NULL, clean error | OK |
| 20 | `get_prob` (unset) vs `probability_evaluate` | – | NULL vs 1.0 | R-D / V6 (document) |
| 21 | `where_provenance` over NULL cells | – | NULL cell traces to its base cell | OK |

## Appendix B. Key code sites

| Concern | Site |
|---|---|
| Deterministic quals left alone (R-C) | `src/provsql.c:11010` (`QUAL_DETERMINISTIC`) |
| EXCEPT antijoin `=` matching (V2) | `src/provsql.c:7266-7437`, esp. `find_equality_operator` at 7294 |
| Set-op / DISTINCT dedup via native GROUP BY | `src/provsql.c:6380-6437`, `6236-6255` |
| Outer-join lowering; silent fallthrough (V5) | `src/provsql.c:8112-8210`; `2122-2132` |
| `count(expr)` IS-NOT-NULL rewrite | `src/provsql.c:2751-2795` |
| HAVING IS [NOT] NULL gates | `src/provsql.c:3301-3410`; null-filtered plus at 3234-3272 |
| RV comparator lift, no NULL guard (V3) | `src/provsql.c:3605-3634` (`rv_OpExpr_to_provenance_cmp`) |
| NULL-token combinator behavior (§4.5) | `provenance_plus`/`times`/`monus`/`delta` in `sql/provsql.common.sql` (times 1087-1128, monus 1136-1168, delta 4493-4514) |
| Aggregation NULL skipping | `provenance_semimod` / `provenance_aggregate` (`sql/provsql.common.sql:4531-4610`); `src/Aggregation.cpp:109-209` |
| Mapping NULL/missing ≡ one() | `src/GenericCircuit.hpp:104-111`; `src/provenance_evaluate_compiled.hpp:75-96` |
| Default probability 1.0 (R-D) | `src/MMappedCircuit.h:79`; NaN→1 in `src/BooleanCircuit.cpp:157-172` |
| Crash B1 | `src/aggregation_evaluate.c:53` (no `PG_ARGISNULL(0)`) |
| Latent B2 | `src/provenance_evaluate_compiled.cpp:361-381` |
| agg_token cast parsing (B3) | `src/agg_token.c:134-147` |
| where-prov NULL literal (B4) | `src/where_provenance.cpp:44-69`; `src/WhereCircuit.cpp:178-183` |
| MC NaN-as-NULL conventions | `src/MonteCarloSampler.cpp:313-368` |

## Appendix C. References

- [SMS25] A. Sen, S. Maniu, P. Senellart. *ProvSQL: A General System for
  Keeping Track of the Provenance and Probability of Data.* Benchmark
  paper, in preparation (`~/git/students/aryak/benchmark_writeup`).
  Defines ProvSQL's multiset difference (t ↦ 0 if t occurs in q₂, else its
  full left multiplicity; matches `NOT IN`, NULL-free) and motivates the
  divergence from SQL's `EXCEPT ALL` by tractability of provenance
  computation.
- [GL17] P. Guagliardo, L. Libkin. *A Formal Semantics of SQL Queries, Its
  Validation, and Applications.* PVLDB 11(1), 2017.
- [RC22] W. Ricciotti, J. Cheney. *A Formalization of SQL with Nulls.*
  J. Automated Reasoning 66, 2022 (Coq development `nullSQL`).
- [BCCKZ21] V. Benzaken, S. Cohen-Boulakia, É. Contejean, C. Keller,
  R. Zucchini. *A Coq Formalization of Data Provenance.* CPP 2021
  (DOI 10.1145/3437992.3439920).
- T. J. Green, G. Karvounarakis, V. Tannen. *Provenance Semirings.* PODS
  2007. — [GP10] F. Geerts, A. Poggi. *On database query languages for
  K-relations.* J. Applied Logic 8(2), 2010 (m-semirings; free m-semiring
  via Birkhoff, no decidability discussion). — Y. Amsterdamer,
  D. Deutch, V. Tannen. *Provenance for Aggregate Queries.* PODS 2011.
- [Suc24] D. Suciu. *Different Differences in Semirings.* OASIcs vol. 119
  (Tannen festschrift), 2024: Amer/Bosbach axiomatization of monus;
  identities of (ℕ,+,·,0,1,∸) are co-r.e.-complete (Thm 21); (A5) fails
  in some m-semirings. — K. Amer. *Equationally complete classes of
  commutative monoids with monus.* Algebra Universalis 18, 1984. —
  B. Bosbach. *Komplementäre Halbgruppen.* Math. Annalen 161, 1965.
- T. Imieliński, W. Lipski. *Incomplete Information in Relational
  Databases.* J. ACM 31(4), 1984. — L. Libkin. *SQL's Three-Valued Logic
  and Certain Answers.* ACM TODS 41(1), 2016.
- S. Feng, A. Huber, B. Glavic, O. Kennedy. *Uncertainty-Annotated
  Databases: A Lightweight Approach for Approximating Certain Answers.*
  SIGMOD 2019; and S. Feng et al., *Efficient Uncertainty Tracking for
  Complex Queries with Attribute-level Bounds.* SIGMOD 2021 (AU-DBs).
