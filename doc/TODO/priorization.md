# Prioritisation: open TODO items, tested on the current build

A cross-file prioritisation of the open work tracked in this directory.
For **every** open point, this file gives three things:

1. a **concrete example** (a query / evaluation that the point impacts);
2. the **current behaviour**, observed by running that example on the
   installed build (verbatim psql output, not reconstructed); and
3. an **after** sketch of what the behaviour would become once the point
   is implemented.

Tested against **ProvSQL 1.10.0** on **PostgreSQL 18**. Each cluster was
probed in its own throwaway database; output blocks are verbatim, and
where a value is the analytically known answer it is noted inline. The
verbatim blocks were captured on 2026-06-09 against the pre-release build
(then the `conditioning` branch, since merged and released as 1.10.0);
every open-item behaviour here was re-verified against the released line
on 2026-06-18 and still holds.

Source plans: [`probability-evaluation.md`](probability-evaluation.md),
[`safe-query-followups.md`](safe-query-followups.md),
[`conditioning.md`](conditioning.md),
[`scalar-subqueries.md`](scalar-subqueries.md),
[`bounded-treewidth-data.md`](bounded-treewidth-data.md),
[`continuous_distributions.md`](continuous_distributions.md),
[`studio.md`](studio.md), [`case-studies.md`](case-studies.md).

## What the testing changed about the priorities

Two findings from running the examples reshuffle the plan:

- **Most case-study coverage gaps were chrome, not engine.**
  `COUNT(DISTINCT)`, `string_agg`, `FILTER`, `LATERAL`, `UPDATE`+`undo`,
  and the temporal SRFs all already produce correct provenance, so those
  case-study extensions landed as pure tutorial / Studio work. Window
  functions warn-and-degrade (documented caveat), and only
  UDF-internal-lineage and join-on-aggregate remain genuinely
  engine-blocked.
- **Robustness, not just features.** Several exhaustive-method runs
  (`possible-worlds` on the Θ(n)-treewidth in-star self-join at n≈25–40;
  the pseudo-poly SUM enumeration on 40 large incommensurate values)
  drove the backend to an out-of-memory crash + auto-recovery. The
  conditioning refusal path, by contrast, was re-checked and errors
  **cleanly** (session survives). So the structural-factoring items
  (Route 3, the SUM FPTRAS) have a *stability* payoff, not only a speed
  one — they remove circuits that can OOM the server.

## Priority ordering (cross-file)

### Tier 3 — larger / workload-gated

5.  HAVING exact residuals: coupled branch-spanning SUM (prob §2),
    shared-contributor UNION/EXCEPT (prob §3).
6.  Studio: result-table batch evaluation (studio §2), notebook polish.
7.  CS4 extensions: direct `get_valid_time`, the `UPDATE`-for-DELETE+INSERT
    swap (window-function caveat verified).
8.  Continuous distributions: the §F.1 refactor, then the quick wins
    (Gamma, Log-normal, quantiles, RV-vs-RV comparators, GMM, and the
    §B.6 comparison-event surface — whose `probability` alias and
    projected-event rewrite are the smallest, refactor-independent items).
9. Scalar subqueries: different-`(Q,corr)` multi-sublinks (hard error
    today), `GROUP BY` body (hard error today).
10. Bounded-treewidth Route C leftovers / Route 3 / non-recursive
    triggers (the in-star and chain shapes) — gated on a real workload,
    but see the robustness note (Route 3 removes an OOM-prone circuit).

### Tier 4 — research bets / explicitly deferred

11. Conditioning: re-based materialised posterior, Shapley over evidence
    (verified refused today), soft/weighted conditioning (not a priority).
12. SUM-safe rounding FPTRAS (prob §1) — research-grade, rare-event SUM.
13. General-semiring width-aware evaluator + Route A automaton (btw §5,§6);
    Monet construction, per-branch FBDD orders, discrete RV families,
    self-joins-with-inversion (safe §2,§3,§5,§6); copulas / MVN /
    stochastic processes / do-calculus (continuous §A.5,§D.2,§D.3);
    UDF-internal lineage and join-on-aggregate provenance (the CS9
    blockers).

---

# Detailed findings (by source plan)

<!-- The sections below are the per-point evidence: example, verbatim
     current behaviour, and after-sketch. -->

## Probability evaluation (probability-evaluation.md)

### 1. SUM-safe rounding FPTRAS

The apx-safe SUM corner is exact when the reachable-sum support is small,
approximate by world-sampling otherwise; the rare-event-efficient
*rounding rejection FPTRAS* is unimplemented. Large incommensurate values
blow the pseudo-poly support (`kMaxSumSupport`).

**Example**
```sql
-- (a) small support: exact weighted-sum pre-pass
SELECT g, probability_evaluate(provenance()) FROM smn GROUP BY g HAVING sum(val) > 20;
-- (b) 40 large incommensurate values, p=0.02: rare-event SUM
SELECT g, probability_evaluate(provenance()) FROM sm2 GROUP BY g HAVING sum(val) > 30000000;
```

