# Window functions

Design note for provenance support of SQL window functions
(`f(…) OVER (PARTITION BY … ORDER BY … [frame])`).

## Status today

Tiers 1 and 2 are implemented: an aggregate used as a window function over a
frame determined by values, and `rank()`, `dense_rank()`, `row_number()`, become
`agg_token`s over the rows of their frames (`replace_window_aggregations`, test
`window_aggregates`, user documentation in `user/aggregation.rst`).

Any other window function runs as before, with a warning: each output row
carries the provenance of its input row, and the windowed value is an opaque
scalar. That is right for the row, and says nothing of the value: `lag(x)` is a
plain value, although which row it reads depends on which rows are present.

## Why it matters

Measured on three public corpora of queries written by people (Stack Exchange
Data Explorer, 11,226 distinct queries; SQLShare, 8,670; the statements of the
PostgreSQL-tagged posts of DBA Stack Exchange, 16,251), window functions occur
in 11.9 %, 2.1 % and 7.1 % of the queries. No other construct that ProvSQL does
not track comes close. Among the queries that use one:

| | SEDE | SQLShare | DBA.SE |
|---|---|---|---|
| ranking (`row_number`, `rank`, `dense_rank`) | 93.0 % | 94.0 % | 44.3 % |
| offset (`lag`, `lead`, `first_value`, `last_value`) | 1.1 % | – | 29.4 % |
| running aggregate (ORDER BY, prefix frame) | 2.2 % | – | 20.0 % |
| whole-partition aggregate (no ORDER BY) | 2.8 % | 13.2 % | 18.5 % |
| sliding `ROWS` aggregate | 0.1 % | – | 2.9 % |
| `ntile`, `percent_rank`, `cume_dist` | 1.8 % | – | 1.9 % |
| value compared with a constant afterwards (`rn = 1`, `rk <= 10`) | 12.3 % | 19.8 % | 23.7 % |
| value only output by the statement | 78.3 % | 41.2 % | 31.8 % |
| value aggregated afterwards | 1.4 % | – | 11.1 % |
| value used as a grouping key afterwards | 1.9 % | – | 8.0 % |

Two uses dominate: numbering or ranking the rows of a result, and the
"top-k per group" idiom, a rank computed in a subquery and compared with a
constant outside.

## Plain semantics

