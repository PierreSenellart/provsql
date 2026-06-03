# Scalar subqueries: remaining unsupported forms

## Why this note exists

The bulk of the scalar-/correlated-subquery work is **done** and lives in the
planner-hook rewrites in `src/provsql.c` (regression coverage in
`test/sql/scalar_subquery.sql`, `having_grouped_column.sql`). What now ships:

- the **outer-join provenance root fix** (`LEFT`/`RIGHT`/`FULL` lowered to the
  matched arm `⊎` the null-padded antijoin arm via ProvSQL's `−` / `NOT IN`
  semantics) -- `lower_outer_joins`;
- **correlated value** subqueries `(SELECT v FROM Q WHERE corr)` -> `R ⟕ Q`,
  `GROUP BY R.*`, `choose(v)`, `HAVING count(Q.key) <= 1` (target-list position,
  and WHERE comparisons lifted to `HAVING`, where a comparison adds
  `count(Q.key) = 1` so the empty group is excluded) -- `decorrelate_scalar_sublinks`;
- **aggregate-body** subqueries `(SELECT agg(v) FROM Q WHERE corr)` -> the
  aggregate over the join group (no count gate; `count(*)` -> `count(Q.key)`);
- **`SELECT DISTINCT` value body** `(SELECT DISTINCT Q.x FROM Q WHERE corr)` ->
  the same `R ⟕ Q`, `GROUP BY R.*`, `choose(v)` decorrelation, but the
  at-most-one-row gate counts distinct **values** instead of rows:
  `HAVING count(DISTINCT v) <= 1` (and `= 1` for a WHERE comparison). This
  admits "many matching rows, all the same value" (which `DISTINCT` collapses to
  one) and still gates the worlds with more than one distinct value -- relies on
  the COUNT(DISTINCT)-over-an-outer-join fix in `rewrite_agg_distinct`;
- **`ORDER BY … LIMIT 1` value body** (argmax / "latest per group") ->
  `choose(v ORDER BY key)` over the join group, no count gate (LIMIT 1 legally
  takes the top of many rows) -- the body's junk sort-key targetList entries and
  `sortClause` become the ordered `Aggref`'s order arguments;
- **`EXISTS` / `IN` / `op ANY`** (semijoin) and **`NOT EXISTS` / `NOT IN` /
  `op ALL`** (antijoin -- `∀q. x op q = ¬∃q. x ¬op q`), plus **row `IN`**
  (`(a,b) IN (SELECT …)`, per-column conjuncts), rewritten to a correlated
  `(SELECT count(*) …) >= 1` / `= 0` -- `rewrite_predicate_sublinks`,
  `extract_quantified_corr`;
- **`ARRAY(SELECT v FROM Q WHERE corr)`** -> `array_agg(v)` over the group, with a
  `FILTER` dropping the null-padded row -- `rewrite_array_sublinks`;
- **multi-table bodies** `(SELECT v FROM Q1, Q2 WHERE …)`, collapsed into a
  derived cross-product subquery -- `oj_wrap_body_from`;
- **uncorrelated** target-list subqueries, moved into a cross-joined derived
  aggregate in the outer FROM (value bodies as `choose(v)` + `HAVING count(*) <=
  1`) -- `move_uncorrelated_sublinks_to_from`;
- **uncorrelated `EXISTS`** and **uncorrelated aggregate comparisons in WHERE**
  (`(SELECT count/max/… FROM Q) OP const`), pushed into a HAVING-gated one-row
  subquery cross-joined into the FROM -- `move_uncorrelated_where_predicates`;
- **uncorrelated `NOT EXISTS`** and **true-on-empty `count(*)` / `count(col)`
  comparisons** (`(SELECT count(*) FROM Q) < k` / `<= k` / `= 0`), the m-semiring antijoin
  `R ⊗ (1 ⊖ ⟦P⟧)` for the false-on-empty `P`; rewritten to the EXCEPT-ALL
  difference `R EXCEPT ALL π_R(R × D)` with `D` the one-row HAVING-gated `P`
  (multiplicity preserved) -- `rewrite_uncorrelated_antijoin`;
