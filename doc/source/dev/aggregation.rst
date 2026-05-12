Aggregation Provenance
======================

Aggregation is the most subtle feature of ProvSQL's query rewriter:
unlike selection or join, an aggregate function fundamentally
*combines* values rather than just propagating them, so the
provenance of an aggregate result cannot be a plain semiring value
of the same type as a tuple's provenance.  This chapter explains the
data model ProvSQL uses to represent aggregate provenance, the gate
types involved, and how the rewriter glues everything together.  The
final section is a step-by-step guide for adding a new aggregate
function in |cpp|.

For the user-facing semantics see :doc:`../user/aggregation`; for
the formal semantics over the extended relational algebra see the
ICDE 2026 paper :cite:`sen2026provsql`.


.. _the-semimodule-picture:

The Semimodule Picture
----------------------

The semiring framework for positive relational algebra (selection,
projection, join, union) annotates *tuples*: each row of a
K-relation carries a single element of a commutative semiring K.
Aggregation does not fit that picture, because the result of
``SUM(salary)`` is a *value*, not a tuple, and the question "where
does this value come from?" has to track both the contributing
rows and the payload each row contributed.  Amsterdamer, Deutch,
and Tannen (:cite:`DBLP:conf/pods/AmsterdamerDT11`) resolved this
by annotating tuples and values *simultaneously* using a
**K-semimodule**.

A K-semimodule is a commutative monoid :math:`(M, +, 0)` together
with a scalar multiplication :math:`\cdot : K \times M \to M` that
distributes over the semiring operations on K and over the monoid
addition on M.  The intuition is that elements of M are
"aggregate values" and elements of K are "provenance
annotations"; scaling ``m`` by ``k`` produces an M-element
"tagged" with the provenance ``k``.

The specific K-semimodule the paper builds for aggregate
provenance is the **tensor product** :math:`K \star M`, whose
elements are finite formal sums
:math:`k_1 \star m_1 + k_2 \star m_2 + \cdots + k_n \star m_n`
of pairs ("simple tensors") :math:`k \star m`, modulo the
equivalences that make :math:`\star` bilinear
(:math:`(k \oplus k') \star m = k \star m + k' \star m`, etc.).
Note how this mixes three different operations: :math:`\oplus`
is the ⊕ of the semiring K, :math:`+` is the monoid addition of
the semimodule :math:`K \star M`, and :math:`\star` is the
tensor product that glues a K-element to an M-element.  A simple
tensor :math:`k \star m` should be read as "the tuple with
provenance ``k`` contributed the value ``m``".  The whole
:math:`K \star M` is itself a K-semimodule, with scalar
multiplication :math:`k' \cdot (k \star m) = (k' \otimes k) \star m`.

Concretely, for a query like

.. code-block:: sql

   SELECT city, SUM(salary) FROM personnel GROUP BY city;

the provenance of the aggregate value in one group is

.. math::

   k_1 \star m_1 \;+\; k_2 \star m_2 \;+\; \cdots
     \;+\; k_n \star m_n

where ``k_i`` is the semiring annotation of the i-th input tuple
in the group and ``m_i`` is its salary payload (lifted into M).
The sum is **not** collapsed into a single number -- it carries,
for every contributing tuple, both its provenance and the value
it contributed, and concrete semirings (Counting, Formula, ...)
are free to specialise the aggregation over :math:`K \star M`
into a meaningful scalar only at evaluation time.

Strictly speaking, the Amsterdamer et al. construction requires
:math:`(M, +, 0)` to be a commutative monoid, which rules out
aggregate operators that are non-associative or non-commutative
(``ARRAY_AGG`` with a user-supplied order, for instance).
ProvSQL therefore reuses the semimodule *framework* -- the
tensor product :math:`K \star M`, the ``semimod`` / ``agg`` /
``value`` gate types, the circuit shape -- but does **not** bake
the monoid axioms into it.  An ``agg`` gate is treated as an
abstract formal sum of simple tensors, and it is up to each
concrete accumulator to decide what "combining" those tensors
means.  Aggregates whose operator genuinely *is* a commutative
monoid (``SUM``, ``COUNT``, ``MIN``, ``MAX``, ``AND``, ``OR``,
...) open the door to optimisations -- reordering, partial
folding during traversal, and so on -- that accumulators for
order-sensitive or non-associative aggregates cannot use.  The
distinction is a property of the accumulator, not of the gate
representation.

