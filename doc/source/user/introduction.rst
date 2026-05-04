Introduction
============

ProvSQL is a PostgreSQL extension that transparently tracks the *provenance*
of query results – a record of which input tuples contributed to each output
tuple, and how. This is described in
:cite:`sen2026provsql,DBLP:journals/pvldb/SenellartJMR18`; a general survey
of provenance in databases can be found in
:cite:`DBLP:conf/rweb/Senellart19,DBLP:journals/sigmod/Senellart17`.

What Is Provenance?
--------------------

Database provenance answers the question: *where does this result come from?*
Given a query result tuple, provenance identifies the source tuples that
produced it and the algebraic relationships between them
:cite:`DBLP:conf/pods/GreenKT07`.

ProvSQL represents provenance as a *circuit* of gates: leaf gates correspond
to input tuples, and internal gates encode the semiring operations (join,
union, set difference, aggregation) that the query applied to derive the
result. This circuit is stored compactly alongside the data and can be
evaluated in many different ways.

What Can ProvSQL Do?
---------------------

Once provenance is tracked, ProvSQL enables a range of computations over
query results without changing the queries themselves:

* **Semiring evaluation** – evaluate the provenance circuit in any
  user-chosen semiring: Boolean reachability, formula display,
  why-provenance, counting, security levels, and more.

* **Probability computation** – assign independent probabilities to
  provenance tokens and compute the exact or approximate probability that each query
  result holds in the probabilistic database.

* **Shapley and Banzhaf values** – quantify the contribution of each input
  tuple to a query result using game-theoretic fairness measures.

* **Where-provenance** – track which specific cell in the source data each
  output value was copied from.

* **Aggregate provenance** – track provenance through ``GROUP BY`` queries
  and aggregate functions.

* **Data-modification tracking** – record the provenance of ``INSERT``,
  ``UPDATE``, and ``DELETE`` operations, enabling temporal queries and undo.

How It Works
-------------

ProvSQL installs a PostgreSQL *planner hook* that rewrites every query at
plan time to append a provenance expression to the result. From the user's
perspective, queries are unchanged: each result row simply gains an extra
``provsql`` UUID column that identifies its provenance circuit gate. All
subsequent computations (probability, Shapley values, semiring evaluation)
operate on these tokens through ordinary SQL function calls.
