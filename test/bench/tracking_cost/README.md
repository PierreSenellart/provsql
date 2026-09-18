# Cost of provenance tracking

Plain SQL against ProvSQL, query by query, one query per SQL construct
(`queries.py`: scans, joins, set operations, aggregates, HAVING, subqueries,
outer joins, a recursive CTE). No semiring evaluation, no probability: what is
measured is the rewriting and the building of the circuit.

For each query:

- `plain`: `provsql.active = off`, best of 3;
- `cold`: `provsql.active = on` on fresh copies of the tracked tables, so that
  no gate of the query exists yet;
- `warm`: the same query again, every gate already in the store;
- `profile`: a second cold run with `track_functions = all`, giving the time
  spent in each ProvSQL function and in each plan node.

Times come from `EXPLAIN (ANALYZE)`; the first query after a server restart
is an outlier and should be discarded.

```
createdb provsql_bench
psql -X provsql_bench -f test/bench/tracking_cost/setup.sql
python3 test/bench/tracking_cost/bench.py --db provsql_bench --out results.json
python3 test/bench/tracking_cost/bench.py --only join_2 union   # a subset
python3 test/bench/tracking_cost/bench.py --set provsql.gate_cache_size=1024
```

`setup.sql` builds the untracked source tables (100,000 rows) that `bench.py`
copies into tracked tables before every cold run. Needs `psycopg2`.

In September 2026 this benchmark drove the audit that moved the gate builders
to C, made every store write unanswered and buffered the worker's reads:
typical queries went from 100–300 times the plain query to 15–45 times.
