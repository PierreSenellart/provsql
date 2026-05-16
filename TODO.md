# TODO

Ideas surfaced during the safe-query / `provsql.boolean_provenance`
discussion that are deferred but worth coming back to.

## Boolean-only optimisations

Currently implemented: hierarchical-CQ safe-query rewriter
(`src/safe_query.c`), Boolean-folded gate marker (`gate_assumed_boolean`).
Next up: `foldBooleanIdentities` (idempotence, absorption,
plus-with-one absorber) as a new phase of `foldSemiringIdentities`.

Further down the road:

- **Factor-structure probability via independent-subtree detection.**
  Even when the safe-query rewriter doesn't apply, scan the circuit
  for `gate_plus` / `gate_times` whose children have pairwise-disjoint
  leaf supports; evaluate probability componentwise.  Strictly more
  general than `independentEvaluation` on read-once circuits.
- **Read-once recognition (Golumbic-Mintz-Rotics, linear-time).**
  When a Boolean formula has a read-once representation, the same
  probability shortcut as safe-query applies without needing to
  recognise the SQL shape upstream.
- **First-class negation gate (`gate_not`).**
  Lets monus normalise to `a ∧ ¬b` under Boolean and opens De
  Morgan / NOT EXISTS / EXCEPT rewrites.
- **Larger tractable CQ subclasses.**
  Forest-shaped CQs ; queries parameterised by treewidth of the
  hypergraph ; the Möbius-inversion path for non-hierarchical CQs.
- **BDD / SDD compilation with Boolean-only minimisation.**
  When the circuit is neither read-once nor independent-factorable,
  compile to a decision diagram ; under Boolean, the variable
  ordering and reduction rules are well-known and the resulting
  structure supports model counting in linear time.

## TID / BID propagation through derived relations

The per-relation `provsql_table_info` store today only records the
kind a user set at `add_provenance` / `repair_key` time.  Derived
relations (CTAS, views) start at the default TID classification ;
that's right for many simple cases but wrong in general.

- **`propagate_provenance(T regclass, source regclass)` helper.**
  After a CTAS that's syntactically TID-preserving (projection +
  filter only, or single-FROM with GROUP BY) the user calls this to
  copy `source`'s kind into `T`'s entry.  First slice : do not try
  to auto-detect ; require the user to assert by calling.
- **Multi-source CTAS classifier.**
  Run a subset of `safe_query.c`'s detector on the source query to
  decide TID / BID / OPAQUE automatically.
- **View descent in the safe-query detector.**
  Today the detector treats `RTE_VIEW` / `RTE_SUBQUERY` as opaque.
  Recursing into the view body and re-classifying inline would let
  the safe-query rewriter fire on queries that join through simple
  views.
