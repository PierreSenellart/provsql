# §F.1 per-distribution class hierarchy — design + migration plan

Untracked working design note for TODO.md §2 (source: `continuous_distributions.md`
§F.1). Synthesised from an exhaustive map of every `DistKind` switch across the
seven files. **The interface below is the artifact to arbitrate before the full
seven-file migration lands.**

## Current state

`enum class DistKind { Normal, Uniform, Exponential, Erlang }` +
`struct DistributionSpec { DistKind kind; double p1, p2; }` (`src/RandomVariable.h`).
Family behaviour is scattered across ~85 `switch(kind)` branches in 7 files.
Parameter meaning: Normal `p1=μ,p2=σ`; Uniform `p1=a,p2=b`; Exponential `p1=λ`
(p2 unused); Erlang `p1=k` (integer ≥1, stored as double), `p2=λ`.

The exhaustive per-file switch inventory (function, formula, unhandled cases) was
produced during mapping; the salient point is the method set it implies.

## Target `Distribution` interface (per-family, unary)

One `src/distributions/<name>.{cpp,h}` per family implementing:

| Method | Replaces (file:switch) | Unhandled → contract |
| --- | --- | --- |
| `DistKind kind()`, `double p1()`, `double p2()` | i.i.d. equality test (`Expectation.cpp:246`) | — |
| `double mean()` | `analytical_mean` (`RandomVariable.cpp:95`) | total |
| `double variance()` | `analytical_variance` (`:106`) | total |
| `double rawMoment(unsigned k)` | `analytical_raw_moment` (`:165`) | total |
| `double pdf(double x)` | `pdfAt` (`AnalyticEvaluator.cpp:22`) | NaN on bad params / non-integer Erlang |
| `double cdf(double x)` | `cdfAt` (`:71`) | NaN (non-integer Erlang) |
| `double quantile(double p)` | **NEW** (unblocks §4.5 `percentile_cont`) | Normal via Beasley-Springer-Moro (`inv_phi`, already in `MonteCarloSampler.cpp:613`); Exp/Uniform by inversion; Erlang: none (NaN) |
| `Support support()` (`{lo,hi}`) | natural support (`RangeCheck.cpp:127`, `:1448`) | total: N `(-∞,∞)`, U `[a,b]`, Exp/Erl `[0,∞)` |
| `bool integrationRange(double& lo,double& hi)` | `rvIntegrationRange` (`Expectation.cpp:765`), `rvSupportRange` (`AnalyticEvaluator.cpp:200`) | false on bad params; the ±12σ / 40λ⁻¹ / (k+12√k)/λ quadrature window |
| `pair plotRange(double tlo,double thi)` | `bare_x_range` (`RvAnalyticalCurves.cpp:85`) | total (SVG window heuristics) |
| `double sample(mt19937_64& rng)` | sampler (`MonteCarloSampler.cpp:195`) | total (std distributions; Erlang = `gamma_distribution(k,1/λ)`) |
| `string serialise()` | inline in `HybridEvaluator.cpp:201/212/230/942` | total (token + `p1[,p2]`) |
| `optional<double> truncatedRawMoment(lo,hi,k)` | `Expectation.cpp:675` + `truncated_*_raw_moment` (`:536-646`) | **Erlang → nullopt** (needs regularised incomplete gamma); NaN-on-degenerate → nullopt |
| `optional<double> iidOrderStatMean(n,isMax)` | `Expectation.cpp:252` | **Normal, Erlang → nullopt**; Exp only when λ>0 |
| `optional<vector<double>> sampleTruncated(rng,lo,hi,n)` | `MonteCarloSampler.cpp:672` | **Erlang → nullopt**; needs cdf + quantile |
| `optional<unique_ptr<Distribution>> scale(double c)` (and `negate()` = `scale(-1)`, `affine(a,b)`) | `try_times_scalar_rv` (`HybridEvaluator.cpp:885`), `try_neg_rv` (`:654`), uniform affine (`:620`) | **Exp/Erlang: nullopt for c≤0** (support flips); nullopt if not location-scale closed |
| `static optional<unique_ptr<Distribution>> parse(const string&)` | `parse_distribution_spec` (`RandomVariable.cpp:59`) | nullopt on malformed |

`Support` is a tiny `{double lo, hi;}` local to `distributions/` (do **not** couple to
`RangeCheck::Interval`; convert at the call site).

## Pairwise behaviour — OUT of the interface, into registries

Keeps the per-family files from re-creating the row-major coupling.

### `ClosureRuleRegistry` keyed `(DistKind, op, DistKind)` — op is always PLUS
- `(Normal,+,Normal) → Normal(Σaᵢμᵢ+Σbᵢ, √Σaᵢ²σᵢ²)` — `try_normal_closure` (`HybridEvaluator.cpp:449`)
- `(Exp|Erlang,+,Exp|Erlang) same λ → Erlang(Σkᵢ, λ)` — `try_erlang_closure` (`:518`); Exp≡Erlang(1,λ); different-rate → miss
- `(Uniform,+,Uniform) → ∅` explicit non-closure (`:585`)
- Applied as a reduce over a PLUS's terms; each `aᵢ·X+bᵢ` term pre-transformed by the per-family `affine`.

### `ComparatorRuleRegistry` keyed `(DistKind, op, DistKind)` — the §B.2 closed forms
- `(Normal,<,Normal)` → difference-closure then `cdf(0)` (`normalDiffDecide`, `AnalyticEvaluator.cpp:171`)
- `(Exp,<,Exp)` → `λx/(λx+λy)` (`:253`)
- `(Uniform,<,Uniform)` → `integralUniformCdf/(d−c)` (`:258`)
- miss → generic `mixedPairLess` composite-Simpson quadrature over `pdf`/`cdf`/`support` (base default, no per-family code)

