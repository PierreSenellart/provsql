Query Rewriting Pipeline
========================

This page is a detailed walkthrough of :cfile:`provsql.c`, the core of
ProvSQL.  It describes how every SQL query is intercepted and rewritten
to propagate provenance tokens.

For the high-level architecture and data-flow overview, see
:doc:`architecture`.


Entry Point: ``provsql_planner``
--------------------------------

PostgreSQL's planner hook allows extensions to intercept every query
before planning.  :cfunc:`provsql_planner` is installed by
:cfunc:`_PG_init` and is called for every query:

1. ``INSERT ... SELECT`` (``CMD_INSERT``): delegates to
   :cfunc:`process_insert_select` to propagate provenance from the
   source ``SELECT`` into the target table's ``provsql`` column.

2. ``SELECT`` (``CMD_SELECT``): checks whether any relation in the
   range table carries a ``provsql`` column (:cfunc:`has_provenance`).
   If so, calls :cfunc:`process_query` to rewrite the query tree.

3. After rewriting, the (possibly modified) query is passed to the
   previous planner hook or ``standard_planner``.

When ``provsql.verbose_level >= 20`` (PostgreSQL 15+), the full query
text is logged before and after rewriting.  At level ≥ 40, the time
spent in the rewriter is logged.


Query-Time TID / BID Classifier
-------------------------------

A second, read-only analysis runs from the same planner-hook entry,
guarded by the :ref:`provsql.classify_top_level <provsql-classify-top-level>`
GUC.  When on, :cfunc:`provsql_classify_query` (in
:cfile:`classify_query.c`) inspects the user's parsed ``Query`` and
emits a ``NOTICE`` certifying the kind of the result relation under
the existing ``provsql_table_kind`` taxonomy (``TID`` / ``BID`` /
``OPAQUE``) together with the provenance-tracked base relations the
query touches.

The classifier runs on the user's original parse tree, before any
rewriting, so the reported kind reflects the SQL the user wrote.
It is purely informational: no side effect on the query tree, no
behavioural change downstream.

**Decision rules.**  A shape gate first rejects any query carrying
any of ``hasSubLinks``, ``hasModifyingCTE``, a non-empty ``cteList``,
a non-NULL ``setOperations``, or a fromlist entry that is neither
a ``RangeTblRef`` nor an ``INNER`` / ``CROSS`` ``JoinExpr`` (recursed
into via :cfunc:`classify_fromlist_shape_ok`, which descends
through each ``JoinExpr``'s two arms to reach the underlying
``RangeTblRef``\ s).  The range table is then walked, collecting
``RTE_RELATION`` entries with ``provsql_table_info`` metadata as
sources; ``RTE_SUBQUERY`` entries (the form views take after PG
rewriting, as well as inline ``FROM``-clause subqueries) are
descended into recursively under the same shape gate, so a tracked
base relation reached through any depth of subquery nesting still
contributes to the accumulator; the PostgreSQL 18 synthetic
``RTE_GROUP`` and the ``RTE_JOIN`` synthetic alias rows PG appends
alongside every ``JoinExpr`` are skipped transparently; any other
rtekind (``RTE_VALUES``, ``RTE_FUNCTION``, ``RTE_CTE`` …) trips
the shape gate.  Outer joins (``LEFT`` / ``RIGHT`` / ``FULL``)
stay OPAQUE because their NULL-padding rows break per-row
independence; ``USING`` and aliased joins are also OPAQUE (they
would require resolving columns through ``joinaliasvars``).

The kind is then decided from the cumulative tracked-source count:

* shape failed → ``OPAQUE`` (hidden structure may carry correlations
  the classifier cannot rule out);
* shape ok and zero tracked sources → ``TID`` (no provenance
  involved, trivially deterministic);
* shape ok and exactly one tracked source → that source's recorded
  kind, refined by the BID projection check below;
* shape ok and two or more tracked sources → conservative
  multi-source TID promotion (see below)
  or fall through to ``OPAQUE``.

**BID projection preservation.**  In the single-source BID case,
:cfunc:`bid_block_key_preserved` walks the outer target list and
requires every block-key column of the source to survive -- matched
on the underlying ``Var`` (resolved transitively through any depth
of ``RTE_SUBQUERY`` ``TargetEntry`` projection, ignoring renames)
rather than on the output column name.  If any block-key column is
dropped from the projection, the result downgrades to ``OPAQUE``:
the mutually-exclusive partitioning the user could observe is no
longer visible in the output.  Whole-table BID
(``block_key_n == 0``) is trivially preserved.

**GROUP BY block-key promotion.**  A pre-dispatch special case
recognises queries of shape ``SELECT k FROM bid_t GROUP BY k``
(and the multi-column-key generalisation): each output row is
exactly one BID block, and the OR over the block's ``gate_mulinput``
children reduces to the block's key token (an independent
``gate_input``), so the output is TID under the cumulative source
list.  Conservative gate: no aggregates / window functions /
sublinks / ``HAVING`` / ``DISTINCT`` / set operations, a single
``RangeTblRef`` fromlist pointing at a BID relation, and the
``groupClause`` matching the source's block-key set exactly (no
extra group columns, no missing keys).  PG 18's synthetic
``RTE_GROUP`` entry is resolved through inline by
:cfunc:`resolve_through_group_rte`.

**Multi-source TID promotion.**  The ``n_meta >= 2`` branch no
longer collapses to OPAQUE.  :cfunc:`try_classify_multi_source_tid`
promotes the result to TID when every classifier-reported source
is itself TID and the registered ancestor sets (see
:ref:`tid-bid-propagation` below) are pairwise disjoint.  Any
failure (a non-TID source, no registry entry, or any pair of
ancestor sets overlapping) keeps the OPAQUE default.  The
hierarchical-CQ structure is *not* inspected here -- the
:ref:`safe-query-rewriter` runs the full check downstream; the
classifier's job is only to certify the per-row independence the
user-visible NOTICE pill advertises.

TID and BID NOTICEs carry a complete source list.  OPAQUE NOTICEs
deliberately omit it: when the shape gate trips on a sublink, set
operation, ``HAVING``, aggregates, window functions, or SRFs in
the target list, the rtable walk only reaches the syntactically
visible RTEs, so any list would be partial and falsely suggest
completeness.  The user already has the query text in front of
them and can identify the involved relations without our help.

**Executor-depth gating.**  ProvSQL's rewriter inserts calls to
PL/pgSQL helpers (``provenance_times``, ``provenance_plus``,
``provenance_aggregate``…) into the rewritten query.  Each of
those helpers contains internal ``SELECT`` statements that PL/pgSQL
plans on first invocation, and that planning fires the planner hook
again.  Without a gate, the classifier would emit one extra
``NOTICE`` per such internal plan.  We track ``Executor`` nesting via
an ``ExecutorStart_hook`` / ``ExecutorEnd_hook`` pair that
increments / decrements a file-local depth counter
(``provsql_executor_depth``); the classifier only emits when
``provsql_executor_depth == 0`` at planner-hook entry, which
corresponds to the user's outermost statement (the helper plans run
during execution, hence at depth ≥ 1).

