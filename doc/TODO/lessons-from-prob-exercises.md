# Lessons from a probability-exercise feasibility study

This file records the actionable items a survey of ~20 standard textbook
probability exercises surfaced in ProvSQL's random-variable surface: one
bug, one gap, one new feature, and one reinforcement of an existing
roadmap item. It is a **to-do** document, not user documentation.

The exercises were drawn from standard university problem sets (Emory Math
Center's discrete-distribution set; the UT-Austin M362K final-practice set
for continuous random variables). Every "works" and "fails" claim below
was executed against **ProvSQL 1.11.0-dev on PostgreSQL 18** in throwaway
databases; psql output is verbatim.

Where a lesson reinforces an item already on the roadmap, this file points
at that item in [`continuous_distributions.md`](continuous_distributions.md)
rather than duplicating it.

## What already works (baseline, for context)

The bulk of "manipulate a concrete distribution, read off a probability /
expectation / moment" exercises are solved, often **exactly** where a
textbook resorts to an approximation:

- Closed-form probabilities: `P(A < B)` for two different-rate
  exponentials is exactly `1/3`; `P(Y − X ≥ 4)` for independent normals is
  `0.27425` (`= 1 − Φ(0.6)`); exponential memorylessness gives `e⁻¹`.
- Discrete laws **derived** from independent Bernoullis, not plugged from a
  formula: six fair coins via `HAVING count(*)` yield `P(#heads = 4) =
  15/64`, `P(≥5) = 7/64`, `P(≤2) = 22/64`; the CLT-flavoured
  `Binomial(100, 0.8)` tail `P(X ≥ 90) = 0.005696` comes out exact.
- Piecewise transforms via `least`/`greatest`/`CASE` (abs, ReLU, clamp,
  nearest-of-k), evaluated by Monte Carlo.
- Sums / products of independent RVs with structural-independence
  detection (`E[XY] = E[X]·E[Y]` for disjoint footprints; sum of i.i.d.
  exponentials folds to Erlang).

## 1. Bug: `product()` aggregate over categorical / mixture RVs

The `product` aggregate is documented alongside `sum` / `avg` (which both
work), but it raises on categorical (and any non-`gate_rv`) children:

```
=> SELECT expected(product(f)) FROM (
     SELECT categorical(ARRAY[0.5,0.5]::float8[], ARRAY[1.3,0.9]::float8[]) AS f
     FROM generate_series(1,5)) t;
ERROR:  provsql.mixture: x must be a scalar RV root (rv / value / arith / mixture), got mulinput
```

The mathematically identical explicit product is exact:

```
=> SELECT expected(  categorical(ARRAY[0.5,0.5]::float8[], ARRAY[1.3,0.9]::float8[])
                   * categorical(ARRAY[0.5,0.5]::float8[], ARRAY[1.3,0.9]::float8[])
                   * categorical(ARRAY[0.5,0.5]::float8[], ARRAY[1.3,0.9]::float8[]) );
 1.3310000000000004   -- = 1.1^3
```

So the failure is in the aggregate ffunc's handling of a `gate_mixture` /
`mulinput`-rooted per-row child, not a modelling boundary. `sum` over the
same categoricals works (verified: `sum` of 100 dice gives exact mean 350,
variance 291.667). This is a plain bug: the `product_rv_ffunc` /
`rv_aggregate_semimod` path should accept the same child shapes `sum` does.

## 2. Gap: conditional probability `P(A | B)` for two RV-comparison events

The conditioning chapter presents `|` as the single, universal
conditioning operator, and `probability(x > y)` already surfaces an RV
comparison as an event. The natural spelling of a conditional probability
of two comparison events is therefore `probability((x>=2000) | (x>=1000))`,
but every form fails:

```
=> SELECT probability( (x >= 2000) | (x >= 1000) ) FROM (SELECT exponential(0.001) AS x) t;
ERROR:  operator does not exist: boolean | boolean

=> SELECT probability_evaluate( (x >= 2000)::uuid | provenance() )
   FROM (SELECT exponential(0.001) AS x) t WHERE x >= 1000;
ERROR:  cannot cast type boolean to uuid
```

**Root cause.** An RV comparison is statically `boolean`-typed even though
the planner hook makes it *evaluate* to a `gate_cmp` UUID. Type resolution
of `a | b` runs at parse time, before the hook rewrites anything, so
`boolean | boolean` fails to resolve and `boolean → uuid` casts are
rejected. A bare `SELECT (x>=2000)` prints a UUID (the value was rewritten)
yet its declared column type is still `boolean`, so it cannot feed `|`.

**Relation to the roadmap.** This is the conditional-probability corner of
§B.6 (*Comparison events as first-class values*) in
[`continuous_distributions.md`](continuous_distributions.md): §B.6 already
proposes surfacing a projected RV comparison as its event token in every
parse position and adding a `probability(<predicate>)` overload. Doing that
work also fixes conditioning: once `x>=2000` and `x>=1000` each rewrite to
their `gate_cmp` `uuid`, `probability(A | B)` type-checks and returns the
`Pr(A∧B)/Pr(B)` ratio (correlation-aware, because both comparisons share
the same `x` leaf). Until then the only spelling is the manual Bayes ratio
`probability(x>=2000) / probability(x>=1000)` (or a conjoined predicate for
the general `Pr(A∧B)`).

**Lesson for §B.6.** Add "conditioning two comparison events with `|`" to
its acceptance criteria, so the fix is validated on `probability(A | B)`,
not only on the standalone `probability(A)` overload.

## 3. New feature to add: covariance / correlation / `stddev` readouts

