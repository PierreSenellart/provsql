Where-Provenance
================

Where-provenance is the *finest-grained* form of provenance ProvSQL
tracks: for every output **cell**, it records the set of source
table cells that the value was copied from.  This is in contrast to
the semiring (why-provenance and friends) world, which works at the
*tuple* level: a semiring annotation tells you which input rows
contributed to an output row, but says nothing about where each
individual value came from.  See :doc:`../user/where-provenance`
for the user-facing description.

The persistent circuit store does not change when
``provsql.where_provenance`` is enabled: it is still the same DAG
of gates held in the mmap backend (see :doc:`memory`).  What
changes is that the rewriter emits two additional gate types
(``project`` and ``eq``) and -- on the C++ side -- where-provenance
queries use a different in-memory reconstruction class
(:cfunc:`WhereCircuit`) whose evaluator implements column-wise
semantics, instead of the :cfunc:`GenericCircuit` /
:cfunc:`BooleanCircuit` classes used by the semiring evaluators.


Why a Separate Circuit Class?
-----------------------------

A semiring takes a value :math:`v` and combines values with
``plus`` / ``times`` / ``monus``, but the result is just another
value of the same carrier type.  Where-provenance is fundamentally
different: the result is a *vector* of locator sets, one entry per
output column.  Plus and times must be promoted to act
column-wise, and two new operators (``project`` and ``eq``) appear
that have no semiring counterpart at all.  Rather than shoehorn
this into the semiring interface, ProvSQL uses a dedicated
:cfunc:`WhereCircuit` class whose evaluator implements column-wise
semantics directly.  The underlying gate DAG that
:cfunc:`WhereCircuit` walks is a sub-view of the same persistent
mmap store that semiring evaluation reads from -- the two views
just pay attention to different gate types.


The ``WhereCircuit`` Data Model
-------------------------------

A :cfunc:`WhereCircuit` is the in-memory reconstruction of a
sub-DAG of the mmap store specialised for where-provenance
evaluation.  Every gate has one of the following types (declared
as :cfunc:`WhereGate` in :cfile:`WhereCircuit.h`):

- **IN** -- a leaf representing one source tuple.  Carries the
  origin table name, the tuple's UUID, and the number of columns.
- **TIMES** -- conjunction of children's locator vectors,
  *concatenated* column-wise.  Used for joins / cross products.
- **PLUS** -- disjunction of children's locator vectors, *unioned*
  column-wise.  Used for ``UNION`` and duplicate elimination.
  All children must have the same number of columns.
- **PROJECT** -- restricts and reorders the columns of its single
  child.  Carries an integer vector ``positions[i] = j`` meaning
  "output column *i* takes its locators from input column *j*"
  (or 0 for "no source", e.g. an expression that is not a bare
  ``Var``).
- **EQ** -- merges the locator sets of two columns of its single
  child.  Used for equijoins: when ``A.x = B.y`` is part of the
  join condition, the EQ gate makes the output column for ``A.x``
  carry the locators of *both* ``A.x`` and ``B.y``.

A :cfunc:`WhereCircuit` is reconstructed in memory by
:cfile:`where_provenance.cpp` from the persistent mmap store every
time the SQL function ``where_provenance(token)`` is called.


Evaluation: Locators
--------------------

The output of evaluation is a ``std::vector<std::set<Locator>>``,
one entry per output column.  A ``Locator`` (in :cfile:`WhereCircuit.h`)
is a triple ``(table, tuple_uuid, column_position)`` -- the address of
exactly one cell in one base table.  The interpretation of each gate
type is straightforward:

- **IN** :math:`(\mathit{table},\mathit{uuid},n)` returns a vector
  of length :math:`n`, with cell :math:`i` set to
  :math:`\{(\mathit{table},\mathit{uuid},i)\}`.
- **TIMES** concatenates the vectors of its children
  (column-wise concatenation).  This is the right operation
  because joins place the columns of the two operands side by
  side.
- **PLUS** takes the union of locator sets, column by column.
  Children must agree on the number of columns.
- **PROJECT** with ``positions = [p_1, ..., p_k]`` returns a
  vector of length :math:`k` whose :math:`i`-th entry is the
  child's :math:`p_i`-th column (or :math:`\emptyset` if
  :math:`p_i = 0`).
- **EQ** with positions :math:`(i, j)` evaluates the child to
  vector :math:`v`, then sets
  :math:`v_i \leftarrow v_i \cup v_j` and
  :math:`v_j \leftarrow v_j \cup v_i`.