**Current behavior**
```
-- (a) small support --
NOTICE:  gate_cmp expression was shortcut by probability-side pre-pass (1 weighted-sum):
         provenance circuit reduced from 29 to 1 gates
 g |    p     | method
---+----------+--------
 1 | 0.371094 | sieve
-- (b) 40 large incommensurate values -- killed after >3 minutes (pseudo-poly support blow-up; OOM)
-- (b') 18 commensurate values, p=0.03, true p≈3e-19 --
 g |           p           |       m
---+-----------------------+----------------
 1 | 3.018929360056198e-19 | compilation:d4
```
Small support → exact weighted-sum pre-pass (29→1 gates). Large
incommensurate values → the distinct-sum support explodes and the query
hangs (and OOM-crashed the backend; see the robustness note). Commensurate
values instead survive as a 1490-gate Boolean circuit decided by
`compilation:d4`; there is no rare-event-efficient sampler.

**After** — `AggFptras` (rounded-sum semiring `S_{n²+1}` + Alg 5.2.1
random-world generator + Lemma 7 accept-test) gives a relative `(ε,δ)`
estimate with a sample count independent of `p`, on a safe SUM skeleton,
instead of the pseudo-poly bail or full compilation.

### 2. HAVING coupled branch-spanning SUM

`sum(b*c + b + c)` over `B ⋈ C` is neither additively nor
multiplicatively separable (rank-≥2 weight tensor), so the marginal-vector
pre-pass declines and the cmp self-gates back to Boolean enumeration.

**Example**
```sql
SELECT bb.k, probability_evaluate(provenance())
FROM bb JOIN cc ON bb.k=cc.k GROUP BY bb.k HAVING sum(bb.b*cc.c + bb.b + cc.c) > 40;
```

**Current behavior**
```
=== separable sum(b*c) ===   NOTICE: pre-pass (1 safe-join aggregate) 28 -> 1 gates;  p=0.562500
=== separable sum(b+c) ===   NOTICE: pre-pass (1 safe-join aggregate) 28 -> 1 gates;  p=0.500000
=== coupled sum(b*c+b+c) === (no pre-pass NOTICE)
 k |    p     |           m
---+----------+-----------------------
 1 | 0.562500 | sieve,possible-worlds
```
Both separable shapes get the exact pre-pass; the coupled shape emits no
pre-pass NOTICE and is resolved by full Boolean enumeration — fine on a
5-tuple toy, `#P`-style in general.

**After** — either a rank-≥2 coupled-weight evaluator over the per-factor
joint `(sum,count)` distribution, or (if shown `#P`-hard) routing the
coupled SUM to the apx-safe sampler instead of enumeration.

### 3. HAVING UNION/EXCEPT over a join reusing a base tuple

`(R⋈S) ∪ (R⋈T)` gives `(r∧s)⊕(r∧t)`, non-read-once on the shared `r`. The
independent-contributor case is now exact; only a base tuple shared
*across* a group's contributors stays `#P`-hard.

**Example**
```sql
-- independent: distinct R-row per output tuple ; shared: one R-row in both branches
SELECT g, probability_evaluate(provenance()) FROM u_indep GROUP BY g HAVING count(*) >= 2;
SELECT g, probability_evaluate(provenance()) FROM u_shared GROUP BY g HAVING count(*) >= 2;
```

**Current behavior**
```
=== INDEPENDENT contributors ===     g=1: pw 0.14063 = deflt 0.14063 (exact, contributorExactMarginal)
=== SHARED-R across contributors === g=1: pw 0.42188 = deflt 0.42188 | method possible-worlds (enumeration)
```
Independent contributors are exact and match possible-worlds; the
shared-`r` case falls to enumeration (correct on a small circuit,
`#P`-hard in general).

**After** — only the shared-across-contributors case remains; it is the
safe-query / read-once problem and stays sampler-bound (or read-once when
the rewriter succeeds). No cheap exact route expected.

### 4. RV probability transparency

`P(X<c)` for a `random_variable` is meant to route through
`stopping-rule`; today the default goes through the analytic / fixed-sample
paths and `stopping-rule` is selectable only by name.

**Example**
```sql
SELECT probability_evaluate(rv_cmp_lt(normal(0,1), as_random(1.0)));                  -- default
SELECT probability_evaluate(rv_cmp_lt(normal(0,1), as_random(1.0)),'stopping-rule');
```

**Current behavior** (true CDF `Φ(1)=0.841345`)
```
-- default --
NOTICE:  gate_cmp expression was shortcut by probability-side pre-pass (1 analytic): 3 -> 1 gates
 0.841345 | possible-worlds
-- stopping-rule -- kind=relative eps=0.1 samples=1363 -> 0.856088
```
The default resolves `P(X<c)` through the **analytic** pre-pass (exact
0.841345) and never selects `stopping-rule` (which only reachable by name
gives a looser 0.856088). The gap is narrow here (analytic is exact and
better); it matters for RV comparisons the analytic curves cannot close.

**After** — routing the RV *probability* case into the catalog so
`stopping-rule` and the analytic-exact route compose under `chooseAndRun`
makes the RV path catalog-driven like the Boolean ones, picking
analytic-exact when available and `stopping-rule` otherwise.

---

## Safe-query rewriter (safe-query-followups.md)

### 2. UCQ(OBDD): functional dependencies (the H-query)

**Example**
```sql
CREATE TABLE hs(x int, y int, PRIMARY KEY(x,y));   -- PK on S
SET provsql.provenance='boolean';
SELECT probability_evaluate(provenance()) FROM hr, hs, ht WHERE hr.x=hs.x AND hs.y=ht.y GROUP BY ();
```

**Current behavior**
```
-- explicit inversion-free method: ERREUR: requires an inversion-free certificate on the provenance root
-- boolean rewriter: the lineage is the UNCHANGED three-way gate_times (no per-atom DISTINCT wraps):
     p     
-----------
 0.3046875
```
The H-query has no single root class, so both the inversion-free detector
and the boolean rewriter decline; the correct 0.3046875 comes from the
generic d4 path, not a structured route.