- a **HAVING comparison of an aggregate against a grouped column** (per-group
  variable RHS), wrapped like a constant in `having_OpExpr_to_provenance_cmp`;
- a multi-table / auto-wrapped outer FROM, and an untracked outer relation;
- a **sublink whose body touches no tracked relation** (over an untracked table,
  or a constant/derived source): a deterministic condition/value (untracked data
  is certain), passed straight through to Postgres with the row keeping its
  provenance -- the "Subqueries not supported" error now fires only when a
  sublink's body involves a tracked relation (`query_has_tracked_sublink`).

This note tracks only what is **still rejected** -- each with the clean
`ProvSQL: Subqueries (EXISTS, IN, scalar subquery) not supported` error, never a
wrong answer.

## Remaining unsupported forms

Tables below: `R(a, k)`, `Q(k, x)`, both provenance-tracked.

| Form | Example | Why rejected | Extensible? |
|---|---|---|---|
| bare `LIMIT` / `OFFSET` body (no `ORDER BY`) | `(SELECT Q.x FROM Q WHERE Q.k=R.k LIMIT 1)` | Picks an arbitrary, non-deterministic row | no -- ill-defined without an order (`ORDER BY … LIMIT 1` IS tractable, see Priorities) |
| `GROUP BY` body | `(SELECT sum(Q.x) FROM Q WHERE Q.k=R.k GROUP BY Q.k)` | Body grouping conflicts with the decorrelation's own `GROUP BY R.*` | hard |
| `ORDER BY` inside `ARRAY(...)` | `ARRAY(SELECT Q.x FROM Q WHERE Q.k=R.k ORDER BY Q.x)` | Element order would not survive the regroup into `array_agg` | hard -- needs an ordered aggregate |
| Two or more correlated sublinks | `SELECT R.a, (SELECT … WHERE Q.k=R.k) a1, (SELECT … WHERE Q.k=R.k) a2 FROM R` | `decorrelate_scalar_sublinks` handles exactly one sublink (the uncorrelated FROM-move already handles several) | yes -- iterate the correlated decorrelation per sublink |
| Sublink nested in a target-list expression | `SELECT R.a, (SELECT Q.x WHERE Q.k=R.k) + 1 FROM R` | Sublink is not the direct target entry; for an aggregate result the arithmetic would coerce the `agg_token` to a scalar and drop its provenance | partly -- safe only where the `agg_token` survives |
| Sublink nested in a WHERE expression | `… WHERE (SELECT Q.x WHERE Q.k=R.k) + 1 > 50` | Comparison is not directly on the sublink (arithmetic in between) | partly |
| Uncorrelated **value**-body comparison in WHERE | `… WHERE (SELECT Q.x FROM Q) > 5` | The aggregate form (`count`/`max`/…) is handled; a value body would need `choose(x)` + `count(*) <= 1` baked into the gated subquery's HAVING | yes -- extend `move_uncorrelated_where_predicates` to value bodies |

## Priorities

The genuinely-tractable next steps, roughly in value order:

1. **Multiple correlated sublinks** -- decorrelate each onto its own LEFT JOIN
   (coalescing sublinks that share a `(Q, corr)`), instead of bailing on >1.
2. **Uncorrelated value-body comparison in WHERE** -- extend
   `move_uncorrelated_where_predicates` to `choose(x)` + `count(*) <= 1` plus a
   WHERE-comparison cmp gate on the cross-joined `agg_token`.

`GROUP BY` / `ORDER BY` bodies and bare (un-ordered) `LIMIT` are deferred: each
needs genuinely new machinery (body-grouping or ordered aggregation), not a
reshaping of the existing rewrites.