ProvSQL realises this construction directly as a circuit built
from three gate types in :cfunc:`gate_type`:

- ``value`` -- a leaf carrying a constant payload :math:`m`
  (stored as a string in the persistent representation; the
  |cpp| accumulator parses it into the appropriate native type).
- ``semimod`` -- a binary gate representing one simple tensor
  :math:`k \star m`.  Its two children are the tuple's
  provenance (a sub-circuit evaluating to :math:`k \in K`) and a
  ``value`` gate carrying :math:`m`.
- ``agg`` -- the aggregate root, the formal sum
  :math:`\sum_i (k_i \star m_i)` taken over all tuples in
  the group.  Its annotations (``info1`` / ``info2`` from
  :sqlfunc:`get_infos`) record the PostgreSQL OID of the
  aggregate function and the OID of its result type, so the
  evaluator can later instantiate the right |cpp| accumulator.

Row-level provenance and the δ operator
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

An ``agg`` gate gives the provenance of the *aggregate value* in
one column of one output row, but the row itself also has a
provenance -- "this row exists in the result" -- that the
downstream evaluators need.  That row-level provenance is the ⊕
of the per-tuple tokens of the group, just like for a plain
``GROUP BY`` without aggregates.  However, summing raw tuple
annotations can over-count existence: a row that exists in
multiplicity :math:`n` in the input would naively have a
row-level provenance of
:math:`1_K \oplus 1_K \oplus \cdots \oplus 1_K` (n times),
whereas what we want is a single "it exists".

The **δ operator** (Amsterdamer, Deutch, Tannen
:cite:`DBLP:conf/pods/AmsterdamerDT11`) solves this.  A
δ-semiring is a semiring together with a unary operation δ
satisfying :math:`\delta(\mathbb{0}) = \mathbb{0}` and
:math:`\delta(\mathbb{1} \oplus \cdots \oplus \mathbb{1}) = \mathbb{1}`
regardless of the number of :math:`\mathbb{1}` s.  Intuitively, δ collapses "any positive number of
witnesses" to a single "exists".  The rewriter emits a ``delta``
gate wrapping the row-level ⊕ whenever the query has aggregation
and no ``HAVING`` clause (see
:cfunc:`make_provenance_expression`).  ``HAVING`` queries do not
add a δ: a ``HAVING`` predicate constrains existence in a way
that depends on the aggregate value, so the row-level token has
to carry more information than a flat "exists", and the rewriter
instead routes the result through ``cmp`` gates that
:cfile:`having_semantics.hpp` evaluates ahead of the main
traversal (see :ref:`semiring-optional-methods`).


The ``agg_token`` Type
----------------------

PostgreSQL evaluates aggregates using its own machinery, which
expects them to return ordinary scalar types (an ``INT``, a
``FLOAT``...).  But the rewritten query needs the aggregate
result to carry *both* the scalar value and the provenance gate
that captures how it was computed -- otherwise downstream
references to ``provenance()`` would have nothing to return.

ProvSQL solves this by introducing :cfunc:`agg_token`, a
composite SQL type wrapping a UUID (the root ``agg`` gate) and a
string form of the aggregate value.  The rewriter wraps every
``Aggref`` in a call to :sqlfunc:`provenance_aggregate` whose
return type is ``agg_token``.  When the executor processes the
result, every aggregate column carries an :cfunc:`agg_token` and
both pieces of information flow downstream:

- :sqlfunc:`provenance` extracts the UUID for the user.
- Cast operators on :cfunc:`agg_token` (defined in
  :cfile:`agg_token.c`) extract the scalar value when the user
  treats the result as a number, e.g. for further arithmetic.

A consequence of this design is that arithmetic on aggregate
results *loses provenance*: once you write
``SUM(x) * 2``, the multiplication operates on the scalar side of
the :cfunc:`agg_token` and the result is a plain ``FLOAT``
without any associated gate.  ProvSQL emits a warning in this
case (controlled by :cfunc:`insert_agg_token_casts` in the
rewriter) but the loss is unavoidable: the result is no longer a
direct combination of base tuples and we cannot honestly attach
a circuit to it.  Comparisons of aggregate results, on the other
hand, are routed through ``cmp`` gates so that ``HAVING``
predicates *do* preserve provenance -- see :doc:`semiring-evaluation`
for the gory details of how that pre-evaluation works.