**NOTICE format.**  Rendered by :cfunc:`provsql_classify_emit_notice`
with schema-qualified, ``quote_identifier``-quoted relation names:

.. code-block:: text

    ProvSQL: query result is TID (sources: public.personnel)
    ProvSQL: query result is BID (sources: public.personnel)
    ProvSQL: query result is TID (no provenance-tracked sources)
    ProvSQL: query result is OPAQUE

ProvSQL Studio enables the GUC automatically and parses the
``NOTICE`` into a kind-aware pill on the result-table ``provsql``
column; see :doc:`/user/studio`.


``process_query``: The Main Rewriting Function
-----------------------------------------------

:cfunc:`process_query` is the heart of ProvSQL.  It receives a
``Query`` tree, rewrites it to carry provenance, and returns the
modified tree.  The function is recursive: subqueries and CTEs are
processed by re-entering :cfunc:`process_query`.

The function proceeds in the following order:

Step 0: Early Exit
^^^^^^^^^^^^^^^^^^

If the query has no ``FROM`` clause (``q->rtable == NULL``), there is
nothing to track -- return immediately.

Step 1: CTE Inlining
^^^^^^^^^^^^^^^^^^^^

Non-recursive common table expressions (``WITH`` clauses) are
inlined as subqueries in the range table via :cfunc:`inline_ctes`.
This converts ``RTE_CTE`` entries to ``RTE_SUBQUERY`` so that the
subsequent recursive processing can track provenance through them.
Recursive CTEs are handled by ``lower_recursive_cte`` (inside
:cfunc:`inline_ctes`): under the ``'boolean'`` or ``'absorptive'``
provenance class, recognised reachability shapes are driven through
the bounded-treewidth compiler and other recursive queries through
the generic fixpoint ``eval_recursive`` (see
:ref:`recursive-lowering` below); outside those classes a recursive
CTE raises an error.

Before any of this, :cfunc:`normalize_distinct_into_group_by` turns a
guarded ``SELECT DISTINCT`` into its provenance-identical
``GROUP BY`` form -- it must run **before** :cfunc:`inline_ctes`
because the reachability-aggregation detectors inside it key on
``groupClause`` (a historical late call site still catches
``DISTINCT`` introduced by intervening rewrites).

Inlining happens before set-operation handling because ``UNION`` /
``EXCEPT`` branches may reference CTEs.

Step 2: Strip Existing Provenance Columns
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

:cfunc:`remove_provenance_attributes_select` scans the target list and
removes any existing ``provsql`` UUID columns (which might be inherited
from base tables or subqueries).  These are stripped so that the
rewriter can append a single, freshly computed provenance expression at
the end.  Matching ``GROUP BY`` / ``ORDER BY`` references and
set-operation column lists are also adjusted.

Step 3: Set-Operation Handling
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

If the query has ``setOperations`` (``UNION``, ``EXCEPT``):

- **Non-ALL variants** (``UNION``, ``EXCEPT`` without ``ALL``):
  :cfunc:`rewrite_non_all_into_external_group_by` wraps the set
  operation in a new outer query with ``GROUP BY`` on all columns.
  This implements duplicate elimination as provenance addition (⊕).
  The function then re-enters :cfunc:`process_query` on the wrapper.

- ``UNION ALL``: each branch is processed independently.  The
  provenance tokens from different branches are combined with ⊕
  (``provenance_plus``) by :cfunc:`process_set_operation_union`.

- ``EXCEPT ALL``: :cfunc:`transform_except_into_join` rewrites
  ``A EXCEPT ALL B`` as a ``LEFT JOIN`` with a ``provenance_monus``
  (⊖) gate, plus a filter removing zero-provenance tuples.

- ``INTERSECT`` is not supported (raises an error).

Step 4: Aggregate ``DISTINCT`` Rewrite
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

If the query has aggregates with ``DISTINCT`` (e.g.,
``COUNT(DISTINCT x)``), :cfunc:`rewrite_agg_distinct` performs a
structural rewrite: the ``DISTINCT`` inside the aggregate is moved to
an inner subquery with ``GROUP BY``, and the outer query aggregates
over the deduplicated results.  The function returns a new query tree,
and :cfunc:`process_query` re-enters on it.

Step 5: Discovery -- ``get_provenance_attributes``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

:cfunc:`get_provenance_attributes` walks every entry in the range table
and collects ``Var`` nodes pointing to provenance columns:

- **RTE_RELATION** (base table): scans column names for one called
  ``provsql`` of type UUID.

- **RTE_SUBQUERY**: recursively calls :cfunc:`process_query` on the
  subquery.  If the subquery's rewritten target list contains a
  ``provsql`` column, a ``Var`` pointing to it is added to the parent's
  provenance list.  Outer ``Var`` attribute numbers are adjusted to
  account for any columns removed from the inner query.

- **RTE_JOIN**: for inner, left, right, and full joins, nothing is
  collected directly -- the underlying base-table RTEs supply the
  tokens.  Semi-joins and anti-joins raise an error.

- **RTE_FUNCTION**: if the function returns a single UUID column named
  ``provsql``, it is collected.

- **RTE_VALUES** / **RTE_GROUP**: no provenance to collect.

If no provenance attributes are found, the query is returned unmodified.

Step 6: Unsupported Feature Checks
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Before proceeding, the function checks for:

- **Sublinks** (``EXISTS``, ``IN``, scalar subqueries) over a tracked
  relation that the decorrelation pre-passes could not rewrite (in
  practice, a body using explicit ``JOIN`` syntax or
  ``LIMIT``/``OFFSET``, or a bare uncorrelated value / ``count(*)``
  body compared against an outer column): not supported.
- ``DISTINCT ON``: not supported.
- ``DISTINCT`` (plain): converted to ``GROUP BY`` via
  :cfunc:`transform_distinct_into_group_by`.
- ``GROUPING SETS`` / ``CUBE`` / ``ROLLUP``: not supported (except
  the trivial ``GROUP BY ()``).

Step 7: Build Column Map
^^^^^^^^^^^^^^^^^^^^^^^^^

:cfunc:`build_column_map` creates a per-RTE mapping from column
attribute numbers to global column identifiers.  This is used by
where-provenance to record which columns participate in equijoin
conditions and projections.

Step 8: Aggregation Rewriting
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

If the query has aggregates, :cfunc:`replace_aggregations_by_provenance_aggregate`
walks the target list and replaces each standard aggregate (e.g.,
``SUM``, ``COUNT``) with a :sqlfunc:`provenance_aggregate` call that wraps
the original aggregate result and the provenance of the aggregated
rows.  The provenance of grouped rows is combined with ⊕
(``provenance_plus``) via ``array_agg`` + ``provenance_plus``.

After aggregation rewriting:

