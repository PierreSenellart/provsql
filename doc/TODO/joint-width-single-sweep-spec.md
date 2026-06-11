# Joint-width UCQ compiler — single-sweep (shared-work) per-answer evaluation

Design + implementation notes for the M4 "single sweep" optimisation, and a
benchmark of it against the current **per-binding** (multiple-pass) approach.

## 0. Where we are

Per-answer evaluation (`SELECT head, probability_evaluate(provenance()) …
GROUP BY head`) is currently **per-binding**: for every answer the head is
pinned to its value and the *Boolean* compiler is rerun. Two SQL surfaces:

- **`ucq_joint_answers(query, head_vars, head_tuples, facts…)`** — the facts
  are gathered **once** (passed as columnar arrays) and the C compiler is run
  once **per head tuple** (each pins the head with a `Sel` atom + a CERTAIN
  fact and calls the Boolean `ucq_joint_evaluate`). One gather, *k* compiles.
- **transparent `ucq_joint_provenance_answer(desc, head_vars, head_vals, …)`**
  — substituted by the planner once per output group, so the relations are
  **re-gathered per answer**. *k* gathers, *k* compiles.

So the cost of *k* answers is, per binding,

```
  per-group transparent :  k · (gather + encode + decompose + DP)
  ucq_joint_answers     :      gather + k · (encode + decompose + DP)
```

## 1. What "single sweep" can share

The work an answer does decomposes into four stages, in increasing share-ability:

| stage | depends on the head value? | shareable across answers? |
|---|---|---|
| gather (SQL reads the relations) | no | **yes** — gather once |
| encode (`JointEncoding`, slice walk) | no (without the `Sel` pin) | **yes** |
| decompose (joint graph + min-fill tree decomposition) | no | **yes** |
| DP (bag sweep, gate emission) | **yes** | partially (see §3) |

The current `Sel`-pin makes *encode* and *decompose* depend on the head (the
pin adds a fact + atom whose element is the head value), which is why
`ucq_joint_answers` cannot share them. Replacing the `Sel` pin with an
**in-DP head pin** — a `(var → element)` constraint that `closeDisjunct`
enforces, rejecting any binding of the head variable to a different element —
removes that dependency: the encoding and the decomposition become identical
for every answer, and only the DP is rerun, pinned, per head:

```
  shared-decomposition  :  gather + encode + decompose + k · DP_pinned
```

This is the implementation here. It is **correct by construction**: a pinned
DP run computes exactly `P(∃ witness with head = v)` — the same marginal the
`Sel`-pin computes — so each answer's probability is unchanged; only the
shared stages are paid once. The 223-test suite (all Boolean, i.e. the
empty-pin path) guards that the Boolean compiler is untouched.

## 2. The in-DP head pin

`closeDisjunct` is the *only* place a query variable receives a fresh binding
from a fact (the join/forget transitions only propagate existing bindings).
Pinning head variable `H` to element `v` is therefore a single guard there:
when an atom would bind `H` to bag position `p`, require `domain[p] == v`,
else drop that extension. The pin is threaded read-only through the sweep;
with an empty pin the guard never fires and the Boolean behaviour is verbatim.

A head with several variables (a multi-column `GROUP BY`) pins each component.
Non-answer head values (probability 0) fall out as the empty/false root and
are dropped, exactly as `ucq_joint_answers` drops them.

## 3. The full top-down single-DP (implemented: `compileAnswersOneDP`)

The shared-decomposition path still pays *k* bottom-up DP sweeps. The thesis's
multi-output construction (Amarilli, tel-01345836, §4.2.9) shares the DP
**itself**, emitting one circuit root per answer from a single sweep. The
obstacle was that the bottom-up state's hom-set **mixes head bindings**: a
single state carries many partial maps that may bind the head variable to
*different* elements. The implementation makes the head a **state-level key**:

- **The head is never existentially projected.** A head variable is bound like
  any other while in the bag, but when its element leaves the decomposition it
  is set `DONE` with its element **value recorded** in the code's `hval`
  (rather than collapsed to a value-less `DONE`). So different head bindings
  live in different states and never merge.
- **Completed answers are tracked per head-tuple.** Instead of the global `sat`
  collapse, a code that reaches the full witnessed mask is a *completion*: its
  head tuple is added to the state's `done` set and the code is discharged.
  `done` is part of the state key, so worlds are partitioned by which answers
  they satisfy.
- **An answer is emitted as its own root** when its head elements have all left
  the tree **and** no surviving partial code can still witness it. The second
  condition is the subtle one: a witness can complete *after* the head element
  is forgotten (the head value lives on in `hval`, and the rest of the witness
  may sit in a higher bag). Emitting on element-departure alone splits one
  answer's provenance across the lifts of its several witnesses, double-counting
  the overlap. At the root (empty parent) nothing can complete, so every
  remaining answer is emitted there.
- **All answer roots share one circuit.** A single probability pass values them
  all: `dDNNF::probabilityEvaluation` caches per-gate values, so evaluating the
  *k* roots in sequence touches each shared gate once.

The candidate answers are **discovered** by the sweep (no candidate list). The
state count grows with the number of *concurrently live* answers (those with a
pending witness crossing the current bag), which is bounded by the treewidth,
not by *k*. So the whole evaluation is **one pass, linear in the data** rather
than *k* passes.

C surface: `UCQJointCompiler::compileAnswersOneDP` + SRF
`ucq_joint_answers_onedp(query, head_vars, facts…)`. Data-graph regime; the
correlated regime keeps the shared-decomposition path (§2) -- the head-pin
merged DP -- the same `done`/`hval` machinery would carry over but is not yet
wired through `mergedCompile`.

