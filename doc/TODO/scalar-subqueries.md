# Scalar subqueries: remaining unsupported forms

The bulk of scalar-/correlated-subquery support is implemented in the
planner-hook rewrites in `src/provsql.c` (regression coverage in
`test/sql/scalar_subquery.sql`, `having_grouped_column.sql`). This note tracks
what is **still open**.

## Nested scalar sublinks (passed through with a warning, provenance under-approximated)

A scalar (`EXPR_SUBLINK`) subquery **nested inside a larger expression** --
arithmetic or a function argument, so the sublink is neither a direct
target-list entry nor a direct operand of a WHERE comparison -- is not
decorrelatable by the current rewrites. Rather than reject it, ProvSQL lets it
through with a one-line `provsql_warning`
(`classify_remaining_sublinks` / `collect_direct_qual_sublinks`):

| Form | Example |
|---|---|
| nested in a target-list expression | `SELECT R.a, (SELECT Q.x WHERE Q.k=R.k) + 1 FROM R` |
| nested in a WHERE expression | `… WHERE (SELECT Q.x WHERE Q.k=R.k) + 1 > 50` |

The output row keeps **only the outer relation's provenance**; the subquery's
data is treated as certain. This is an under-approximation, a deliberate
stop-gap. The missing prerequisite has since shipped: native `agg_token`
arithmetic (`+ - * /`, unary `-`) builds `gate_arith` tokens with a tracked
running value (`test/sql/agg_arithmetic.sql`). What remains is wiring it into
the decorrelation path, so the `agg_token` survives the surrounding operators
and the nested sublink can be lifted into a `choose()` like a direct target
entry.

## Correlated sublinks over different `(Q, corr)`

```
SELECT R.a, (SELECT … WHERE Q1.k=R.k) a1, (SELECT … WHERE Q2.k=R.a) a2 FROM R
```

The same-`(Q, corr)` case coalesces onto one LEFT JOIN
(`oj_sub_bodies_coalescible` + the `coalesce` arm of
`decorrelate_scalar_sublinks`); distinct bodies would each need their own LEFT
JOIN, and a chain `R ⟕ Q1 ⟕ Q2` is not lowered by `lower_outer_joins` (it needs
both join arms to be base `RangeTblRef`s). The fix: generalise
`lower_outer_joins` to a left-deep chain `R ⟕ Q1 ⟕ Q2 …`, or wrap-and-recurse
that materialises each decorrelation as a derived `R'` before the next sublink.

## `GROUP BY` body (deferred)

```
(SELECT sum(Q.x) FROM Q WHERE Q.k=R.k GROUP BY Q.k)
```

Body grouping conflicts with the decorrelation's own `GROUP BY R.*`. Currently a
hard error (`ProvSQL: Subqueries … not supported`); needs genuinely new
body-grouping machinery, not a reshaping of the existing rewrites.