- :cfunc:`migrate_probabilistic_quals` moves any ``WHERE``
  comparisons on aggregate results to ``HAVING`` (and lifts
  ``WHERE`` comparisons on ``random_variable`` columns into the
  per-tuple provenance), because aggregate-typed and continuous-RV
  values both need post-classification routing the executor cannot
  do directly. See :ref:`probabilistic-qual-classifier` below for
  the routing matrix.

- :cfunc:`insert_agg_token_casts` inserts type casts for
  :cfunc:`agg_token` values used in arithmetic or window functions.

- A bare boolean aggregate in ``HAVING`` (``bool_or(x)``,
  ``NOT (every(x))``) is first normalised to the ``agg = true``
  comparison form by :cfunc:`normalize_bool_agg_having`, so the
  m-semiring routing sees a uniform aggregate-vs-constant shape.

- An ordered aggregate's ``ORDER BY`` (``aggorder``) is carried onto
  the internal token-collecting ``array_agg`` built by
  :cfunc:`make_aggregation_expression`, so order-sensitive HAVING
  comparisons (``array_agg(x ORDER BY k) = ARRAY[...]``) see the
  user's order regardless of plan shape and PostgreSQL version.

See :doc:`aggregation` for the semantics of the ``agg`` /
``semimod`` / ``value`` gates produced here, and for the exact
shape of the :sqlfunc:`provenance_aggregate` call that replaces each
``Aggref``.

Step 9: Expression Building -- ``make_provenance_expression``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

:cfunc:`make_provenance_expression` combines the collected provenance
``Var`` nodes into a single expression tree:

**Combining tokens from multiple tables** (the ⊗ / ⊕ / ⊖ choice):

- ``SR_TIMES`` (default for ``SELECT ... FROM ... WHERE``): wraps
  the list of provenance tokens in ``provenance_times(ARRAY[...])``
  to create a multiplication gate.

- ``SR_PLUS`` (``UNION ALL``): uses the single provenance token
  from the union directly (each branch already has its own token).

- ``SR_MONUS`` (``EXCEPT ALL``): wraps the two tokens in
  ``provenance_monus(left, right)``.

If a single table is in the ``FROM`` clause, no combining function is
needed -- the token is used as-is.

**GROUP BY / aggregation**:  When ``group_by_rewrite`` or
``aggregation`` is true, the combined token is wrapped in
``provenance_plus(array_agg(token))`` -- this sums the provenance of
all tuples in each group.

**Delta gate**:  For queries with aggregation but no ``HAVING``
clause, a ``provenance_delta`` gate is added.  This implements the
δ-semiring operator that normalizes aggregate provenance.

**HAVING**:  When a ``HAVING`` clause is present, the lift is gated
by the :cfunc:`needs_having_lift` walker, which returns true only
when the qual references an ``agg_token`` ``Var`` or a
:sqlfunc:`provenance_aggregate` wrapper. On that path,
:cfunc:`having_Expr_to_provenance_cmp` translates the predicate
into a ``provenance_cmp`` gate tree and the original
``havingQual`` is removed from the query (its semantics are now
captured in the provenance circuit). On the deterministic-outcome
path (e.g. ``HAVING expected(avg(rv)) > 20``, where the outer
predicate collapses to a plain Boolean), the qual is left for
PostgreSQL to evaluate natively on the surviving groups while a
per-group ``provenance_delta`` wrapper is still emitted, so the
surviving rows carry the expected provenance shape.

**Where-provenance** (when the provenance class is ``'where'``):

- **Equijoin gates**: :cfunc:`add_eq_from_Quals_to_Expr` scans
  ``JOIN ... ON`` conditions and ``WHERE`` equalities, wrapping
  the provenance expression in ``provenance_eq`` gates that record
  which column pairs were compared.

- **Projection gates**: if the output columns are a strict subset
  of the input columns (or are reordered), a ``provenance_project``
  gate is added with an integer array recording the column mapping.

Step 10: Splicing -- ``add_to_select``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

:cfunc:`add_to_select` appends the provenance expression to the
query's target list as a new column named ``provsql``.  If the query
has ``GROUP BY``, the column is added to the ``groupClause`` as well.

Step 11: Replace ``provenance()`` Calls
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

:cfunc:`replace_provenance_function_by_expression` walks the target
list looking for calls to the :sqlfunc:`provenance` SQL function.  Each
occurrence is replaced with the computed provenance expression, so
that ``SELECT provenance() FROM ...`` returns the actual provenance
token.

A :sqlfunc:`provenance` call inside a simple scalar SubLink is an
**inert** scope-local fetch: it resolves to the inner query's token
without coupling the outer query to it.  The conditioning surface
(the binary ``|`` operator / :sqlfunc:`cond`, the prefix
:sqlfunc:`given` whole-tuple rewriter, the prefix ``!`` /
:sqlfunc:`provenance_not`, and the natural ``X | (predicate)`` form
for the uuid, ``random_variable`` and ``agg_token`` carriers) is
rewritten here too, building terminal ``gate_conditioned`` gates; see
:doc:`/user/conditioning` for the semantics.


Rewriting Rules and Formal Semantics
-------------------------------------

The rewriting implemented by :cfile:`provsql.c` realises the
rewriting rules (R1)--(R5) from the ICDE 2026 paper
:cite:`sen2026provsql`, which is the authoritative reference for
the formal semantics and correctness results.  The rules are stated
over an extended relational algebra on annotated relations;
:cfile:`provsql.c` has to reproduce them on PostgreSQL ``Query``
trees, so the mapping below is approximate rather than literal --
the pipeline phases described earlier on this page interleave the
rules with PostgreSQL-specific bookkeeping (range-table surgery,
target-list rewriting, ``HAVING`` handling, where-provenance...).

.. list-table::
   :header-rows: 1
   :widths: 15 25 60

   * - Rule
     - Algebra operator
     - Concrete realisation in :cfile:`provsql.c`
   * - (R1)
     - Projection :math:`\Pi`
     - The provenance token column is *kept* on the target list.
       :cfunc:`get_provenance_attributes` collects the token
       columns from the range table and :cfunc:`add_to_select`
       appends the computed token expression back onto the
       rewritten target list.
   * - (R2)
     - Cross product / join :math:`\times`
     - The ``SR_TIMES`` branch of
       :cfunc:`make_provenance_expression` wraps the list of
       per-input tokens in a ``provenance_times(ARRAY[...])``
       (⊗) call.
   * - (R3)
     - Duplicate elimination :math:`\varepsilon`
     - ``SELECT DISTINCT``, ``UNION``, and ``EXCEPT`` (the
       non-``ALL`` set operations) are all rewritten into an outer
       ``GROUP BY`` by
       :cfunc:`transform_distinct_into_group_by` /
       :cfunc:`rewrite_non_all_into_external_group_by`, and the
       grouped tokens are combined with ``provenance_plus`` (⊕)
       via ``array_agg`` inside
       :cfunc:`make_provenance_expression`.
   * - (R4)
     - Multiset difference :math:`-`
     - ``EXCEPT ALL`` is rewritten by
       :cfunc:`transform_except_into_join` into a ``LEFT JOIN`` on
       all data columns, and the ``SR_MONUS`` branch of
       :cfunc:`make_provenance_expression` wraps the two tokens in
       ``provenance_monus`` (⊖).
   * - (R5)
     - Aggregation :math:`\gamma`
     - :cfunc:`replace_aggregations_by_provenance_aggregate` walks
       the target list and replaces each ``Aggref`` with a
       :sqlfunc:`provenance_aggregate` call built by
       :cfunc:`make_aggregation_expression` (which in turn wraps
       arguments in ``provenance_semimod``).  The enclosing
       provenance expression is then wrapped in a
       ``provenance_delta`` (δ) gate by
       :cfunc:`make_provenance_expression`.

