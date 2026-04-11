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
Recursive CTEs are not supported and raise an error.

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

- **Sublinks** (``EXISTS``, ``IN``, scalar subqueries): not supported.
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
``SUM``, ``COUNT``) with a ``provenance_aggregate`` call that wraps
the original aggregate result and the provenance of the aggregated
rows.  The provenance of grouped rows is combined with ⊕
(``provenance_plus``) via ``array_agg`` + ``provenance_plus``.

After aggregation rewriting:

- :cfunc:`migrate_aggtoken_quals_to_having` moves any ``WHERE``
  comparisons on aggregate results to ``HAVING``, because
  aggregate-typed values can only be filtered after grouping.

- :cfunc:`insert_agg_token_casts` inserts type casts for
  :cfunc:`agg_token` values used in arithmetic or window functions.

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

**HAVING**:  When a ``HAVING`` clause is present,
:cfunc:`having_Expr_to_provenance_cmp` translates the ``HAVING``
predicate into a ``provenance_cmp`` gate tree.  The original
``havingQual`` is removed from the query (its semantics are now
captured in the provenance circuit).

**Where-provenance** (when ``provsql.where_provenance`` is enabled):

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
list looking for calls to the ``provenance()`` SQL function.  Each
occurrence is replaced with the computed provenance expression, so
that ``SELECT provenance() FROM ...`` returns the actual provenance
token.


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
target-list rewriting, HAVING handling, where-provenance, ...).

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
       ``provenance_aggregate`` call built by
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