**After** — a per-atom anchor scheme (read-once via the PK FD) or the
per-branch builder of §3 lets a structured path recognise the
FD-tractable H-query instead of dropping to d4.

### 3. UCQ(OBDD): per-branch decision orders (h₁ / FBDD)

**Example**
```sql
-- h1: (R join S) UNION (S join T) on shared S, all tuples p=0.5
SELECT probability_evaluate(provenance(),'inversion-free') FROM (
   SELECT k_r.x AS g FROM k_r,k_s WHERE k_r.x=k_s.x
   UNION ALL SELECT k_t.y FROM k_s,k_t WHERE k_s.y=k_t.y) u GROUP BY ();
```

**Current behavior**
```
-- explicit inversion-free method: ERREUR: requires an inversion-free certificate
-- default chooser:   0.59375
```
h₁ carries an inversion, so no single global OBDD order exists and the
detector declines; the value still computes (0.59375) via compilation/d4.

**After** — choosing the Shannon decision variable per branch (FBDD /
Decision-DNNF) certifies h₁ with a poly d-DNNF and subsumes the FD case of
§2; a research extension to the single-order builder.

### 4. Discrete random_variable extensions

**Example**
```sql
SELECT provsql.poisson(3);  SELECT provsql.binomial(10,0.5);  SELECT provsql.geometric(0.3);
```

**Current behavior**
```
ERREUR:  la fonction provsql.poisson(integer) n'existe pas
ERREUR:  la fonction provsql.binomial(integer, numeric) n'existe pas
ERREUR:  la fonction provsql.geometric(numeric) n'existe pas
```
`random_variable` constructors are continuous (`normal`/`uniform`/
`exponential`/`erlang`) + `categorical` + `mixture`; no discrete
parametric family.

**After** — a Poisson variant in the `gate_rv` `extra` blob + a
closed-form CDF in the analytic evaluator unlocks the sum-of-Poissons
identity (a `HAVING sum(rate)>k` collapses to one Poisson CDF), the
discrete twin of the Normal-sum closed form.

### 5. Möbius / Monet non-hierarchical UCQ

**Example**
```sql
SELECT probability_evaluate(provenance(),'compilation') FROM (
   SELECT 1 AS g FROM nr,ns WHERE nr.x=ns.x
   UNION ALL SELECT 1 FROM ns,nt WHERE ns.y=nt.y) u GROUP BY g;
```

**Current behavior**
```
-- compilation (d4):     0.734375
-- possible-worlds:      0.734375   (ground truth)
```
The safe non-hierarchical UCQ is handed to natural-lineage d4 and computes
correctly; there is no Monet/inclusion-exclusion construction in the
pipeline.

**After** — Monet's PTIME deterministic-decomposable circuit (negation via
`gate_monus`) is an alternative construction, but d4 is as fast on the
workloads seen; deferred until a benchmark shows d4 choking on a safe
non-hierarchical UCQ.

### 6. Hierarchical-detector follow-up: self-joins without PK/constant rescue

**Example**
```sql
SET provsql.provenance='boolean';
SELECT e1.a, probability_evaluate(provenance()) FROM edge e1, edge e2
  WHERE e1.a=e2.b AND e1.b=e2.a GROUP BY e1.a;   -- symmetric closure
```

**Current behavior**
```
NOTICE:  ProvSQL: not inversion-free: class at inconsistent column positions within one relation (inversion / self-equality)
-- lineage UNCHANGED (raw self-join gate_times); values via the generic path:
 symmetric closure: a=1 0.25, a=2 0.25     path R(x,y),R(y,z): a=1 0.375, a=2 0.25
```
Neither PK-unification nor disjoint-constant rescue fires; the rewriter
declines (explicit NOTICE) and the generic path produces the values.

**After** — these carry inversions (not even in UCQ(OBDD)); resolving them
needs Monet-style d-DNNF (§5) or the full Dalvi-Suciu dichotomy, deferred
since the shapes tend to be recursive-query territory the CQ-only rewriter
already excludes.

### 7. TID/BID: UNION ALL of compatible BID legs

**Example**
```sql
SELECT repair_key('bid_a','k');  SELECT repair_key('bid_b','k');  -- both bid, block_key={1}
SET provsql.classify_top_level=on;
CREATE TABLE t_union AS SELECT k FROM bid_a UNION ALL SELECT k FROM bid_b;
SELECT (get_table_info('t_union'::regclass)).*;
```

**Current behavior**
```
=== classifier on the UNION ALL ===  NOTICE: ProvSQL: query result is OPAQUE
=== derived relation metadata ===    kind | block_key   ->  (empty)
-- contrast: a single BID leg projecting its key -> NOTICE: query result is BID
```
The `setOperations` shape trips, so the union classifies OPAQUE and the
CTAS hook records no kind/block_key — correct (a leg-A and leg-B row
sharing `k` are independent, not exclusive), just a missed optimisation.

**After** — two opt-in paths: disjoint-range certification (legs with
provably disjoint key ranges keep `k` as block key, ~100–150 LOC, no
schema change) or a GUC-gated `__provsql_leg_id` composite-key synthesis
(~200–300 LOC); neither implemented, to avoid injecting an unasked-for
column.

---

## Conditioning & scalar subqueries (conditioning.md, scalar-subqueries.md)