Two algebra operators are deliberately **not** rewritten, matching
the paper:

- **Selection** :math:`\sigma` -- ``WHERE`` clauses pass through
  unchanged because selection does not affect provenance (with
  where-provenance enabled, the rewriter additionally wraps the
  result in ``provenance_eq`` / ``provenance_project`` gates, but
  that is orthogonal to R1--R5).
- **Multiset sum** :math:`\uplus` -- ``UNION ALL`` is left alone by
  :cfunc:`process_set_operation_union`; the tokens from each branch
  flow through independently.

Formal Verification
^^^^^^^^^^^^^^^^^^^

The correctness of rules (R1)--(R5) is fully machine-checked
in the ProvSQL Lean 4 library.  The unified
statement covering all five rules is
`Query.rewriting_valid_full
<https://provsql.org/lean-docs/Provenance/QueryEvaluateInVK.html#Query.rewriting_valid_full>`_
in the ``Provenance.QueryEvaluateInVK`` module, stated against
the V_K-lifted evaluator
`Query.evaluateAnnotatedFull
<https://provsql.org/lean-docs/Provenance/QueryEvaluateInVK.html#Query.evaluateAnnotatedFull>`_:
for every supported query ``q`` and every annotated database
``d``, evaluating ``q`` against the annotated semantics and
then projecting to the composite (tuple + annotation)
representation yields the same result as evaluating the
rewritten query against the composite database.  The V_K
interpretation (values lifted with their K-semimodule
annotation, Definition 7 of the ICDE 2026 paper) is what makes
the aggregation rule (R5) statable alongside (R1)--(R4) in a
single theorem.

A companion theorem,
`Query.evaluateAnnotatedFull_hom
<https://provsql.org/lean-docs/Provenance/QueryEvaluateInVKHom.html#Query.evaluateAnnotatedFull_hom>`_
in the ``Provenance.QueryEvaluateInVKHom`` module, proves
that query evaluation commutes with m-semiring homomorphisms
on the full algebra including aggregation (the formal analogue
of :cite:`DBLP:conf/pods/GreenKT07`, Proposition 3.5, and
:cite:`DBLP:journals/japll/GeertsP10`, Proposition 1, lifted to
m-semirings via ``SemiringWithMonusHom`` and extended to
aggregation via the K-semimodule structure formalised in
`Provenance.KSemiModule
<https://provsql.org/lean-docs/Provenance/KSemiModule.html>`_
and
`Provenance.LiftedTK
<https://provsql.org/lean-docs/Provenance/LiftedTK.html>`_).
The restriction of this theorem to the non-aggregation
fragment is
`Query.evaluateAnnotated_hom
<https://provsql.org/lean-docs/Provenance/QueryAnnotatedDatabaseHom.html#Query.evaluateAnnotated_hom>`_
in the ``Provenance.QueryAnnotatedDatabaseHom`` module.
This is the formal counterpart of ProvSQL's architectural
choice: a single persistent provenance circuit is kept once,
and each ``sr_*`` semiring evaluator is the realisation of one
m-semiring homomorphism out of that circuit; the theorem says
that running the homomorphism on the inputs and then evaluating
the query yields the same result as evaluating the query first
and applying the homomorphism to the annotations.

.. _probabilistic-qual-classifier:

Probabilistic-Qual Classifier
-----------------------------

