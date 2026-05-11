# Mixture-distribution playbook for ProvSQL Studio

This is the manual-test checklist for the `provsql.mixture` feature. It
walks through every code path the new gate type touches, anchored on the
`mixture_demo` fixture under `studio/scripts/load_demo_mixture.sql`.

## Setting up

```bash
createdb mixture_demo
psql -d mixture_demo -f studio/scripts/load_demo_mixture.sql
provsql-studio --database mixture_demo
```

The script seeds **8 mixture rows** in `mixture_demo`, each exercising a
different facet of the feature, plus a `mixture_demo_bernoulli` side
table holding four reusable `gate_input` tokens (`p_50`, `p_50b`,
`p_95`, `p_30`).

## What to try

### A. Reveal the bimodal shape that started this whole thread

In the Studio querybox (Circuit mode):

```sql
SELECT id, label, expr FROM mixture_demo WHERE id = 1;
```

Click the `expr` cell. The circuit renders with **`Mix`** in the root
circle — the new gate glyph.

Eval strip → **Distribution profile** → set **bins = 60** → Run. Two
clean peaks at ±3, empty middle band. The bimodal histogram that the
convolution version (`N(-3,0.5) + N(3,0.5)`) could not produce.

### B. Compare against the unimodal convolution

To convince yourself the constructor really matters, also try:

```sql
SELECT 'sum (convolution)' AS label,
       provsql.normal(-3::float8, 0.5::float8)
     + provsql.normal( 3::float8, 0.5::float8) AS expr;
```

Same eval strip → **Distribution profile** → single peak at 0. The
mixture's bimodality is structural, not parameter tuning.

### C. Heavy-tailed contamination (row 2)

`0.95·N(0,1) + 0.05·N(0,10)` is mass-equivalent to `N(0,1)` but with a
tail bloat. Distribution profile with default bins shows a narrow
centre and visible outliers in the bins past ±3. Variance:

```sql
SELECT provsql.expected(expr), provsql.variance(expr)
  FROM mixture_demo WHERE id = 2;
-- Expected: E[M] = 0, Var(M) = 0.95·1 + 0.05·100 = 5.95
-- Orders of magnitude larger than N(0,1)'s variance of 1.
```

### D. Mixed-family mixture (row 3)

`0.3·U(0,2) + 0.7·Exp(1)`. The closed-form mean still works
(`provsql.expected` = `0.3·1 + 0.7·1 = 1.0`) but the analytic-CDF fast
path doesn't (different distribution families). The histogram is a
flat block on `[0,2]` plus an exponential tail.

### E. 3-component cascade (row 4)

Demonstrates **compositionality**: a 3-way GMM built by nesting two
binary mixtures. Effective weights `(0.5, 0.25, 0.25)` over peaks at
0/5/10. In the circuit, the root `Mix` has a Y branch that is itself
a `Mix`.

### F. Simplifier lift (row 5)

`3 + mixture(p, N(0,1), N(4,1))`. In the circuit, click the root cell
— the simplified subgraph should show a `Mix` at the top with two
**`N(3,1)`** and **`N(7,1)`** leaves rather than an arith tree. (The
lift fired; the inner normal-family closure collapsed each branch.)
Verify the analytical numbers:

```sql
SELECT provsql.expected(expr), provsql.variance(expr)
  FROM mixture_demo WHERE id = 5;
-- Expected: 5.0 (= 3 + 0.5·0 + 0.5·4)
-- Variance: 5.0 (mixture variance of N(3,1) / N(7,1))
```

### G. Coupling — the killer feature (rows 6 vs 7)

Side-by-side comparison of shared vs distinct Bernoullis:

```sql
SELECT id, label,
       provsql.expected(expr) AS mean,
       provsql.variance(expr) AS variance
  FROM mixture_demo
 WHERE id IN (6, 7);
```

Both rows have mean = 0. But:

- **Row 6** (shared `p_50`): variance ≈ **100** — the joint always lands
  on ±10 (perfectly correlated branches).
- **Row 7** (distinct Bernoullis): variance ≈ **50** — joint spans
  {-10, 0, +10} with masses 0.25 / 0.5 / 0.25.