What the Rewriter Builds
------------------------

The generic rewriting pipeline in :doc:`query-rewriting` covers
aggregation at the *pipeline* level: Step 4 calls
:cfunc:`rewrite_agg_distinct` to lift ``COUNT(DISTINCT ...)``
into an inner ``GROUP BY``, Step 8 calls
:cfunc:`replace_aggregations_by_provenance_aggregate` followed
by :cfunc:`migrate_probabilistic_quals` and
:cfunc:`insert_agg_token_casts`, and Step 9 fuses the row-level
tokens with ``provenance_plus(array_agg(...))`` and wraps the
result in ``provenance_delta``.  This section only documents the
aggregation-specific *structures* those passes produce -- the
pieces that the generic pipeline description is too terse to spell
out.

The call that :cfunc:`make_aggregation_expression` synthesises to
replace an ``Aggref`` is a ``FuncExpr`` for
``provsql.provenance_aggregate`` whose arguments are:

- the OID of the aggregate function;
- the OID of its result type;
- the original ``Aggref`` itself, so PostgreSQL still computes
  the scalar value (this is what ends up inside the
  :cfunc:`agg_token`);
- an ``ARRAY[...]`` of per-tuple ``provenance_semimod(arg, t)``
  calls -- one ``semimod`` gate per row of the input, glueing
  the row's provenance ``t`` to the row's contributed value
  ``arg``.

That fourth argument is where the semimodule construction of
:ref:`the-semimodule-picture` is actually assembled: every
``semimod`` gate is one simple tensor :math:`k \star m`, and the
``agg`` gate at the root of the ``provenance_aggregate`` call is
their formal sum :math:`\sum_i (k_i \star m_i)`.

The row-level side of the rewrite is much simpler.  It reuses the
ordinary ``get_provenance_attributes`` collection, combines the
per-row tokens with ``provenance_plus(array_agg(...))``, and -- for
queries with aggregation and no ``HAVING`` -- wraps the result in
``provenance_delta``.  The row-level token therefore has *no*
semimodule content: it only records which input rows witness the
output row's existence.

The end result is that each row of the aggregation output
carries:

- per-aggregate-column ``agg_token`` values whose UUID points to
  an ``agg`` gate combining ``semimod`` per-tuple contributions;
- a row-level ``provsql`` token whose root is a ``delta`` gate
  over the ⊕ of the group's row tokens.

These two pieces are independent: an evaluator that asks for the
provenance of a *value* in the result reaches an ``agg`` gate; an
evaluator that asks for the provenance of the *row* itself reaches
a ``delta`` gate.


Currently Supported Aggregates
------------------------------

The :cfunc:`AggregationOperator` enum in :cfile:`Aggregation.h`
lists the operators currently implemented in |cpp|: ``COUNT``
(normalised to ``SUM`` over ``INT``), ``SUM``, ``MIN``, ``MAX``,
``AVG``, ``AND``, ``OR``, ``CHOOSE``, and ``ARRAY_AGG``.  Adding
to this list is the topic of the next section.


Random-Variable Aggregates
--------------------------

When the aggregated column has type ``random_variable``
(see :doc:`continuous-distributions`), the rewriter routes
through a separate path: instead of producing a
``provenance_aggregate`` call that wraps the original
``Aggref`` in an :cfunc:`agg_token`, it produces an aggregate
that *returns* a ``random_variable`` root. The aggregate's
result is the lifted scalar :math:`\sum_i \mathbf{1}\{\varphi_i\}
\cdot X_i` (or its product / average analogue), realised as a
single ``gate_arith`` over per-row ``gate_mixture``
children.