### 1. Baseline: medical-test Bayes (SHIPPED, works)

**Example**
```sql
SELECT repair_key('world','id');   -- (disease,positive) joint as a BID block
WITH d AS (
  SELECT (SELECT provenance() FROM world WHERE disease  GROUP BY id) AS dtok,
         (SELECT provenance() FROM world WHERE positive GROUP BY id) AS ptok)
SELECT probability_evaluate(dtok | ptok) AS p_disease_given_pos,
       probability_evaluate(ptok | dtok) AS p_pos_given_disease FROM d;
```

**Current behavior**
```
 p_disease_given_pos | p_pos_given_disease
---------------------+---------------------
            0.153846 |            0.900000
```
P(disease | positive) = 0.1538 (≠ the 0.9 sensitivity), the textbook
posterior; the binary `|` evaluates P(A∧B)/P(B) exactly and
correlation-aware. Verified baseline the open items build on.

**After** — no change; this is the shipped surface.

### 2. Materialised re-based discrete posterior cannot re-enter algebra (OPEN)

**Example**
```sql
SELECT cond(b1, b3) INTO TEMP cstore ...;          -- store a conditioned uuid token
SELECT get_gate_type(condtok), probability_evaluate(condtok) FROM cstore;   -- OK
SELECT probability_evaluate(provenance_times(condtok, other_token)) FROM cstore;  -- refused
```

**Current behavior** — store / re-condition / evaluate is fine
(`gate = conditioned`, p = 0.6); composing under `times` is refused:
```
ERREUR:  ProvSQL: The requested semiring does not support conditioning: P(·|C) = P(·∧C)/P(C)
needs a normalising division no general semiring provides.  A conditioned token is evaluable
only in the measure interpretation (probability_evaluate, or the random-variable / agg_token
distribution evaluators).
```
The conditioned token is terminal. (Re-checked separately: this refusal is
**clean** — the session survives; the crashes seen elsewhere were OOM on
exhaustive methods, not this path.)

**After** — a *re-based* posterior (MayBMS assertion) folds P(C) into a
fresh independent representation so the result re-enters join/union.
Deferred until a persisted posterior must be composed onward.

### 3. Shapley / Banzhaf over a conditioned token are refused (OPEN)

**Example**
```sql
SELECT provsql.shapley(cond(b1, b3), b1);
```

**Current behavior**
```
ERREUR:  ProvSQL: shapley/banzhaf: conditional Shapley / Banzhaf values are not supported --
a conditioned token (X | C) cannot be passed to shapley() / banzhaf().  Compute the index on
the unconditioned token, or use probability_evaluate for the conditional probability P(X|C)
```
Refused cleanly: Shapley/Banzhaf are linear over the semiring,
conditioning is not.

**After** — connecting code over a `gate_conditioned` root plus the
existing Shapley machinery, once the conditional Shapley value *definition*
is settled (research track).

### 4. Soft / weighted conditioning has no overload (OPEN, not a priority)

**Example**
```sql
SELECT cond(b1, b3, 0.9);   -- a 3-arg / weighted conditioning
```

**Current behavior**
```
ERREUR:  la fonction cond(uuid, uuid, numeric) n'existe pas
```
All eight `|` overloads are binary; no `gate_conditioned(X,event,weight)`.

**After** — a finite-weight likelihood-weighted gate reweights worlds
instead of restricting them (soft evidence; tractable rare-evidence). It
lands as a parameter on the existing gate, with the dispatcher routing
out-of-[0,1] synthetic intermediates exact-only. Hard conditioning is the
weight→∞ limit.

### 5. Nested scalar sublink in target-list arithmetic — SHIPPED (target-list); WHERE form open

**Example**
```sql
-- qq probs 0.5; subquery nested in "+ 1" (not a direct target entry)
SELECT rr.a, (SELECT qq.x FROM qq WHERE qq.k = rr.k) + 1 AS v1 FROM rr;
```

**Current behavior** — the sublink is now lifted to `choose()` in place under
the `+ 1`, and the agg_token arithmetic carries qq's provenance through it as a
`gate_arith` token (no warning).  The value is tracked ("`101 (*)`"); its circuit
root is `arith` over the `choose(qq.x)` aggregate, reaching qq's input leaves.
Covers `+ - * /`, unary `-`, compound nesting (`*2+3`), and int→numeric casts /
division (`/ 4.0`, `::numeric + 0.5`).  See `decorrelate_scalar_sublinks` /
`oj_tl_sublink_in_arith` in `src/provsql.c`, Part 22 of
`test/sql/scalar_subquery.sql`.

```
 a  |   v1    |   p
----+---------+--------
 10 | 101 (*) | 1.0000   (row exists via the LEFT JOIN; provenance is in v1's token)
```

**Still open** — a sublink nested in WHERE arithmetic
(`… WHERE (SELECT …)+1 > k`) still passes through with a warning: the comparison
would have to lift to a HAVING `cmp` gate over `choose(qq.x)+1` rather than over a
bare `choose(qq.x)`.  Opaque (non-cast) function-argument nestings stay a
passthrough by design.

### 6. Correlated sublinks over different (Q, corr) (hard error)

**Example**
```sql
SELECT rr7.a, (SELECT q1.x FROM q1 WHERE q1.k = rr7.k) AS a1,
              (SELECT q2.y FROM q2 WHERE q2.k = rr7.a) AS a2 FROM rr7;
```