The output of ``where_provenance(token)`` is rendered as
``{[loc;loc;...],[loc;...],...}`` -- one bracket-pair per output
column, locators inside separated by semicolons.


Building the Circuit During Query Rewriting
-------------------------------------------

When ``provsql.where_provenance`` is on,
:cfunc:`make_provenance_expression` builds the where-provenance
fragment in three phases on top of the regular semiring expression
(see :doc:`query-rewriting` for the surrounding context):

1. **Column map.**  :cfunc:`build_column_map` walks the range
   table and assigns a global integer position to every output
   column of every base RTE.  The provsql column itself is mapped
   to ``-1``; join RTEs (which redirect to base columns) are
   mapped to ``0``.  The result is a 2-D array ``columns[rte][att]``
   used by the next two phases.

2. **EQ gates from join conditions.**
   :cfunc:`add_eq_from_Quals_to_Expr` walks the ``ON`` clauses of
   the ``FROM`` jointree and the top-level ``WHERE`` clause looking
   for equality predicates of the form ``Var = Var``.  For each one,
   :cfunc:`add_eq_from_OpExpr_to_Expr` looks up both columns in the
   column map and wraps the current provenance expression in a
   ``provenance_eq(expr, col1, col2)`` call -- creating an EQ gate
   that records which two columns were compared.

3. **PROJECT gate from the SELECT list.**  After EQ gates have
   been added, :cfunc:`make_provenance_expression` walks the target
   list one more time, building an integer array of column
   positions that the ``SELECT`` clause keeps and in what order.  If that ordering
   differs from the identity (``[1, 2, 3, ...]`` covering every
   column of every input), the expression is wrapped in a
   ``provenance_project(expr, positions...)`` call.  Output columns
   that are expressions rather than bare ``Var``\ s get position
   ``0`` -- they have no source cell.

The result is a single token that, when evaluated by
``where_provenance``, walks the circuit, applies the gate semantics
above, and returns the per-column locator sets.


Sub-Circuit Materialization
---------------------------

To evaluate a where-provenance token, the C function
:cfile:`where_provenance.cpp` doesn't traverse the persistent mmap
store directly; instead it calls the SQL helper
``provsql.sub_circuit_for_where(token)``, which walks the circuit
recursively from the root and returns one row per gate, including
the metadata each gate type needs (table name and column count for
IN, position pair for EQ, projection vector for PROJECT, etc.).  The
C function then rebuilds a :cfunc:`WhereCircuit` from those rows and
calls ``evaluate``.

The reason for this round-trip is that where-provenance gates carry
*structured* annotations (column lists, position pairs, table OIDs)
that the generic gate inspection functions used by the semiring
evaluator cannot return in one shot.  The SQL helper joins all the
relevant catalogs and returns everything the C reconstructor needs.


Why-Provenance vs Where-Provenance
----------------------------------

It is worth being explicit about how the two flavours of provenance
differ at the level of the data structures the developer guide
describes:

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * -
     - Why-provenance (semirings)
     - Where-provenance
   * - Granularity
     - Per output *tuple*
     - Per output *cell*
   * - Output type
     - A semiring value (Boolean, integer, formula...)
     - ``vector<set<Locator>>``
   * - In-memory class
     - :cfunc:`GenericCircuit` / :cfunc:`BooleanCircuit`
     - :cfunc:`WhereCircuit`
   * - Gate types used
     - ``input``, ``plus``, ``times``, ``monus``, ``delta``,
       ``agg``, ``semimod``, ``value``, ``cmp``
     - ``input``, ``plus``, ``times``, ``project``, ``eq``
   * - Extra gates emitted
     - Always (when ProvSQL is active)
     - Only when ``provsql.where_provenance`` is on
   * - Read by
     - :sqlfunc:`provenance_evaluate`,
       :sqlfunc:`probability_evaluate`, ``sr_*``...
     - :sqlfunc:`where_provenance`

Where-provenance does not interact with aggregation, ``HAVING``,
``EXCEPT``, or set operations beyond ``UNION ALL`` -- features
covered by the semiring side -- because cell-level lineage has no
agreed semantics under those operators.  Enabling
``provsql.where_provenance`` does not turn off the semiring side;
the same persistent circuit then contains both the regular
semiring gates and the extra ``project`` / ``eq`` gates, and each
gate type is interpreted by the evaluator that cares about it.