The dispatch in :cfunc:`make_aggregation_expression` keys on
``aggtype`` (the aggregate's *result type* OID): when
``aggtype = OID_TYPE_RANDOM_VARIABLE``, the per-row argument
``X_i`` is wrapped in ``rv_aggregate_semimod``
(a :sqlfunc:`mixture` over the row's provenance gate and
the identity for the aggregate) *before* it reaches the SFUNC.
The aggregate's SFUNC accumulates the wrapped per-row UUIDs;
the FFUNC builds the final ``gate_arith`` root. The three
RV-returning aggregates currently shipped –
:sqlfunc:`sum`, :sqlfunc:`avg`,
:sqlfunc:`product` – share an
``INITCOND = '{}'`` so the FFUNC runs even on an empty group,
with per-aggregate empty-group identities.

This is the *semimodule-of-mixtures* shape: rather than minting a
new M-polymorphic ``gate_agg`` that would require parallel
evaluation paths in every analytical evaluator, the rewrite
composes through the existing ``gate_arith`` /
``gate_mixture`` rules. See
:doc:`continuous-distributions` for the FFUNC details.

Step-by-Step: Adding a New Aggregate
------------------------------------

As a running example, assume we want to add ``bit_and`` -- the
bitwise-AND of all non-null integer values in a group.  The
mechanics are independent of which aggregate you are adding; the
only design point is the |cpp| accumulator that knows how to
combine the values incrementally.

1. **Add an enum value.**  In :cfile:`Aggregation.h`, extend
   :cfunc:`AggregationOperator`:

   .. code-block:: cpp

      enum class AggregationOperator {
        COUNT, SUM, MIN, MAX, AVG,
        AND, OR,
        CHOOSE, ARRAY_AGG,
        BIT_AND,     // new
        NONE,
      };

2. **Map the PostgreSQL function name to the enum.**  In
   :cfile:`Aggregation.cpp`, extend :cfunc:`getAggregationOperator`
   with a case matching the PostgreSQL function name returned by
   ``get_func_name()``:

   .. code-block:: cpp

      } else if(func_name == "bit_and") {
        op = AggregationOperator::BIT_AND;

   Each PostgreSQL aggregate function with distinct semantics
   needs its own enum value and accumulator; aliases with
   identical semantics (e.g. ``stddev`` / ``stddev_samp``) can
   share one.

3. **Implement the accumulator.**  Add a templated
   :cfunc:`Aggregator` subclass in :cfile:`Aggregation.cpp` next
   to the existing ones (:cfunc:`SumAgg`, :cfunc:`MinAgg`,
   :cfunc:`AvgAgg`, etc.).  The class implements four virtual
   methods:

   .. code-block:: cpp

      template <typename T>
      struct BitAndAgg : Aggregator {
        T acc = ~T{0};  // all-ones identity
        bool has = false;
        void add(const AggValue& x) override {
          if (x.getType() == ValueType::NONE) return;
          acc &= std::get<T>(x.v);
          has = true;
        }
        AggValue finalize() const override {
          return has ? AggValue{acc} : AggValue{};
        }
        AggregationOperator op() const override {
          return AggregationOperator::BIT_AND;
        }
        ValueType inputType() const override { return ValueType::INT; }
        ValueType resultType() const override { return ValueType::INT; }
      };

4. **Instantiate the accumulator.**  Extend :cfunc:`makeAggregator`
   in :cfile:`Aggregation.cpp` with a case that creates the right
   template instantiation for each supported input type:

   .. code-block:: cpp

      case AggregationOperator::BIT_AND:
        switch (t) {
          case ValueType::INT: return std::make_unique<BitAndAgg<long>>();
          default:
            throw std::runtime_error("BIT_AND not supported for this type");
        }

5. **Add a regression test.**  Create ``test/sql/agg_bit_and.sql``
   and its expected output, following the pattern of the existing
   aggregation tests.  Reference it from ``test/schedule.common``
   (see :doc:`testing` and :doc:`build-system` for the schedule
   conventions).

6. **Update the user guide.**  Mention the new aggregate in
   :doc:`../user/aggregation`, and add it to the list of currently
   supported operators in the "Currently Supported Aggregates"
   section above.

Nothing else needs to change: the query rewriter, the
``provenance_aggregate`` SQL function, the :cfunc:`agg_token`
composite type, and the :cfunc:`aggregation_evaluate` SQL
dispatcher all operate on OIDs and metadata, so they pick up new
aggregates automatically once steps 1--4 are in place.