**Current behavior**
```
ERREUR:  ProvSQL: Subqueries (EXISTS, IN, scalar subquery) not supported
```
Only the *same*-`(Q,corr)` case coalesces onto one LEFT JOIN; distinct
bodies are a hard error (the chain `R ⟕ Q1 ⟕ Q2` is not lowered).

**After** — generalise `lower_outer_joins` to a left-deep chain
`R ⟕ Q1 ⟕ Q2 …`, or wrap-and-recurse materialising each decorrelation as
a derived `R'` before the next sublink.

### 7. GROUP BY body in a scalar sublink (hard error)

**Example**
```sql
SELECT rr.a, (SELECT sum(qq.x) FROM qq WHERE qq.k = rr.k GROUP BY qq.k) FROM rr;
```

**Current behavior**
```
ERREUR:  ProvSQL: Subqueries (EXISTS, IN, scalar subquery) not supported
```
A body `GROUP BY` conflicts with the decorrelation's own `GROUP BY R.*`.

**After** — genuinely new body-grouping machinery (nested grouping under
the decorrelation's regroup), not a reshaping of the existing rewrites.
Deferred.

---

## Bounded-treewidth data (bounded-treewidth-data.md)

Observable signals: the route's NOTICEs (`SET provsql.verbose_level=20`
for certification, `>=10` for fallback) and the root gate type (a
certified reachability root is wrapped `assumed` with `info1=1`).

### 2. Route 3 — in-star self-join threshold-2 blow-up

`FROM e a, e b WHERE a.x <> b.x` over a star yields the symmetric
threshold-2 function `⋁_{i<j} eᵢ∧eⱼ`, a Θ(n)-treewidth sum-of-products;
the equivalent `HAVING count(*) >= 2` is handled exactly by
`CountCmpEvaluator`.

**Current behavior** (circuit size; same probability both ways)
```
  n |  phrasing  | root | gates |   prob
----+------------+------+-------+----------
  5 | self-join  | plus |    65 | 0.812500    possible-worlds, 1.4 ms
  5 | HAVING>=2  | cmp  |    27 | 0.812500    CountCmpEvaluator, 0.1 ms
 40 | self-join  | plus |  4720 |  (OOM)      possible-worlds: 90 s timeout already at n=25
 40 | HAVING>=2  | cmp  |   167 | 1.000000    CountCmpEvaluator, 1.9 ms
```
The self-join circuit grows Θ(n²) gates / Θ(n) treewidth; absorption
cannot touch it, so the chooser falls to exhaustive `possible-worlds`,
which is exact but blows up (and OOM-crashed the backend at n=40). The
`HAVING` phrasing stays a single linear `cmp` gate.

**After** — a structural recogniser rewrites the threshold/`gate_plus`
shape into the linear-size sequential counter (`seen1`/`seen2`), emitted
with a d-DNNF certificate, matching what `CountCmpEvaluator` already does
for the count phrasing. Pure recognition; scope if a workload insists on
the self-join form (note the stability payoff: it removes an OOM-prone
circuit).

### 3. Non-recursive self-join chain → recursive CTE

A 2-hop reachability written as a non-recursive self-join chain
(`a.dst=b.src`) does NOT hit the bounded-hop certified route (which fires
only for the recursive-CTE form).

**Current behavior** (`verbose_level=20`)
```
-- non-recursive chain: NO reachability NOTICE; roots are plain 'times'   (1,3) 0.25, (2,4) 0.25
-- recursive bounded-hop CTE:
NOTICE:  ProvSQL: Recursive CTE 'reach' recognised as reachability over pchain
NOTICE:  ProvSQL: recursive CTE "reach" compiled along a tree decomposition of pchain
   node 0: 1.0   node 1 (1 hop): 0.5   node 2 (2 hops): 0.25
```
Both give the right 2-hop value, but only the recursive CTE is recognised
and compiled along the decomposition.

**After** — a non-recursive trigger rewrites the self-join chain (with
DISTINCT) into the depth-bounded recursive CTE the bounded-hop route
already compiles. Worthwhile only if such shapes appear in real workloads.

### 4. Shared-support join-defined edges

Join-defined edges are accepted only when token supports are pairwise
disjoint; a join sharing a base tuple across edges makes them correlated
and the route declines.

**Current behavior**
```
-- DISJOINT (verbose 20):
NOTICE:  ... recognised as reachability over a join-defined edge query / compiled along a tree decomposition
   reach(2)=0.63, reach(3)=0.3024
-- SHARED (verbose 10):
NOTICE:  ProvSQL: reachability route for "reach" fell back to the generic fixpoint
         (join-defined edges share base tuples (their supports overlap), so they are not independent)
   still correct: reach(3)=reach(4)=0.25
```
The disjoint case is certified; the shared-support case declines with an
explicit message and falls back to the generic fixpoint (still correct).

**After** — a faithful variables-in-the-decomposition DP with
late-branching states accepts shared-support edges (tractability scaling
with sharing), plus static disjointness certification from keys/FDs to
skip the dynamic walk.

### 5. General (non-absorptive) m-semiring width-aware evaluation

The certified circuits evaluate linearly only for absorptive semirings; a
general m-semiring (counting, why) over a reachability token is refused on
the absorptive-wrapped token and works only via the generic compiled
evaluator under `'semiring'`, which does not exploit treewidth.