**Not currently on the roadmap.** ProvSQL exposes the univariate moment
surface (`expected`, `variance`, `moment`, `central_moment`) but no
**bivariate** readout: there is no one-call `covariance(X, Y)`,
`correlation(X, Y)`, or `stddev(X)`. This is the *readout* complement to
the representation-side correlation work (§A.5 Multivariate Normal, §D.2
copulas): you can already *build* correlated RVs (shared `gate_rv` leaves,
shared-coin mixtures, conditioning on a region), you just cannot *read off*
their covariance.

That the readout is already latent was verified end-to-end. Building the
uniform-over-a-triangle distribution by conditioning `X,Y ~ U(0,1)` on
`X + Y ≤ 1`, then assembling the covariance by hand from three conditional
moments:

```
=> SELECT expected(x, provenance())   AS e_x,
          expected(y, provenance())   AS e_y,
          expected(x*y, provenance()) AS e_xy
   FROM (SELECT uniform(0,1) AS x, uniform(0,1) AS y) t
   WHERE x + y <= 1;
   e_x    |   e_y    |   e_xy
 0.333602 | 0.332937 | 0.083318      -- Cov = e_xy - e_x*e_y = -0.0277 ≈ -1/36 (exact)
```

So all the machinery exists; what is missing is the one-call surface.

**UI.** Mirror the existing polymorphic moment dispatchers, including the
optional `prov` conditioning argument:

```sql
SELECT covariance(x, y)               AS cov,      -- E[XY] - E[X]E[Y]
       correlation(x, y)              AS rho,      -- cov / (stddev(x)*stddev(y))
       stddev(x)                      AS sd_x,     -- sqrt(variance(x))
       covariance(x, y, provenance()) AS cond_cov  -- conditioned on a filter
FROM ...;
```

**Why it is a Quick win (scalar, same-row form).** `covariance(x, y)` is
exactly `expected(x*y) − expected(x)*expected(y)`, and `expected` on a
`gate_arith TIMES` already runs the `FootprintCache`
structural-independence path: disjoint footprints return `E[X]·E[Y]`, so
`covariance` returns an **exact** `0` for independent RVs and a
correlation-aware value when leaves are shared, with the Monte Carlo
fallback inherited for free. `stddev` is `sqrt(variance)` on the scalar
`double` result (a numeric readout, so it needs no RV-level `sqrt` and does
not depend on §B.3); `correlation` is a scalar division of these.

**What is Mid-term.** The SQL-standard *aggregate* spellings over a column
of RV pairs or a `GROUP BY` (`covar_pop(x, y)`, `covar_samp`, `corr`,
`stddev_pop` / `stddev_samp`) need the row-aggregate plumbing (the
`make_rv_aggregate_expression` / `rv_aggregate_semimod` path the RV `sum` /
`avg` use), and a two-argument aggregate shape. Ship the scalar two-arg
readout first; add the aggregate form alongside §B.4's `MIN` / `MAX` /
percentile aggregate work, which touches the same plumbing.

**Suggested roadmap placement.** A new Expressivity-completion item, priced
**Quick win** for the scalar readout and **Mid-term** for the aggregate,
adjacent to §B.5 (information-theoretic readouts) which is the nearest
analogue (also a moment-style readout with closed forms plus MC fallback).

## 4. Reinforces §B.3: nonlinear transforms (`pow` / `log` / `exp` / `sqrt`)

**Already on the roadmap** as §B.3 (*Function application beyond +, −, ×,
÷*), item 8, Mid-term, which explicitly lists `log`, `exp`, `sqrt`, `abs`,
`pow`. This session gives it concrete, previously-unrecorded motivation and
one clarification.

**Fresh motivation.** The missing transforms, not any dependence
limitation, are what block generative constructions of dependent joints.
The exercise with joint density `c·xy` on a triangle has the natural
inverse-CDF recipe `Y = 2·U^{1/4}`, `X = Y·√V` (with `U, V` independent
uniforms), which is unrepresentable purely for lack of roots:

```
=> SELECT sqrt(uniform(0,1));      ERROR:  function sqrt(random_variable) does not exist
=> SELECT uniform(0,1) ^ 0.25;     ERROR:  operator does not exist: random_variable ^ numeric
```

**Clarification: `sqrt` is not a feature of its own.** It is exactly
`pow(X, 0.5)`, so it warrants no gate, no `provsql_arith_op` opcode, and no
evaluator arm: everything lives in the generic `pow` (the `gate_arith POW`
arm, its MC evaluation, and its domain guard). At most `sqrt` is an
optional thin SQL alias `sqrt(random_variable) → X ^ 0.5`, mirroring how
PostgreSQL offers `sqrt()` as sugar over `^` — pure convenience, zero new
machinery. The real design point belongs to `pow`, not `sqrt`:

- **Integer exponents are total** and already partly reachable (`X^2` is
  `X*X`); `pow(X, k)` for integer `k` needs no domain guard.
- **Non-integer exponents need a domain guard.** `pow(X, p)` for
  non-integer `p` is real-valued only where `X ≥ 0`; a negative base with a
  fractional exponent is NaN / complex. The existing `RangeCheck`
  support-interval propagation is the right place: when a non-integer
  exponent meets a support interval that crosses `0`, raise (or yield NaN)
  rather than silently returning garbage. A user who wants the
  non-negative branch writes it explicitly as `pow(abs(X), p)`, and `abs`
  is already expressible as `greatest(X, -X)` / a `CASE`.

So the whole `sqrt` question reduces to a `pow` domain check plus, if
desired, one-line SQL sugar. §B.3 should design the fractional-exponent
domain semantics into `pow`; `sqrt` needs no separate treatment.