The single walker :cfunc:`migrate_probabilistic_quals` covers both
the historical *agg_token* HAVING surface and the *random_variable*
WHERE/JOIN surface introduced by the continuous-distribution work.
It routes every qual into one of four mutually-exclusive classes
(the ``qual_class`` enum), plus a short tail of *mixed-error*
classes that raise a clean diagnostic rather than producing a
malformed circuit:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - ``qual_class``
     - Routing
   * - ``QUAL_PURE_AGG``
     - The qual is built only from :cfunc:`agg_token` comparators
       (HAVING on aggregate results). Moved into the HAVING list
       so :cfunc:`having_Expr_to_provenance_cmp` builds the
       corresponding ``gate_cmp`` over ``gate_agg``
       children.
   * - ``QUAL_PURE_RV``
     - The qual is built only from ``random_variable``
       comparators. Each comparator is rewritten into a
       ``gate_cmp`` whose UUID is conjoined into the row's
       provenance via ``provenance_times``; the
       original ``OpExpr`` is dropped from the WHERE clause so the
       executor never reaches the placeholder procedure that would
       raise.
   * - ``QUAL_DETERMINISTIC``
     - Ordinary SQL, left untouched.
   * - Mixed-error classes
     - Quals that compare an RV against an agg_token raise a
       structured error so users see the offending shape rather
       than a downstream evaluation failure.  A *regular*
       (deterministic) comparison conjoined with a probabilistic one
       in the same predicate is accepted: the Boolean analysis keeps
       the deterministic leaf as an ordinary-comparison node and
       routes only the probabilistic part into the circuit (needed
       by the conditioning surface's mixed predicates).

For ``QUAL_PURE_RV``, the planner-hook path also covers the
corner case of ``WHERE rv > 2`` on a FROM-less ``SELECT``: there
is no row provenance to conjoin into, so the rewriter synthesises
a single-row FROM-less host so :sqlfunc:`probability_evaluate`
reads a well-formed circuit. The dispatch in
:cfunc:`make_aggregation_expression` keys on the aggregate's
result type (``OID_TYPE_RANDOM_VARIABLE`` vs
``OID_TYPE_AGG_TOKEN``) so the same path covers any future
RV-returning aggregate. See :doc:`continuous-distributions` for
the broader architecture.

Beyond the qual positions, ``rewrite_probability_events`` (run next
to ``rewrite_cond_predicates`` in ``process_query``) lifts RV
comparisons that appear as *values* in the ``SELECT`` target list: a
projected ``x > y`` surfaces its ``gate_cmp`` token, and the
``probability(<predicate>)`` Boolean overload is rewritten into
:sqlfunc:`probability_evaluate` over the argument's event token
(``predicate_to_condition_gate``, the same lift the conditioning
surface uses). The same mutator carries a ``CaseExpr`` arm: an
RV-typed searched ``CASE`` with an RV-comparison guard is flattened
into a ``gate_case`` (``[guard_1, value_1, …, default]``) via
``rv_case``. A builtin ``GREATEST`` / ``LEAST`` over
``random_variable`` arguments -- which parses only because a default
btree operator class is declared on the type -- is rewritten in the
same pass into the ``provsql.greatest`` / ``provsql.least`` order-
statistic constructor (a ``gate_arith`` ``MAX`` / ``MIN``).

.. _safe-query-rewriter:

Safe-Query Rewriter
-------------------

The safe-query rewriter (``src/safe_query.{c,h}``) is an opt-in
pre-pass invoked from ``process_query`` in ``provsql.c`` when the
GUC the provenance class is ``'boolean'`` (see
:doc:`../user/probabilities`).  It recognises the safe class of
Dalvi and Suciu :cite:`DBLP:journals/jacm/DalviS12`, namely
self-join-free hierarchical conjunctive queries, and rewrites them
with per-atom
``SELECT DISTINCT`` projections so the resulting provenance circuit
is read-once.  A read-once circuit is probability-evaluated in
linear time by :cfunc:`BooleanCircuit::independentEvaluation`,
which replaces the fallback to tree decomposition or external
knowledge compilation that the unrewritten circuit would require.

Two normalisation pre-passes run at the head of
``try_safe_query_rewrite`` so the detector and rewriter see a flat
fromlist of base ``RTE_RELATION`` entries regardless of how the
user wrote the query:

- ``try_flatten_inner_joins(q)`` dissolves every ``INNER`` /
  ``CROSS`` ``JoinExpr`` in any fromlist into flat
  ``RangeTblRef``\ s plus AND-merged ``ON``-clauses, recursing
  through ``RTE_SUBQUERY`` bodies so a wrapped INNER JOIN gets
  flattened in the inner body before the inlining pre-pass picks
  it up.  Refuses outer joins (NULL-padding rows break per-row
  independence), aliased joins (``JOIN ... AS j`` would require
  resolving columns through ``joinaliasvars``), and ``USING``
  clauses.  Orphaned synthetic ``RTE_JOIN`` entries are dropped
  via the shared :cfunc:`compact_orphan_rtes` helper.
- ``try_inline_simple_subqueries(q)`` then inlines every simple
  ``RTE_SUBQUERY`` fromlist entry (PG-rewritten view bodies,
  inline ``FROM (SELECT …)`` subqueries) into the outer rtable.
  A subquery is "simple" when it is a flat conjunctive ``SELECT``
  with no kind-altering features (no ``DISTINCT`` / ``GROUP BY``
  / ``HAVING`` / aggregates / window functions / set operations
  / sublinks / CTEs / ``ORDER BY`` / ``LIMIT`` / ``OFFSET`` /
  SRFs), no ``LATERAL`` or security-barrier member RTEs, and a
  target list of plain base-level ``Var``\ s (possibly through
  ``RelabelType`` wrappers).  Fixed-point loop handles nested
  views.

The disjoint-base-ancestor property is enforced downstream by the
candidate gate's existing shared-relid bail (modulo the PK /
disjoint-constant self-join rescues): two fromlist entries that
inline to the same base relid produce duplicates the gate
refuses.  A second, finer-grained check (see
:ref:`tid-bid-propagation` below) consults the per-relation
ancestor registry to refuse joins whose registered ancestor sets
overlap through different relids.

After normalisation, two stages, both static helpers in
``src/safe_query.c``:

1. ``is_safe_query_candidate(q)`` : cheap shape gate.  Rejects
   ``hasAggs`` / ``hasWindowFuncs`` / ``LIMIT`` / ``OFFSET`` /
   ``groupingSets`` / sublinks / set operations / explicit
   ``JoinExpr`` ; rejects any ``RangeTblEntry`` that is not
   ``RTE_RELATION`` (the normalisation pre-passes above remove
   the legitimate ``RTE_SUBQUERY`` / ``RTE_JOIN`` cases) ;
   requires an outer ``GROUP BY`` or top-level ``DISTINCT`` so
   the per-atom ``DISTINCT`` wraps do not change the user-visible
   row count ; requires every tracked base relation to carry a
   TID or BID classification in the per-database
   ``provsql_table_info`` registry ; requires the registered
   ancestor sets of any two distinct-relid RTEs to be disjoint
   (catches CTAS-derived TID joined with one of its sources, or
   two CTAS-derived TIDs sharing a base ancestor through
   different relids -- same-relid pairs are deliberately exempted,
   those go through the existing PK-unification /
   disjoint-constant rescues).  The OPAQUE
   classification (which bails the gate) covers tables where the
   user has bypassed the planner's UUID injection by writing a
   non-fresh ``provsql`` value directly : the rewriter cannot
   assume input independence for such tuples, so any query
   touching them falls through to the standard pipeline.
2. ``find_hierarchical_root_atoms(q)`` : variable-equivalence
   analysis on the surviving atoms.  Walks every equijoin
   predicate, unions the variables it links via a union-find
   structure, and looks for a root variable: one whose
   atom-set covers every atom (or, in the multi-level case, every
   atom of an inner group whose remaining variables also admit a
   root).  Cases handled:

   - **Single-component fully covered.**  Every atom has the root
     variable in a known column.  The rewriter emits one wrapped
     subquery per atom, joining on the root.
   - **Multi-level partial coverage.**  Some atoms do not bind
     the root.  The rewriter groups the missing atoms by the
     variables they actually share and recurses on each group as
     an inner wrapped subquery, which on re-entry is rewritten
     again by the same pipeline: :cfunc:`get_provenance_attributes`
     re-runs :cfunc:`process_query` on each subquery, so no
     explicit recursion is needed in the rewriter itself.
   - **Disconnected components.**  Atoms decompose into
     connected components by shared variables ; each component
     becomes an independent wrapped subquery joined by Cartesian
     product at the outer level.
   - **Head variables.**  A head variable that happens to also
     appear in atoms is propagated through the wrapping.
     Several positional sub-cases are pinned (single-atom head
     Vars, head Vars on grouped atoms in or out of first member
     position).  When the head Var falls on no atom with a
     defined slot (the **bridge case**), the rewriter currently
     bails rather than rewriting incorrectly.

3. ``rewrite_hierarchical_cq(q, atoms, groups, residual)`` : the
   actual surgery.  For every atom in the partition, builds an
   ``RTE_SUBQUERY`` whose body is
   ``SELECT DISTINCT <root-binding cols>, provsql FROM A WHERE
   <atom-local predicates>``, replaces the original
   ``RTE_RELATION`` in ``q->rtable``, rebuilds the ``jointree``
   ``fromlist`` accordingly, and rewrites any WHERE / join
   predicate so Vars refer to the new subqueries' output
   columns.  The detector's BID block-key alignment check
   ensures that the chosen root-binding columns in every BID
   atom are a superset of that atom's block_key, so the
   ``mulinput`` structure survives the projection.