## What stays OUT
- **Footprint / structural independence** (`FootprintCache`) switches on gate type, never
  on family — a `gate_rv` is an opaque atom. Stays gate-level.
- The op-switches (`ComparisonOperator`, arith opcode canonicalisation) are family-agnostic.

## Migration order (behaviour-preserving; the `test/` suite is the net)
1. `src/distributions/`: `Distribution.h` (interface + `Support`) and one file per family,
   bodies **copied verbatim** from the mapped switches (behaviour preserved by construction),
   + `makeDistribution(const DistributionSpec&)` factory and `parseDistribution(string)`.
2. Migrate the pure free functions first (lowest risk, exercised by the whole suite):
   `RandomVariable.cpp` `analytical_{mean,variance,raw_moment}` → thin wrappers over the
   factory. Rebuild + `make test`.
3. Migrate `pdfAt`/`cdfAt` (`AnalyticEvaluator`) → `dist->pdf/cdf`; then the support /
   integration-range consumers (`RangeCheck`, `Expectation::rvIntegrationRange`); then the
   sampler; then `RvAnalyticalCurves` plotRange; then serialise (`HybridEvaluator`).
   Rebuild + test after each consumer.
4. Lift the pairwise logic (`try_normal_closure`, `try_erlang_closure`, `rvVsRvDecide`) into
   the two registries. This is the only step that restructures rather than relocates —
   arbitrate the registry shape here.
5. Land Gamma (§3.1) as the first proof-of-concept family: one new file + registry entries,
   no existing file touched. Chi-squared = `Gamma(k/2, 1/2)` sugar.

## Design decisions (arbitrated)
- **Distribution lifetime: transient `unique_ptr`.** `makeDistribution(spec)` builds on
  demand; no gate-ABI or on-disk change. Hot paths (`pdfAt`/`cdfAt`/`sample`) MUST construct
  the `Distribution` once *outside* the quadrature / MC loop and reuse it — never per
  point/draw (that is the only correctness-neutral perf trap in the migration).
- **`quantile` is OPTIONAL.** `std::optional<double> quantile(double p) const`, returning
  `std::nullopt` when the family has no elementary inverse CDF (Erlang) rather than NaN;
  callers branch on availability explicitly. Backs §4.5 `percentile_cont` and the
  Normal/Exp truncated samplers. (Chosen over a hard NaN-returning method so "unsupported"
  cannot be silently consumed.)
- **Pairwise rules: static self-registration.** Each family file registers its
  `(kind, op, kind)` closure / comparator rules at static-init into a **function-local-static**
  registry (so initialisation order is well-defined); adding a family touches no existing
  file. The registries are looked up by the closure / comparator drivers.
- **Transform primitive: `affine(a, b)` = a·X + b.** `scale(c)` / `negate()` are thin
  wrappers; the Normal / Uniform closure reducers call `affine` per term rather than inlining
  the per-term mean / variance math (`try_normal_closure:488`, uniform affine `:620`).

## Status
Committed (all 235 tests green after each; interface lives in `src/Distribution.{h,cpp}`):
- `f84f6df9` — 4 family classes (moments / pdf / cdf / support / sample) + `makeDistribution`;
  `RandomVariable.cpp` moment free-functions.
- `f5b0c07a` — `RangeCheck` support → `Distribution::support()`.
- `5f942d67` — `integrationRange` + `plotRange` on the interface; migrated
  `Expectation::rvIntegrationRange`, `AnalyticEvaluator::rvSupportRange`,
  `RvAnalyticalCurves::bare_x_range`.
- `6c1f624c` — `pdfAt`/`cdfAt` bodies delegate; the three Simpson loops (`mixedPairLess`,
  `mixedOrderStatMean`, `rvVsRvConditionalMoment`) construct the `Distribution` **once before
  the loop**; dead `rvSupportRange`/`rvIntegrationRange` removed. Cold callers (`cdfDecide`,
  `shape_mass`, `bare_pdf`/`bare_cdf`) keep the delegating free functions.
- `860b560f` — plain MC sampler: a per-`gate_rv` `Distribution` cache on `Sampler` (persists
  across iterations, not cleared in `resetIteration`); RNG bit-identical.

**All analytic consumers + plain sampling are now on virtual dispatch.** Remaining
`DistKind` switches, by area:
1. **Comparators** — `AnalyticEvaluator::rvVsRvDecide` / `normalDiffDecide` →
   `ComparatorRuleRegistry` (Normal-Normal difference, Exp-Exp ratio, Uniform-Uniform;
   a miss falls to the already-migrated `mixedPairLess` quadrature).
2. **Closures** — `HybridEvaluator::try_normal_closure` / `try_erlang_closure` /
   `try_neg_rv` / `try_times_scalar_rv` + the inline serialise → `ClosureRuleRegistry` +
   `affine(a,b)` + `serialise()` (relocate `double_to_text`).
3. **Truncated** — `MonteCarloSampler` truncated sampler + `Expectation::try_truncated_closed_form`
   → `truncatedRawMoment` + `sampleTruncated` + optional `quantile` (relocate `inv_phi`).
4. **Order statistics** — `Expectation::iidOrderStatMean` → a per-family method (Uniform/Exp
   closed forms; Normal/Erlang nullopt).
5. **Parse** — `RandomVariable::parse_distribution_spec` → optional `Distribution::parse` factory.

Then split `src/Distribution.{h,cpp}` → `src/distributions/` one file per family (add
`src/distributions/*.cpp` to the `OBJS` glob in `Makefile.internal`), and Gamma (§3.1) — whose
general-shape CDF needs a regularised-lower-incomplete-gamma implementation, unlike the
integer-shape Erlang finite sum.