Distribution profile on both makes the difference visible: row 6 is a
clean bimodal at ±10, row 7 has three peaks at -10 / 0 / +10.

### H. Ad-hoc probability overload (row 8)

When you don't care about reusing the Bernoulli, the `mixture(p_value,
x, y)` overload mints an anonymous `gate_input` on the fly with the
given probability. Useful for one-off mixtures in interactive sessions:

```sql
-- Three ways to write the same shape (asymmetric GMM, 70/30 mix):
SELECT 'ad-hoc'     AS form, provsql.mixture(0.7::float8,
                                             provsql.normal(0, 1),
                                             provsql.normal(8, 1)) AS m
UNION ALL
SELECT 'explicit p' AS form, provsql.mixture(
                                (SELECT token FROM mixture_demo_bernoulli WHERE name='p_30'),
                                provsql.normal(8, 1),   -- swap branches because p_30
                                provsql.normal(0, 1))   -- has prob 0.3 not 0.7
UNION ALL
SELECT 'row 8'      AS form, expr                              FROM mixture_demo WHERE id = 8;
```

Click each row's `m` cell; all three should report `provsql.expected ≈
2.4` and `provsql.variance ≈ 14.44` (= 0.7·1 + 0.3·65 - 5.76 from the
mixture-variance formula).

The crucial property: two calls to `mixture(0.5, X, Y)` are
**independent** — the convenience form is not designed for coupling.
If you need two mixtures to share a coin, pre-mint a `gate_input`
yourself and use the `uuid` form.

### I. Validation errors

The eval strip works on errors too; what fails is the constructor
itself. Try these in querybox (you'll see the ProvSQL error banner):

```sql
-- p must be a gate_input (gate_value is rejected):
SELECT provsql.mixture(random_variable_uuid(provsql.as_random(0.5)),
                       provsql.normal(0, 1), provsql.normal(0, 1));

-- p must have prob in [0,1] (the uuid form -- check on user-managed token):
DO $$
DECLARE p uuid := public.uuid_generate_v4();
BEGIN
  PERFORM provsql.create_gate(p, 'input');
  PERFORM provsql.set_prob(p, 1.7);
  PERFORM provsql.mixture(p, provsql.normal(0,1), provsql.normal(0,1));
END $$;

-- p must be in [0,1] (the scalar overload -- direct guard):
SELECT provsql.mixture( 1.5::float8, provsql.normal(0, 1), provsql.normal(0, 1));
SELECT provsql.mixture(-0.1::float8, provsql.normal(0, 1), provsql.normal(0, 1));
SELECT provsql.mixture('NaN'::float8, provsql.normal(0, 1), provsql.normal(0, 1));
```

### J. Aggregate root rejection

`SUM` over a mixture column produces an `agg_token`, which the
constructor cannot accept as a child (out of scope for v1):

```sql
-- Should raise: provsql.mixture: x must be a scalar RV root
--               (rv / value / arith / mixture), got agg
WITH s AS (SELECT SUM(expr) AS s FROM mixture_demo)
SELECT provsql.mixture(0.5::float8,
                       s.s::random_variable,
                       provsql.normal(0, 1))
  FROM s;
```

### K. Studio's evaluator menu

With a mixture root pinned, the eval-strip dropdown should show
**Distribution profile** and **PROV-XML** only (not the Boolean
families) — same filter that gate_rv / gate_arith roots get. Verifies
the `_SCALAR_GATE_TYPES` extension landed correctly.

## What's *not* in this PR (deferred)

- **AnalyticEvaluator closed-form CDF** for mixtures of bare RVs.
  Today `cmp(mixture, c)` falls through to MC; a closed-form
  `π·F_X(c) + (1−π)·F_Y(c)` is feasible when X / Y are bare gate_rv
  leaves but was deferred.
- **MINUS / DIV / NEG lifts** in the simplifier (only PLUS / TIMES
  today).
- **Mixture inside aggregations**: `SUM(mixture_column)` over a tracked
  relation — `agg_raw_moment` would need to learn `random_variable`
  values, not just float8 scalars.
- **Studio inspector showing π** inline — today the user clicks the
  `gate_input` child to see its probability via the inline editor; a
  dedicated π row in the mixture inspector is a small follow-up.