We follow the operator of Lindner, Naumann and Lerner, *Window Function
Optimization: Co-Evaluation and Other Techniques*, PVLDB 19(11), 2026
([PDF](https://www.vldb.org/pvldb/vol19/p3525-lindner.pdf)), which to our
knowledge is the first formal definition of window functions. The operator
⊞<sub>P,O,w,f</sub>(r) has four parameters, the partition attributes P, the
order attributes O (with direction and NULL ordering), a frame function w and an
aggregate or ranking function f, and is defined over sequences, in six phases:

1. partition r by P (syntactic equality: NULLs are equal);
2. sort each partition by O into a sequence S<sub>j</sub>;
3. for each position i, the frame function gives a sequence of indexes into
   S<sub>j</sub>, hence the frame contents W<sub>i,j</sub>;
4. f maps that sequence to a value;
5. emit the tuple extended with the value;
6. collect the partitions back into a bag.

When O does not determine the order, several sequences are valid, and the result
of position-dependent functions (`row_number`, `lag`, `ROWS` frames) is not
determined. Their Equivalence (4) is the base case below: for a standard
aggregate f and a window with no partitioning, ⊞<sub>∅,…,f</sub>(r) ≡ r ×
γ<sub>f</sub>(r), a window aggregate being the join of each row with the grouped
query.

## Annotated semantics

The row keeps its annotation: a window function neither removes nor merges
rows. The value becomes an aggregate token, as for `GROUP BY`: the function f
together with the annotated sequence of the occurrences it may read, its value
in a world being f applied to the occurrences present in that world.

One change to the plain operator is needed. Its frame function returns
*positions* in the sorted partition of the instance, and positions are not
stable under uncertainty: "the previous row" is the previous row *that is
present*. The frame of the annotated semantics is therefore defined from the
values of the ordering attributes, which are certain, and f reads the
subsequence of present occurrences:

- whole partition, the default frame (`RANGE UNBOUNDED PRECEDING` to the current
  row and its peers), explicit `RANGE` and `GROUPS` frames: the frame is a set of
  rows determined by values, f is the ordinary aggregate. Positional and
  value-based definitions coincide;
- `rank()`: 1 + `count` over the rows strictly before the current one;
  `dense_rank()`: 1 + the number of distinct ordering values strictly before;
- `lag(x)`, `lead(x)`, `first_value(x)`, `last_value(x)`: "first present element"
  of a value-determined sequence (the strict predecessors in reverse order, for
  `lag`), which is the order-dependent `choose` aggregate;
- `ROWS BETWEEN k PRECEDING AND CURRENT ROW`: f is "aggregate of the last k + 1
  elements" of the prefix; `lag(x, k)`, `nth_value`: "k-th element from the end".
  Expressible, but these f are not functions the evaluators know;
- `ntile`, `percent_rank`, `cume_dist`: arithmetic over two counts (a position
  and the size of the partition).

Two conventions need care.

- **The current row is present.** A frame that contains the current row is only
  ever read in worlds where that row is present. In the Boolean semiring, and in
  absorptive semirings, the worlds without it vanish when the value's provenance
  is multiplied by the row's annotation, so nothing special is needed; in
  general the possible-world sum has to be restricted to the worlds containing
  the row ("anchored" token). For `count` and `sum` this can be obtained without
  a new notion, as the current row's contribution plus a token over the *other*
  rows of the frame.
- **The empty frame has a value.** A frame that excludes the current row (strict
  predecessors, for `rank` and `lag`) may be empty while the row exists: `count`
  is then 0, the others NULL. This is the convention of scalar aggregation (the
  scalar flag of `provenance_aggregate`), not that of grouped aggregation, where
  an empty group is no row and `provenance_aggregate` returns `gate_zero()`.

## Plan

### Tier 1: aggregates over a whole partition (done)

`f(x) OVER (PARTITION BY g)`, with f one of the supported aggregates, `FILTER`
included. Exact, and no more expensive than `GROUP BY`.

Lowering, mirroring `make_aggregation_expression`: the `WindowFunc` is replaced
by

```
provenance_aggregate(fn, type,
                     f(x) OVER w,                                  -- displayed value
                     array_agg(provenance_semimod(x, k)) OVER w,   -- same window
                     is_scalar)
```

where k is the row's provenance and the second window call shares the `winref`
of the first. `provenance_aggregate` is an ordinary function of the row, which
PostgreSQL evaluates above the `WindowAgg` node. The special cases of
`make_aggregation_expression` (`count(*)`, `count(e)`, `FILTER`, NULL-keeping
aggregates, the ORDER BY of the aggregate) carry over. Gates are
content-addressed, so the rows of a partition share one aggregate gate.

By δ-absorption this is the join of each row with the grouped query, the
equivalence quoted above.

As implemented, `is_scalar` is false when every frame contains its current row,
so that a whole-partition window gets the very gate of the `GROUP BY` (checked
in the test), and true when the frame may exclude it. For the latter,
`provenance_aggregate` over no token now builds an `agg` gate without children
rather than 𝟘, which also fixed `HAVING count(*) < 1` over no row (it failed with
"This semiring does not support value gates"). Windows over the aggregates of a
`GROUP BY` (`sum(sum(x)) OVER ()`) summed the `agg_token` datums as integers;
`cast_agg_token_mutator` now casts the arguments of a `WindowFunc`, and they
stay untracked.

### Tier 2: ranks and running aggregates (done)

- `sum`, `count`, `min`, `max`, `avg` with `ORDER BY` and the default frame, or an
  explicit value-based frame: the lowering of tier 1 applies unchanged, since
  PostgreSQL's frame is then determined by values. Done, with
  `window_frame_by_values` deciding: no `ORDER BY`, `RANGE`, `GROUPS` without
  offsets, `ROWS` over the whole partition, any `EXCLUDE`.
- `rank()`: `1 + count(*) OVER (PARTITION BY … ORDER BY … RANGE BETWEEN UNBOUNDED
  PRECEDING AND CURRENT ROW EXCLUDE GROUP)`, lowered as above with the scalar
  convention, the `+ 1` being arithmetic on an `agg_token` (an `arith` gate).
  Done (`make_rank_window`, `make_rank_expression`); PostgreSQL 11 and later,
  for `EXCLUDE`. Counting the current row instead (`EXCLUDE TIES`, no `+ 1`)
  was tried and dropped: the count then reads the row's own token, which also
  multiplies the comparison, and the correlation sent the comparison to
  knowledge compilation (minutes for 50 rows).
- `row_number()`: equal to `rank()` when the ordering is total on the partition,
  which can be checked on the instance (a sub-instance of a totally ordered
  instance is totally ordered). With ties its value is unspecified by SQL
  itself: it is then read as `rank()`, with a warning. Done: the rank is
  wrapped in `row_number_as_rank`, which warns once per statement when
  PostgreSQL's row number differs from it.
- `dense_rank()`: needs the number of distinct ordering values before the
  current row. PostgreSQL has no `DISTINCT` in window aggregates, so this goes
  through a subquery that merges peers first (one token per ordering value, the
  ⊕ of its rows), as `AGG(DISTINCT)` does. Done without the subquery: two
  `array_agg` over the frame of the rows strictly before collect the ordering
  values (a row value, for several keys) and the tokens, and
  `window_distinct_tokens` groups them into one `semimod(1, ⊕ tokens)` per
  value, under a scalar `COUNT` gate (`make_dense_rank_expression`).

**The top-k idiom.** A rank compared with a constant in an enclosing query,

```sql
SELECT … FROM (SELECT …, rank() OVER (PARTITION BY g ORDER BY s DESC) AS rk FROM t) u
WHERE rk <= 3
```

is a selection on an aggregate column of a subquery, which
`migrate_probabilistic_quals` already routes to a comparison gate. The `+ 1` is
not folded by `normalize_agg_comparison`, which only sees the subquery's
column; the comparison evaluators fold it (`matchAggCmp` peels constant
arithmetic off the aggregate), and a top-3 over 300 rows takes half a second.
The row's
annotation becomes α ⊗ ⟦count of present rows before it < 3⟧, a `COUNT`
comparison: the existing evaluators (the Poisson-binomial pre-pass for
probabilities) apply. This is also the exact annotation of a `LIMIT k`, and
`ORDER BY … LIMIT k` is now lowered to it, at every level of a statement
(`lower_limit_to_rank`, test `limit_rank`, user documentation in
`user/querying.rst`): the query becomes the subquery of a filter on
`row_number()` (or `rank()` for `FETCH … WITH TIES`), and returns every row
that may be kept, annotated with that condition. `LIMIT actual(k)` keeps the
truncation of the actual result, each kept row carrying its provenance in the
full result. Left as truncations: a `LIMIT` without `ORDER BY`, over an
aggregation, `DISTINCT` or set operation, `OFFSET` with `WITH TIES`, and every
`LIMIT` before PostgreSQL 11. `OFFSET m LIMIT k` compares the same rank twice,
which the closed-form evaluators decline (a shared aggregate): it goes through
the general enumeration, limited in size; a range comparison in the COUNT
evaluator would lift that.

### Tier 3: offset functions

`lag(x)`, `lead(x)`, `first_value(x)`, `last_value(x)` as ordered `choose`
aggregates over a value-determined frame that excludes the current row.
Comparisons on `choose` are already evaluated; ties in the ordering make the
value unspecified, as for `row_number`.

### Not planned

Sliding `ROWS` frames, `lag` / `lead` with an offset, `nth_value`, `ntile`,
`percent_rank`, `cume_dist`; window functions over the result of a `GROUP BY` of
the same query (`avg(sum(x)) OVER (…)`), which aggregate over aggregate values.
They keep the present behavior, with the warning.

## What stays out of reach

A window value is an `agg_token`, with the restrictions that come with it: it
can be output, used in arithmetic, and compared; it cannot be a grouping key, a
join key, or the input of another aggregate. The "gaps and islands" idioms
(flag a change with `lag`, number the runs with a running sum of the flags,
group by that number) are of that last kind: about a quarter of the uses of
`lag` and `lead` in the DBA.SE corpus aggregate the value afterwards.

One restriction had to be lifted: `ORDER BY` on an `agg_token` is refused, and
`ORDER BY rn` is the most common thing done with a row number. Sorting on the
displayed value is presentation, like `ORDER BY` itself: a window value is now
sorted that way (`replace_window_aggregations` moves the sort key to a junk copy
of the original window call). The aggregates of a `GROUP BY` are still refused,
and could be treated the same way.

## Cost

- One aggregate gate per row for an ordered frame, with as many children as the
  frame has rows: the circuit of a running aggregate is quadratic in the size of
  the partition, and so is the work of `array_agg` over a growing frame. Whole
  partitions share one gate and are linear.
- For the monoid aggregates (`sum`, `count`, `min`, `max`), a prefix token could
  be built from the previous one plus one term, giving a linear circuit; this
  needs the evaluators to accept an aggregate gate among the children of an
  aggregate gate. For `lag`, a chain of `case` gates would do the same.
- The per-row cost of `provenance_semimod` (a PL/pgSQL function with several
  round trips to the circuit store), which already dominates aggregate queries,
  applies here as well.

A first version can take the quadratic form, with the documentation saying so.

## Where in the code

- `process_query`: the warning on `q->hasWindowFuncs` becomes a classification
  of the `WindowFunc` nodes of the target list (tiers above, else the warning).
- `replace_aggregations_by_provenance_aggregate` and its mutator only look at
  `Aggref` nodes; a sibling for `WindowFunc` builds the call above, reusing
  `make_aggregation_expression`'s handling of the argument. The window clause
  (`q->windowClause`) is unchanged for tier 1 and the running aggregates; `rank`
  adds a window definition with `EXCLUDE GROUP`.
- `insert_agg_token_casts` already knows `WindowFunc` as a consumer of
  `agg_token` values (it casts them back); the new values are producers.
- Several rewrites decline queries with `hasWindowFuncs` (sublink
  decorrelation, outer-join lowering, the reachability recognizer); that stays.
- Evaluators: the anchored and empty-frame conventions above; nothing else for
  tiers 1 and 2.

## Testing

- Displayed values against the same query over an untracked copy, as in
  `agg_distinct_expr`.
- Probabilities against brute-force enumeration of the worlds of a small
  partition: `rank() <= k`, running `sum` compared with a constant, the first row
  of a partition (empty frame), ties.
- Gate sharing for whole partitions; circuit size for running aggregates.
- The refusal or warning paths: `row_number` with ties, `ROWS` frames.

## Open questions

1. Anchoring in non-absorptive semirings: decompose into "current row plus the
   others" where possible (`count`, `sum`), and otherwise document the value as
   meant for Boolean and absorptive evaluation, or add an explicit flag on the
   aggregate gate?
2. Linear-size circuits for running aggregates: worth the change in the
   evaluators from the start, or only once the quadratic form has shown its
   limits? For a top-k (`LIMIT k`, `rank() <= k`), a circuit of `O(n·k)` gates,
   "exactly j of the first i rows are present" for `j < k`, is deterministic and
   decomposable, and would replace the quadratic prefix counts.