**Current behavior**
```
-- absorptive min-cost over the certified circuit (linear):   node 4 = 3   (via 1->2->4 = 1+2)
-- counting / why over the SAME absorptive-wrapped token: REFUSED
ERREUR:  ProvSQL: ... The requested semiring is not absorptive; the wrapped sub-circuit only
  represents the absorptive quotient ... on acyclic data, re-run under the 'semiring' provenance class.
-- under 'semiring' class (generic evaluator, root = plain 'plus'):
   counting node 4 = 6 witnesses ; why = {{1,2},{1,4}}
```
Absorptive min-cost runs linearly; counting/why are refused on the
absorptive token and, re-run under `'semiring'`, are computed generically
(correct on this small acyclic graph, blow up with size).

**After** — a width-aware semiring evaluator (a bag-by-bag DP over the
semiring carrier) lets a bounded-treewidth `GenericCircuit` evaluate
general m-semirings in width-bounded time, with a Mumick-Shmueli finiteness
story gating recursion; the absorptive case is the compile-once /
evaluate-per-semiring counterpart of Ramusat-Maniu-Senellart NodeElimination
on the treewidth axis.

### 6. Route A — full MSO/CQ automaton (deferred research)

A fixed non-recursive CQ whose lineage mirrors a self-join through a star
centre has data treewidth 1 but circuit treewidth n; the
tree-decomposition method does not stay linear.

**Current behavior** (the treewidth-exploiting method, by graph size)
```
=== n=10 ===  0.989258   tree-decomposition: 243 ms
=== n=15 ===  ERREUR:  ProvSQL: Treewidth greater than 10
=== n=20 ===  ERREUR:  ProvSQL: Treewidth greater than 10
```
Although `tw(data)=1`, the plan-built circuit has treewidth Θ(n): the
method costs 243 ms at n=10 and refuses at n≥15; the default chooser
instead falls to exhaustive `possible-worlds` (exact but exponential, and
OOM at large n). The circuit is built along the join plan, so nothing caps
its treewidth ahead of building it.

**After** — Route A compiles the query to a bottom-up tree automaton run
over a tree decomposition of the Gaifman graph, emitting one gate per
transition (treewidth `O(|Q|)`, independent of `|I|`); the two missing
moves are restricting the state to the active bag (forget = drop) and
capping the summary (the in-star "at least two" becomes a counter capped at
2). The missing piece is the query-to-automaton compiler (research-grade
even for CQ/UCQ), deferred until lighter levers prove insufficient.

---

## Continuous distributions (continuous_distributions.md)

This file is a roadmap of mostly-unbuilt features; the findings are mostly
"constructor X does not exist", each made concrete with the verbatim error
and the realistic post-implementation surface.

### §A.1 Gamma / Chi-squared
```sql
SELECT provsql.gamma(2.5, 0.4);  SELECT provsql.chi_squared(3);
```
```
ERROR:  function provsql.gamma(numeric, numeric) does not exist
ERROR:  function provsql.chi_squared(integer) does not exist
```
Erlang (integer shape) exists; the generalisation to real-valued shape is
the delta. **After** — `gamma(shape,rate)` as a new `DistKind::Gamma`
(closed-form moments, equal-rate sum-closure); `chi_squared(k)` is sugar
for `gamma(k/2,0.5)`.

### §A.2 Log-normal
```sql
SELECT provsql.lognormal(0.0005, 0.02);
```
```
ERROR:  function provsql.lognormal(numeric, numeric) does not exist
```
**After** — `DistKind::LogNormal` (closed-form `exp(mu+σ²/2)`),
product-closure in log-space; the `exp`/`log`↔Normal bridge (§B.3) is the
prerequisite for the simplifier to exploit it.

### §A.4 Discrete families (Poisson / Binomial / Geometric)
```sql
SELECT provsql.poisson(3);  SELECT provsql.binomial(10,0.3);  SELECT provsql.geometric(0.4);
```
```
ERROR:  function provsql.poisson(integer) does not exist        (+ binomial, geometric)
```
`categorical` handles enumerated supports; no analytical discrete family.
**After** — three `DistKind` variants; Poisson/fixed-p Binomial gain
sum-closure, Geometric inherits Exponential's memoryless truncation; the
`gate_cmp` path needs a discrete CDF (overlaps safe-query §5).

### §A.5 Multivariate Normal
```sql
SELECT provsql.mvnormal(ARRAY[0.0,1.0], ARRAY[[1.0,0.5],[0.5,1.0]]);
```
```
ERROR:  function provsql.mvnormal(numeric[], numeric[]) does not exist
```
`random_variable` is strictly scalar; no vector-valued gate.
**After** — a table-valued constructor returning one `random_variable`
column per component over a shared Cholesky-decomposed covariance; an
architectural move (vector scopes in the gate ABI + `FootprintCache`),
prerequisite for §D.2/§D.3.

### §B.1 Quantiles / inverse CDF
```sql
SELECT provsql.quantile(provsql.normal(0,1), 0.95);   -- and inverse_cdf / cdf / ppf
```
```
ERROR:  function provsql.quantile(random_variable, numeric) does not exist     (all four names)
```
No inverse-CDF surface; dispatchers are `expected`/`variance`/`moment`/
`central_moment`/`support`. **After** — a polymorphic `quantile(rv,p)`
(Normal via Beasley-Springer-Moro already in the sampler; Exp/Uniform by
inversion; Gamma/Beta by root-finding), one virtual call under §F.1.