When the rewriter accepts a query, it returns a new ``Query`` that
is re-entered into ``process_query`` from the top (same recursion
pattern as ``rewrite_agg_distinct``).  When it rejects, it returns
``NULL`` and the planner falls through to the existing pipeline
unchanged.

The root gate of every rewritten circuit is wrapped in a
``gate_assumed`` marker labelled ``'boolean'`` (see
:doc:`semiring-evaluation`) so that semirings whose algebra is not
Boolean-faithful refuse to evaluate it at runtime.

The predicate-tree analyses the detector relies on -- AND flattening,
``Var = Var`` equijoin and ``Var = Const`` selection recognition,
splitting a conjunction into per-relation selections and the
cross-relation residual -- live in the shared
:cfile:`qual_classify.c` module (``qc_flatten_and``, ``qc_is_var_eq``,
``qc_is_var_const_eq``, ``qc_split_quals``), which the joint-width
recogniser below consumes as well.

FD-Aware Extensions
^^^^^^^^^^^^^^^^^^^

Six extensions layered onto the base detector recover safety for
query shapes that the raw ``atom-set covers every atom`` criterion
refuses.  All operate through the same ``DETERMINED`` matrix and
the per-atom ``atom_anchor_class`` selection in the ``fd_aware_mode``
branch of ``find_hierarchical_root_atoms``; the theoretical
framework is the induced-FD construction Γ\ :sub:`p`\ (q) of Dalvi
and Suciu :cite:`DBLP:journals/vldb/DalviS07`, the dissociation
framework of Gatterbauer and Suciu
:cite:`DBLP:journals/pvldb/GatterbauerS15`, and the textbook
treatment in :cite:`DBLP:series/synthesis/2011Suciu` (Chapter 4).

The extensions and their interaction with the base rewriter:

- **Constant-selection elimination.**  A WHERE conjunct of shape
  ``Var = Const`` induces the FD ``∅ → Var.attno``; the equijoin
  closure propagates the literal to every variable in the same
  union-find class.  Implemented as ``apply_constant_selection_fd_pass``
  (a pre-pass between ``safe_split_quals`` and the multi-component
  dispatcher), which propagates synthesised ``Var = const`` conjuncts
  to every atom touched by the class and drops the now-redundant
  equijoin conjuncts from the residual.  Atoms whose every Var is
  in a constant class become disconnected from the rest of the
  residual and route through the multi-component path, which emits
  them as independent ``gate_plus`` subqueries Cartesian-joined at
  the outer level.  That factoring is what preserves read-once
  across multiple-match rows on the rest of the query.

- **Primary-key / NOT-NULL UNIQUE FDs.**  A relation with primary
  key ``K`` carries ``K → A`` for every non-key attribute ``A``;
  NOT-NULL UNIQUE is FD-equivalent.  A separate per-backend cache
  (``provsql_lookup_relation_keys`` in :cfile:`provsql_utils.c`)
  scans ``pg_constraint`` filtered to ``contype IN ('p','u')``,
  joins ``pg_index`` for column lists, and verifies
  ``pg_attribute.attnotnull`` on every UNIQUE column.  Invalidation
  hooks into ``CacheRegisterRelcacheCallback`` with its own
  registration flag so ``ALTER TABLE ADD/DROP CONSTRAINT`` refreshes
  the next lookup without polling.  The detector applies each FD
  once: when every key column's union-find class is multi-atom (the
  determinant is pinned by some equijoin to a Var on another RTE),
  every non-key column's class is tagged ``DETERMINED(c, rte)`` and
  drops from the class's FD-aware atom set.  Composite PKs require
  every column to satisfy the multi-atom check (the soundness trap
  on partial coverage).  When no single class covers every atom
  under the raw count but the FD-reduced atom sets are pairwise
  nested-or-disjoint, the rewriter falls into ``fd_aware_mode`` and
  uses a per-atom local-root anchor instead of a global root.  For the
  canonical ``R(x), S(x,y), T(y)`` shape (two join classes, ``S``'s key
  determining ``y``) the flat per-atom wrap is read-once only when the
  determined value ``y`` is distinct across keys; when several keys
  collide on one ``y`` it would reuse the shared ``T(y)`` leaf.  The
  detector recognises this and instead folds the determining component
  ``{R, S}`` into an inner sub-query **grouped on the determined value**
  ``y`` (projecting ``x`` out), so the shared leaf factors out and the
  lineage is read-once for all data -- the Dalvi-Suciu safe plan that
  applies the independent project to ``{R, S}`` before joining ``T``.
  Exercised by the collision case in ``test/sql/safe_query_pk_fd.sql``.

- **Deterministic-relation transparency.**  A relation that is not
  provenance-tracked (no ``provsql`` column and no metadata entry)
  contributes probability-1 tuples; dissociating tuples in it does
  not change the query's probability
  :cite:`DBLP:journals/pvldb/GatterbauerS15`.  The detector treats
  such atoms as structurally transparent for atom-set purposes by
  setting ``DETERMINED(c, rte)`` for *every* union-find class on
  the deterministic RTE, so each class drops the atom from its
  FD-aware set.  Soundness guards on ``pg_class.relkind = 'r'`` and
  ``has_superclass = false`` exclude views, foreign tables, and
  inheritance children; the residual CTAS-correlation trap
  (``CREATE TABLE foo AS SELECT * FROM <tracked>`` without
  ``add_provenance``) is closed by the lineage hook (see
  :ref:`tid-bid-propagation`) and the ancestry-based
  disjointness gate.  An anchor-fallback pass
  selects any anchored multi-atom class as the local root for
  deterministic atoms (the FD-aware preference for non-determined
  classes leaves them orphan otherwise).  Each deterministic atom's
  wrap uses the standard ``SELECT DISTINCT slots FROM dim WHERE
  <filter>`` shape with no ``provsql`` column; ``process_query``'s
  recursion finds no provenance to add, and the outer
  ``gate_times`` simply skips the missing entry.

- **FD closure.**  Detector-only: accept any query whose FD-reduced
  atom-sets are pairwise nested-or-disjoint *and* whose existing
  single-level wrap is read-once.  Delivered by the cumulative
  constant-selection + PK-FD + deterministic-transparency passes
  -- each FD application in the current rule set is independent of
  the others, so no fixpoint iteration is needed.  The canonical
  example is the triangle CQ with PKs on two of its three
  relations: each PK FD is applied once and the FD-aware atom sets
  become disjoint pairwise.  The FD-induced nested rewrite for
  shapes where the single-level wrap is not read-once (the
  function/free split) is deliberately deferred.

- **PK-unifiable self-joins.**  When two (or more) RTEs over the
  same relation have all PK / NOT-NULL UNIQUE columns equated
  through the union-find closure, the key proves they refer to the
  same tuple.  ``try_pk_self_join_unification`` runs before
  :cfunc:`is_safe_query_candidate` and merges every fully-unifiable
  group into a single survivor: ``rtable`` is compacted,
  ``jointree->fromlist`` renumbered, and every Var's ``varno`` (and
  the parallel ``varnosyn`` for ``ruleutils.c``-style deparsing) is
  rewritten through ``safe_unify_remap_mutator``.  Partial
  unification (a 3-RTE group with one outlier) bails the entire
  group: full unification or full bail.  The candidate gate's
  shared-relid bail then sees one RTE per surviving relid and
  accepts.