### Measured (same H0 family, w=2, p=0.5)

| k | per-binding ms | swept ms | onedp ms | onedp vs swept |
|---|---|---|---|---|
| 16   | 9.1      | 2.7     | 1.1   | 2.5× |
| 64   | 109.2    | 29.5    | 2.7   | 11×  |
| 256  | 1596.0   | 446.7   | 8.5   | 53×  |
| 1024 | 27047.7  | 7297.7  | 34.1  | 214× |
| 4096 | 490685.0 | 129590.0| 152.0 | 853× |

`onedp` is **linear** in *k* (≈4× per 4× answers) while per-binding and swept
are **quadratic** (each of the *k* passes sweeps all *k·w* facts). At k=4096 the
single DP is 853× faster than the shared-decomposition sweep and ~3200× faster
than per-binding, and the gap widens without bound. All three agree on every
answer. Cross-checked against per-binding in `ucq_joint_answers` section 19 and,
on a treewidth-2 instance with correlated answers, against the standard ladder.

## 4. Scope of the implementation

Both regimes share the decomposition the same way; the in-DP head pin is the
single primitive that makes it sound (it lives in `closeDisjunct`, which both
regimes use to bind a query variable from a fact).

- **Data-graph (TID/BID) regime.** New C SRF `ucq_joint_answers_swept(query,
  head_vars, facts…)` → `SETOF (head int[], probability float8)`:
  gather/encode/decompose once, loop the pinned DP over the distinct head
  tuples in the data. Cross-checked against `ucq_joint_answers` (per-binding):
  identical answers and probabilities.
- **Correlated regime (internal-gate tokens).** `mergedCompile` gained the
  same optional `shared_td` / `shared_elim` / `head_pin` parameters and threads
  the pin through its `closeFact` → `closeDisjunct`; `compileImpl` forwards them
  and `compileAnswers` no longer rejects the correlated encoding. The joint
  graph spans **data and circuit slice**, but neither depends on the head, so
  it is built once and only the pinned merged DP (the per-world gate valuation +
  hom DP) is rerun per answer. New C SRF `ucq_joint_answers_swept_tracked(query,
  head_vars, head_tuples, fact_rel, fact_elems, fact_arity, fact_tokens)`: the
  fact tokens are real provenance gates, walked through the circuit **once**
  (shared `buildTrackedFacts` helper), the correlated encoding + decomposition
  built once, each answer a pinned sweep. Cross-checked against the standard
  ladder per group (`test/sql/ucq_joint_correlated.sql`): exact, e.g. two
  answers q(0)=0.125 (shared e0) and q(5)=0.5 (three facts sharing e3).

## 5. Benchmark (single sweep vs multiple passes)

`test/bench/ucq_joint_single_sweep_bench.sql`: a per-answer family with a
growing number of answers *k*, reporting wall-clock for

1. per-group transparent (k gathers + k compiles),
2. `ucq_joint_answers` per-binding (1 gather + k compiles),
3. `ucq_joint_answers_swept` shared-decomposition (1 gather + 1 decompose + k
   pinned DPs),

all three agreeing on the answer set and the marginals.

### Measured (q(x) :- R(x), S(x,y), T(y), w=2 private witnesses, p=0.5)

| k (answers) | transparent ms | per-binding ms | swept ms | per-binding / swept |
|---|---|---|---|---|
| 16   | 110.4  | 9.2     | 2.8    | 3.3× |
| 64   | 1359.5 | 121.0   | 35.0   | 3.5× |
| 128  | 5250.2 | 478.4   | 127.2  | 3.8× |
| 256  | (skipped) | 1826.3  | 500.7  | 3.6× |
| 512  | (skipped) | 7151.1  | 1831.4 | 3.9× |
| 1024 | (skipped) | 28232.6 | 7278.3 | 3.9× |

The single sweep is a steady **~3.3–3.9×** over per-binding, the ratio widening
with *k* as more identical encode+decompose passes are amortised onto one. Both
remain ~quadratic in *k* here (the dense encoding still carries all *k·w* facts,
and the pinned DP still sweeps them once per answer — the shared-decomposition
pass removes the *repeated* encode+decompose constant, not the per-answer DP;
killing the latter is the deferred top-down single-DP of §3). Per-binding is in
turn 10–40× faster than the re-gather-per-group transparent path, whose constant
is dominated by *k* planner substitutions and *k* full relation gathers. The
transparent path is skipped past k=128 (it is O(k) gathers of O(k·w) data, i.e.
the row already takes 5 s at k=128 and would dominate the run).

### Correlated regime (R(x) gated by an internal times of shared base events)

`test/bench/ucq_joint_single_sweep_correlated_bench.sql` times the same SRF
called once-per-answer (k walks + k decompositions + k DPs) against one call
with all candidates (1 walk + 1 decomposition + k pinned merged DPs) — the two
differ only by the shared walk+encode+decompose over the joint data+circuit
graph:

| k (answers) | per-binding ms | swept ms | per-binding / swept |
|---|---|---|---|
| 16  | 27.9   | 12.3   | 2.3× |
| 64  | 452.4  | 171.0  | 2.6× |
| 128 | 1588.8 | 664.7  | 2.4× |
| 256 | 6466.1 | 2593.4 | 2.5× |

A steady **~2.3–2.6×**, lower than the data-graph ~3.9× because the pinned
merged DP (per-world gate valuation + hom DP) is a larger share of each answer's
cost than the plain hom DP, so amortising the shared stages saves a smaller
fraction of the whole. Both methods agree on every answer.