### §B.2 RV-vs-RV analytical comparisons
```sql
INSERT INTO rv VALUES (provsql.normal(0,1), provsql.normal(1,1));    -- analytic
INSERT INTO rv VALUES (provsql.exponential(2.0), provsql.exponential(3.0));  -- MC fallback
SELECT probability_evaluate(provenance()) FROM rv WHERE x < y;
```
```
-- Normal-Normal: 0.76025  (exact Φ(1/√2); stable across rv_mc_samples → analytic)
-- Exp-Exp:       0.4036 / 0.2 / 0.3936  (varies with rv_mc_samples → MC; exact = 2/5 = 0.4)
```
`normalDiffDecide` handles Normal-Normal; all other family pairs return
NaN and fall back to MC (Exp-Exp confirmed by `rv_mc_samples=5` giving a
clearly wrong 0.2). **After** — lookup entries per family pair
(Exp-Exp = `λ_X/(λ_X+λ_Y)`), a `ComparatorRuleRegistry` keyed by
`(DistKind,op,DistKind)` under §F.1.

### §B.3 Function application beyond +−×÷
```sql
SELECT provsql.exp(provsql.normal(0,1));   -- and log / sqrt / abs
```
```
ERROR:  function provsql.exp(random_variable) does not exist     (all)
```
Only `+ - * /` (`gate_arith`) exist. **After** — a `gate_apply(rv,op)`
gate with a monotonic-transform whitelist (bounds transform for
RangeCheck/Analytic, function applied per draw for MC); the
`exp(normal)→lognormal` bridge unlocks §A.2 sum closure.

### §B.4 Order statistics: `MIN`/`MAX` aggregates and `greatest`/`least`
```sql
SELECT max(x) FROM rv_col;              -- x is random_variable (over rows)
SELECT provsql.expected(GREATEST(x,y,z)) FROM d;   -- three RV columns, one row
```
```
ERROR:  function max(random_variable) does not exist     (min, percentile_cont likewise)
ERROR:  could not identify a comparison function for type provsql.random_variable
```
`SUM`/`AVG`/`PRODUCT` over `random_variable` exist; neither the
`MIN`/`MAX`/ordered-set *aggregates* nor a same-row *variadic*
`greatest`/`least` do (`GREATEST` needs btree ordering, which the RV type
has no business providing). The motivating question — `E[max(x,y,z)]` for
three `uniform(0,1)` columns, exact `3/4` — needs the variadic form, not
the aggregate. **Reachable today** by decomposing over which column is the
max via the shipped `|` surface: `expected(x | (x>y AND x>z))` returns
`0.750` (`E[x | x is max]`; MC-backed, `rv_mc_samples>0`). **After** — one
shared `PROVSQL_ARITH_MAX`/`_MIN` gate (append-only opcode) feeding both an
RV-aware `min`/`max` aggregate (accumulating the order-statistic
distribution: CDF of the min = `1−∏(1−Fᵢ)`; i.i.d. Exponential min is
Exponential at the summed rate) and a variadic `greatest`/`least`; MC gives
correct answers immediately, the closed-form `E[max]` is the §B.2-adjacent
refinement; `percentile_cont` needs §B.1.

### §B.6 Comparison events as first-class values
```sql
SELECT x > y FROM d;                              -- project the event
SELECT probability_evaluate(x>y AND x<z) FROM d;  -- probability of a predicate
```
```
ERROR:  random_variable comparison must be rewritten by the ProvSQL planner hook (is provsql.active off?)
ERROR:  function probability_evaluate(boolean) does not exist
```
RV comparisons are rewritten into their `gate_cmp` token only in WHERE /
JOIN / HAVING quals and the `|`-RHS; elsewhere they hit
`random_variable_cmp_placeholder` and raise. So a projected `x>y` and a
naturally-written `probability(x>y AND x<z)` both fail, even though
`expected(x | (x>y AND x>z))` works (only because `|` has a
`(random_variable, boolean)` overload the hook rewrites). Today's escape
hatch is explicit token building:
`probability_evaluate(provenance_times(rv_cmp_gt(x,y), rv_cmp_lt(x,z)))` →
`0.16732` (≈ `1/6`, the ordering `y<x<z`). **After** — (1) broaden the
hook to lift RV comparisons in the target list / general argument
positions, surfacing `x>y` as its event token; (2) a
`probability(boolean)` placeholder overload the hook rewrites, mirroring
`random_variable_cond_predicate`; (3) a trivial `probability` alias bound
to the same `probability_evaluate` C symbol, for the concise surface
(`expected`/`variance`/`support`). All three are quick wins.

### §C.1/§C.2/§C.3 Empirical / GMM distributions
```sql
SELECT provsql.gmm(ARRAY[0.3,0.5,0.2], ARRAY[120,380,1200], ARRAY[40,90,250]);
SELECT provsql.empirical_samples(ARRAY[3.21,3.18,3.24]);  SELECT provsql.empirical_cdf(...);
```
```
ERROR:  function provsql.gmm(...) does not exist          (empirical_samples, empirical_cdf likewise)
```
`mixture(p,x,y)` exists as a binary mixer; no array-GMM sugar, no
data-driven gates. **After** — `gmm(...)` is pure wrapper sugar over
`gate_mixture`/`gate_rv` (zero new gate types); `gate_empirical_samples` /
`gate_empirical_cdf` are two new `Distribution` subclasses (CDF-vs-constant
becomes an exact Bernoulli leaf).