- **Disjoint-constant self-joins.**  When two same-relid RTEs carry
  mutually exclusive ``Var = Const`` conjuncts on the same column,
  their tuple-sets are disjoint and the ``provsql`` tokens never
  overlap.  ``try_disjoint_constant_self_join_split`` runs after
  the PK-unification pass and certifies eligible relids in a
  ``Bitmapset``; :cfunc:`is_safe_query_candidate` consults the set and
  skips the shared-relid bail for those relids.  Pairwise
  disjointness uses ``datumIsEqual`` on matching ``consttype`` to
  prove distinct literals; non-equality predicates and constants on
  different columns do not contribute.  The rewriter is unchanged:
  each RTE becomes its own ``SELECT DISTINCT slots FROM R WHERE
  <filter>`` wrap, with the disjoint partition guaranteeing
  read-once.

The interaction between these extensions and the base rewriter is
ordered as: PG-18 group-RTE strip → INNER / CROSS JoinExpr
flattening → simple-subquery inlining → PK self-join unification
→ disjoint-constant certification → candidate gate (shape /
metadata / ancestry disjointness) → ``safe_split_quals`` →
constant-selection pre-pass → multi-component dispatch →
``find_hierarchical_root_atoms`` (PK FDs, deterministic
transparency, ``fd_aware_mode``) → ``rewrite_hierarchical_cq``.
The detector's ``DETERMINED`` matrix accumulates contributions
from the PK-FD pass and the deterministic-transparency pass; the
``atom_anchor_class`` selection makes a first FD-aware preference
pass followed by a fallback for atoms whose every anchored class
is FD-determined (needed for the deterministic case to land its
local-root choice).

Each extension's soundness argument is recorded in inline comments
in :cfile:`safe_query.c` alongside the corresponding block; the
regression tests live in ``test/sql/safe_query_const_sel.sql``,
``safe_query_pk_fd.sql``, ``safe_query_deterministic.sql``,
``safe_query_self_join_pk.sql``, ``safe_query_fd_closure.sql``
(cumulative regression checks for the FD closure),
``safe_query_self_join_disjoint.sql``,
``safe_query_view_descent.sql`` (subquery-inlining pre-pass),
``safe_query_inner_join.sql`` (JoinExpr flattening), and
``safe_query_ancestry_disjoint.sql`` (ancestry-based disjointness
gate).

.. _recursive-lowering:

Recursive-Reachability Lowering
-------------------------------

Under the ``'boolean'`` or ``'absorptive'`` provenance class,
``lower_recursive_cte`` pattern-matches a recursive CTE against
the reachability shapes (a constant or ``SELECT v FROM sources`` base
arm; a recursive arm joining a tracked edge relation on a
mergejoinable equality, including the undirected ``CASE`` /
``IN (src, dst)`` spelling, deterministic edge filters, derived edge
subqueries, and the hop-counting bounded variant) and, on a match,
drives ``provsql.eval_reachability``: gather the edges (consulting the
table-characterisation registry for TID / BID fast paths), compile all
reachable vertices along a tree decomposition of the data graph
(:cfile:`ReachabilityCompiler.cpp`), materialise the certified
circuits, and fill the working table.  Any failure (treewidth cap,
non-input tokens, unrecognised shape) falls back to the generic
fixpoint ``eval_recursive`` with a verbosity-gated notice.

Above the CTE, ``detect_reach_aggregations`` recognises
``GROUP BY`` / ``DISTINCT`` aggregations over ``reach JOIN members``
(planting per-group any-member circuits at plus-canonical addresses,
with deterministic member-local filters pushed into the gathering)
and reachability self-join conjunctions (k-terminal coverage, planted
at times-canonical addresses); see :doc:`/user/probabilities` for the
user-level semantics.

Joint-Width and Möbius Substitution
-----------------------------------

Still under the ``'boolean'`` class, :cfile:`joint_width_query.c`
recognises UCQ-existence and per-answer shapes over tracked relations
-- comma lists, ``JOIN ... ON``, subquery pull-up, ``UNION`` as a
multi-disjunct UCQ, ``GROUP BY`` heads of any type, single-relation
prefilters split off via ``qc_split_quals`` -- and substitutes a
transparent provenance expression: :sqlfunc:`ucq_mobius_provenance`
(``_answer``) when the Möbius recogniser accepts (safe UCQs needing
Möbius inversion; declines unless every fact token is a bare
``gate_input``), else :sqlfunc:`ucq_joint_provenance` (``_answer``)
for the joint-width compiler.  Both materialise certified circuits
under a subtransaction and fall back to the standard token on any
failure, so a recognised query never errors.

Route precedence is enforced by an ``in_boolean_rewrite`` flag
threaded through :cfunc:`process_query` /
:cfunc:`get_provenance_attributes` into subqueries: when the
safe-query rewriter or the inversion-free certifier has committed to
a subtree, the joint-width / Möbius recognisers decline throughout
it.  The overall order is therefore safe-query and inversion-free
first (linear), then Möbius (guaranteed PTIME where it fires), then
joint width.

.. _tid-bid-propagation:

TID / BID Propagation Through Derived Relations
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The per-relation metadata that gates the safe-query rewriter now
flows through view / CTAS / matview / ``SELECT INTO`` derivations
so a query that reaches a tracked base relation through any depth
of indirection is recognised by the rewriter and the classifier.

**Per-table base-ancestor registry.**  The
``ProvenanceTableInfo`` record (in :cfile:`MMappedTableInfo.h`)
carries two halves: the kind / block_key half (set by
``add_provenance`` / ``repair_key`` / ``set_table_info``) and the
ancestor half ``(ancestor_n, ancestors[PROVSQL_TABLE_INFO_MAX_ANCESTORS])``
storing the sorted-deduplicated ``pg_class`` OIDs of the original
``add_provenance`` / ``repair_key`` relations a derived table's
atoms ultimately come from.  Base tables seed ``{self}``;
CTAS-derived tables inherit the transitive union of their source
ancestor sets via the lineage hook below.  Three IPC opcodes (``A``
set ancestry, ``a`` get ancestry, ``R`` remove ancestry) and a
parallel :cfunc:`provsql_lookup_ancestry` backend cache in
:cfile:`provsql_utils.c` keep the registry off the planner's hot
path while propagating ``ALTER TABLE`` / ``DROP TABLE`` /
``add_provenance`` / ``repair_key`` invalidations through the
existing relcache-invalidation channel.  The SQL surface
(``set_ancestors`` / ``remove_ancestors`` / ``get_ancestors``)
mirrors the kind-half API.

