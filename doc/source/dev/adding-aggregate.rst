Adding a New Aggregate
======================

ProvSQL supports aggregation through a dedicated pipeline that runs
from the query rewriter in :cfile:`provsql.c` down to a |cpp|
accumulator in :cfile:`Aggregation.cpp`.  This page explains the
layers involved and gives a step-by-step guide for adding a new
aggregate (e.g., ``STDDEV``).  See the user-facing documentation at
:doc:`../user/aggregation` for semantics.


Architecture
------------

Aggregation flows through three layers.  Two of them are hardcoded
and one is fully generic:

1. **Query rewriting** (:cfile:`provsql.c`) -- generic.
   :cfunc:`replace_aggregations_by_provenance_aggregate` walks the
   ``Query`` tree, finds every ``Aggref`` node, and replaces it with
   a call to ``provsql.provenance_aggregate(fnoid, resulttype,
   agg(...), tokens)``, where ``tokens`` is the array of provenance
   tokens of the rows contributing to the group.  The rewriter does
   not inspect the aggregate's name -- it just passes the function
   OID and result type through.

2. **Gate creation** (``sql/provsql.common.sql``) -- generic.  The
   ``provenance_aggregate`` SQL function creates an ``agg`` gate
   whose ``info1`` / ``info2`` annotations store the aggregate
   function OID and its result type, and whose ``extra`` payload is
   the string form of the computed aggregate value.

3. **Aggregate accumulator** (:cfile:`Aggregation.h` /
   :cfile:`Aggregation.cpp`) -- **hardcoded**.  When a semiring
   evaluator encounters an ``agg`` gate, it calls
   :cfunc:`getAggregationOperator` to map the OID to an
   :cfunc:`AggregationOperator` enum value, then
   :cfunc:`makeAggregator` to instantiate a concrete |cpp|
   :cfunc:`Aggregator` subclass that implements ``add(value)`` and
   ``finalize()``.

The hardcoded part is intentional: the accumulator needs to know the
exact semantics of the operation in |cpp|.  Adding a new aggregate
therefore means teaching :cfile:`Aggregation.cpp` about it.

Currently Supported
^^^^^^^^^^^^^^^^^^^

The :cfunc:`AggregationOperator` enum in :cfile:`Aggregation.h`
lists the operators currently implemented: ``COUNT`` (normalized to
``SUM`` over ``INT``), ``SUM``, ``MIN``, ``MAX``, ``AVG``, ``AND``,
``OR``, ``CHOOSE``, and ``ARRAY_AGG``.


Step-by-Step: Adding a New Aggregate
------------------------------------

As a running example, assume we want to add ``bit_and`` -- the
bitwise-AND of all non-null integer values in a group.

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

   Each PostgreSQL aggregate function with distinct semantics needs
   its own enum value and accumulator; aliases with identical
   semantics (e.g. ``stddev`` / ``stddev_samp``) can share one.

3. **Implement the accumulator.**  Add a templated :cfunc:`Aggregator`
   subclass in :cfile:`Aggregation.cpp` next to the existing ones
   (:cfunc:`SumAgg`, :cfunc:`MinAgg`, :cfunc:`AvgAgg`, etc.).  The
   class implements four virtual methods:

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

5. **Add a regression test.**  Create
   ``test/sql/agg_bit_and.sql`` and its expected output, following
   the pattern of the existing aggregation tests.  Reference it from
   ``test/schedule.common`` (see :doc:`testing` and
   :doc:`build-system` for the schedule conventions).

6. **Update the user guide.**  Mention the new aggregate in
   :doc:`../user/aggregation`, and add it to the list of currently
   supported operators in the "Currently supported" section above.

Nothing else needs to change: the query rewriter, the
``provenance_aggregate`` SQL function, the :cfunc:`agg_token`
composite type, and the :cfunc:`aggregation_evaluate` SQL dispatcher
all operate on OIDs and metadata, so they pick up new aggregates
automatically once steps 1--4 are in place.