### §D.2 Copulas / correlation
```sql
SELECT provsql.copula(provsql.normal(0,1), provsql.normal(1,1), 'gaussian', ARRAY[0.7]);
```
```
ERROR:  function provsql.copula(...) does not exist
```
No `gate_copula`; correlation enters only via a shared base `gate_rv`
leaf. **After** — a `gate_copula` carrying marginal sub-circuits + family
+ params (joint-aware closed form where available; Cholesky/inverse-CDF
otherwise; `FootprintCache` treats it as a shared footprint). MVN (§A.5) is
the Gaussian-copula special case.

### §D.3 Stochastic processes
```sql
SELECT t, value FROM provsql.brownian(sigma => 1.0, steps => 100) LIMIT 5;   -- and ar1(...)
```
```
ERROR:  function provsql.brownian(...) does not exist
```
**After** — table-valued constructors returning `SETOF (t int, value
random_variable)` compiling to correlated `gate_rv` chains via §D.2
(AR(1) a Gaussian-copula chain, Brownian the cumulative-sum case).
Requires §D.2.

### §F.1 Per-distribution class hierarchy (internal refactor)
```
$ ls src/distributions/   ->  No such file or directory
```
Family discrimination is a `DistKind` enum with `switch` blocks across six
files (`AnalyticEvaluator` 12, `HybridEvaluator` 19, `MonteCarloSampler` 8,
`RangeCheck` 9, `Expectation` 4, `RvAnalyticalCurves` 5; `RandomVariable`
19 parse/serialise). Adding Gamma today is a coordinated patch across all
seven. **After** — a `src/distributions/` directory, one file per family
implementing an abstract `Distribution` interface + a `DistributionRegistry`
(and `ClosureRuleRegistry` / `ComparatorRuleRegistry` for pairwise
behaviour); a new family becomes one file + one registry entry. Prerequisite
for §§A.1, A.2, A.4, B.1, B.3, C.1–C.3.

---

## Studio & case studies (studio.md, case-studies.md)

These two plans are about UI / documentation, so the test confirms the
**backing SQL capability** each item relies on.

### 2. Result-table batch evaluation — backing works
```sql
SELECT id, val, probability_evaluate(provenance()) FROM contrib_t;   -- one pass, per row
```
Per-row evaluation works in a single SQL pass; the gap is a Studio button
that batch-posts all UUIDs from the displayed `provsql` column and appends
a result column. **After** — a result-table extension over `/api/evaluate`,
reusing the existing dispatch.

### 7. CS4: UPDATE + undo — backing works (with a token subtlety)
```sql
SET provsql.update_provenance='on';
UPDATE ministers SET role='Interior' WHERE id=1;
SELECT undo('<query token from update_provenance>'::uuid);
SELECT id, name, role, get_valid_time(provsql, 'ministers') FROM ministers;
```
`UPDATE`+`undo` round-trips: after undo, the "Interior" row gets a closed
interval and "Finance" gains a new open interval (a two-period
`tstzmultirange`). `undo()` takes the *query* token from
`update_provenance`, not the row's `provsql` (passing the wrong token
no-ops with a notice — the likely user mistake to document). The `undo`
round-trip is already shown in the rebuilt CS4; what remains is a step
calling `get_valid_time` directly to make the reversion visible and an
explicit `UPDATE` replacing the DELETE + INSERT pair. No engine change.

### 8. CS9 (future): UDFs and join-on-aggregate — genuinely engine-blocked

**(a) Provenance through a UDF.** A pure scalar UDF on one tracked row
keeps that row's token (correct); a UDF that *reads a second tracked table*
drops the lineage:
```sql
CREATE FUNCTION apply_rate(v numeric, cat text) RETURNS numeric LANGUAGE SQL AS
  $$ SELECT v*multiplier FROM rates WHERE category=cat $$;   -- rates is tracked
SELECT id, apply_rate(val, cat), where_provenance(provenance()) FROM items;
-- formula lists only the items attributes; the rates dependency is silently dropped
```
**(b) Join-on-aggregate.** Each row gets its own token; the aggregate
subquery's dependency on co-group rows is not propagated:
```sql
SELECT s.id, s.dept, g.dept_total, probability_evaluate(provenance())
FROM sales s JOIN (SELECT dept, sum(amount) dept_total FROM sales GROUP BY dept) g ON s.dept=g.dept;
-- id=1 (dept A) gets 0.9 (own token), not the group-conditioned 0.9*0.8 = 0.72;
-- where_provenance raises: ProvSQL: Where-provenance does not support gates of type delta
```
Both confirm the documented "blocked" status. **After** — the rewriter
must add a times-gate between the outer row's token and the
aggregate/UDF-internal combined token; structurally like the LATERAL
multi-output issue but harder (the aggregate grouping is many-to-many).
Future engine work; the CS9 case study can document the gap with the
numbers above until then.

---

## Robustness note (surfaced while testing)

Two exhaustive-method runs drove PostgreSQL to an out-of-memory crash +
automatic recovery (data intact): the `possible-worlds` evaluation of the
Θ(n)-treewidth in-star self-join at n≈25–40 (btw §2 / §6), and the
pseudo-poly SUM enumeration over 40 large incommensurate values (prob §1).
These are the precise pathologies the structural items target, so Route 3
(btw §2) and the SUM FPTRAS (prob §1) carry a *stability* payoff beyond
speed. The conditioning-refusal path was separately re-checked and errors
cleanly (the session survives) — it is not implicated. A defensive
guard (a treewidth / support-size ceiling that refuses with a clear error
instead of OOM-ing) would be a cheap robustness win independent of the
structural rewrites.
