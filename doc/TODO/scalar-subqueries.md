# Scalar subqueries: remaining unsupported forms

The bulk of scalar-/correlated-subquery support is implemented in the
planner-hook rewrites in `src/provsql.c` (regression coverage in
`test/sql/scalar_subquery.sql`, `having_grouped_column.sql`). This note tracks
what is **still open**.

## Nested scalar sublinks

A scalar (`EXPR_SUBLINK`) subquery **nested inside a larger expression** is not
a direct target-list entry nor a direct operand of a WHERE comparison, so the
base decorrelation does not reach it.

**Target-list arithmetic: done.** A sublink nested in target-list arithmetic
(`SELECT R.a, (SELECT Q.x WHERE Q.k=R.k) + 1 FROM R`) is now decorrelated like a
direct entry: `decorrelate_scalar_sublinks` detects the sublink under a chain of
agg_token-tracked arithmetic (`+ - * /`, unary `-`) and casts
(`oj_tl_sublink_in_arith`, peeling exactly what `peel_agg_casts` does), runs the
same `R ⟕ Q` / `choose()` / `count(Q.key)≤1` decorrelation, and replaces the
`SubLink` *in place* with `choose(Q.x)` (`oj_replace_sublink_mut`).  The shipped
native `agg_token` arithmetic then carries Q's provenance through the surrounding
operators as a `gate_arith` token, so the value is tracked rather than the old
outer-only passthrough.  Covered by Part 22 of `test/sql/scalar_subquery.sql`.

**Still open:**

| Form | Example | Status |
|---|---|---|
| nested in a WHERE expression | `… WHERE (SELECT Q.x WHERE Q.k=R.k) + 1 > 50` | passthrough + warning |
| nested in a non-cast function argument | `… f((SELECT Q.x WHERE Q.k=R.k)) …` | passthrough + warning (provenance cannot flow through an opaque function) |
| several nested sublinks coalescing onto one `(Q, corr)` | two `(SELECT …)+c` entries over the same Q | declined (the coalesce path still requires direct entries) |

For the WHERE case the comparison would have to lift to a HAVING `cmp` gate over
`choose(Q.x) + 1` rather than over a bare `choose(Q.x)`; for the coalesce case
each nested entry would lift in place over the shared group.

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