**CTAS / SELECT INTO / matview lineage hook.**  A
``ProcessUtility_hook`` in :cfile:`provsql.c` intercepts every
``CreateTableAsStmt`` (covering ``CREATE TABLE AS``,
``CREATE MATERIALIZED VIEW``, and ``SELECT INTO``: PG's parser
transforms the third into the same statement node).  Pre-pass:
:cfunc:`provsql_classify_query` runs on the inner ``Query``;
when the classifier returns TID or BID and the target list
projects a ``provsql`` column from a tracked, non-OPAQUE source,
the hook captures the inherited kind plus the transitive ancestor
union (via :cfunc:`provsql_lookup_ancestry` on each
classifier-reported source).  After delegating to
``standard_ProcessUtility``, the post-pass resolves the new
relation via ``RangeVarGetRelid``, aligns BID block-key columns
to their output ``resno`` through a target-list walk (demotes to
TID when any key column is dropped), calls ``set_table_info`` /
``set_ancestors`` via ``DirectFunctionCall``, and installs
``provenance_guard`` via SPI.  Matviews skip the trigger install
(PG forbids triggers on them; matview content only changes
through ``REFRESH MATERIALIZED VIEW``, which re-runs the inner
SELECT and carries the same lineage).  The hook fires only when
the inner SELECT projects a ``provsql`` column from a tracked
source -- otherwise the new relation has no ``provsql`` column
and lineage metadata would be operationally pointless.

**Ancestry-based disjointness gate.**  After the existing
per-RTE metadata gate, :cfunc:`is_safe_query_candidate` computes
each RTE's ancestor set (registry lookup, fallback to ``{self}``
on the rare race where the registry has no entry) and rejects
any pair of RTEs with different relids whose ancestor sets
overlap.  Catches the case the syntactic shared-relid bail
misses: a CTAS-derived table joined with one of its source
tables, or two CTAS-derived tables sharing a base ancestor
through different relids.  Same-relid pairs are deliberately
exempted -- those go through the existing PK-unification /
disjoint-constant rescues, which prove disjointness at the gate
level on a same-relid basis (a coarser ancestry overlap check
would undo those rescues).

NULL Handling in the Rewriter
------------------------------

The user-level contract is in :doc:`/user/nulls`: predicates follow
SQL's three-valued logic on the actual data, a condition evaluating to
*unknown* annotates the tuple with the semiring zero, and set operations
match tuples syntactically.  Deterministic predicates need no rewriter
work (the executor evaluates them, 3VL included; only surviving rows'
tokens are combined).  The rewriter must be NULL-aware exactly where it
builds predicates of its own -- the negative fragment -- and in the
comparison lift:

- **EXCEPT antijoin matching.** :cfunc:`transform_except_into_join`
  builds its per-column antijoin condition as
  ``NOT (l IS DISTINCT FROM r)`` (a ``DistinctExpr`` under a ``NOT``),
  not plain ``=``: SQL's set difference treats two NULLs as the same
  value, and the outer dedup ``GROUP BY`` of the same rewrite already
  groups NULL-identically.

- **Quantified-sublink removal condition.** In
  ``extract_quantified_corr``, when the lift's final sense is the
  antijoin (``NOT IN``, ``op ALL``, ``NOT (op ANY)``), each correlation
  conjunct becomes ``(x ¬op q) OR x IS NULL OR q IS NULL``: under 3VL an
  *unknown* comparison also prevents the outer row from being an answer,
  so a NULL on either side must count as a match of the removal
  condition.  Per-side guards are skipped when that side is provably
  non-nullable (``expr_provably_not_null``: non-NULL constants and
  ``attnotnull`` base columns), which keeps the NULL-free path in its
  historical form.  The semijoin sense needs no guards -- matching only
  the *true* rows is SQL's own top-level conflation of unknown with
  false.

  With the guards, a correlation-matched subquery row can be NULL in
  every data column, so no data column can key the
  ``count(*) -> count(Q.key)`` rewrite that keeps the decorrelation's
  null-padded row out of the count.  ``oj_wrap_body_with_match_ind``
  therefore wraps the body into a derived subquery projecting a constant
  ``provsql_match_ind`` column -- non-NULL on every genuine row,
  NULL-padded on the antijoin row -- and the ``aggstar`` arm of
  ``decorrelate_scalar_sublinks`` prefers it as the count key.

- **Comparison gates.** ``rv_OpExpr_to_provenance_cmp`` emits
  ``gate_zero`` at plan time when an operand is a NULL constant, and
  ``provenance_cmp`` (deliberately not ``STRICT``) returns
  ``gate_zero`` when a NULL flows in at execution -- a NULL
  ``random_variable`` cell, or an aggregate that is NULL on the
  instance.  A STRICT comparison would instead produce a NULL token that
  ``provenance_times`` drops as the neutral 1, silently turning
  "unknown" into "certainly true"; no rewrite may let that happen.

- **Unlowered outer joins.** ``check_unlowered_outer_joins`` runs after
  every outer-join-absorbing rewrite and raises when a
  provenance-tracked relation sits on a null-padded side of a surviving
  ``LEFT``/``RIGHT``/``FULL`` join (the ``RTE_JOIN`` discovery arm would
  treat it as an inner join).  ProvSQL-generated antijoins are marked
  with the ``PROVSQL_JOIN_ALIAS`` eref sentinel and skipped; a fully
  untracked padded side is sound as-is and allowed.

- **Aggregation.** ``provenance_semimod`` returns NULL for a NULL
  aggregated value and ``provenance_aggregate`` strips those slots, so
  NULL inputs never enter the semimodule combination; the ``count(expr)``
  rewrite (``CASE WHEN expr IS NOT NULL THEN 1 ELSE 0 END``) preserves
  count's all-rows view.  For ``avg`` over ``random_variable`` the
  planner emits the two-argument, value-aware
  ``rv_aggregate_indicator``, NULL exactly when the row's value
  is NULL, so a NULL cell drops out of the denominator count as well.
  The ``HAVING`` world enumeration counts a world as passing only when
  the aggregate is defined there (see ``provsql_having``).

NULL provenance *tokens* are a separate concern from data NULLs: each
SQL combinator maps a NULL token slot to its own neutral element (⊗: 1,
untracked source; ⊕ and the monus right-hand side: 0, absent row /
no match; δ: 1).  The invariant that makes this sound is stated with the
combinators in ``sql/provsql.common.sql`` and in :doc:`/user/nulls`: a
NULL token never means "false" -- unknown conditions get an explicit
``gate_zero``.

Regression coverage: ``null_semantics.sql`` (the [GL17] ``NOT IN`` /
``NOT EXISTS`` / ``EXCEPT`` trio with hand-computed probabilities, set
and bag difference over NULLs, NULL grouping keys, NULL aggregates under
``HAVING``, NULL ``random_variable`` comparisons and aggregates, and the
outer-join refusal), plus the NULL steps of ``casestudy5.sql`` and
``casestudy6.sql``.
