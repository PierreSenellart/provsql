/**
 * @file Aggregation.cpp
 * @brief Aggregation operator and accumulator implementations.
 *
 * Implements the two factory functions declared in @c Aggregation.h:
 * - @c getAggregationOperator(): maps a PostgreSQL aggregate function OID
 *   (looked up by name via @c get_func_name()) to an @c AggregationOperator
 *   enum value.
 * - @c makeAggregator(): constructs a concrete @c Aggregator subclass
 *   for the given operator/type combination.
 *
 * Each built aggregation function × value-type combination has its own
 * @c Aggregator subclass defined locally in this file (e.g. @c SumAgg<long>,
 * @c MinAgg<double>, @c ChooseAgg<long>).  Only the aggregates the Monte-Carlo
 * sampler and the subset enumerator evaluate directly are built: the numeric
 * ones (SUM / COUNT / MIN / MAX / AVG) and CHOOSE (the categorical analog,
 * decided by the exhaustive subset enumerator).  The boolean (bool_or /
 * bool_and) and array_agg aggregates are resolved to a Boolean subcircuit by
 * the m-semiring HAVING rewrite (@c having_semantics) and so never reach
 * @c makeAggregator.
 */
#include "Aggregation.h"

#include <string>
#include <stdexcept>

extern "C" {
#include "utils/lsyscache.h"
#include "utils/elog.h"
}

#include "provsql_error.h"

AggregationOperator getAggregationOperator(Oid oid)
{
  char *fname = get_func_name(oid);

  if(fname == nullptr)
    provsql_error("Invalid OID for aggregation function: %d", oid);

  std::string func_name {fname};
  pfree(fname);

  AggregationOperator op;

  if(func_name == "count") {
    op = AggregationOperator::COUNT;
  } else if(func_name == "sum") {
    op = AggregationOperator::SUM;
  } else if(func_name == "min") {
    op = AggregationOperator::MIN;
  } else if(func_name == "max") {
    op = AggregationOperator::MAX;
  } else if(func_name == "choose") {
    op = AggregationOperator::CHOOSE;
  } else if(func_name == "avg") {
    op = AggregationOperator::AVG;
  } else if(func_name == "array_agg") {
    op = AggregationOperator::ARRAY_AGG;
  } else if(func_name == "bool_and" || func_name == "every") {
    op = AggregationOperator::AND;
  } else if(func_name == "bool_or") {
    op = AggregationOperator::OR;
  } else {
    provsql_error("Aggregation operator %s not supported", func_name.c_str());
  }

  return op;
}

ComparisonOperator cmpOpFromOid(Oid op_oid, bool &ok)
{
  ok = false;
  char *opname = get_opname(op_oid);
  if(opname == nullptr)
    return ComparisonOperator::EQ;

  std::string s {opname};
  pfree(opname);

  ok = true;
  if(s == "=")  return ComparisonOperator::EQ;
  if(s == "<>") return ComparisonOperator::NE;
  if(s == "<")  return ComparisonOperator::LT;
  if(s == "<=") return ComparisonOperator::LE;
  if(s == ">")  return ComparisonOperator::GT;
  if(s == ">=") return ComparisonOperator::GE;

  ok = false;
  return ComparisonOperator::EQ;
}

template <class ...>
struct False : std::bool_constant<false> { };

/**
 * @brief Base aggregator template for scalar types (int, float, bool, string).
 *
 * @tparam T  The C++ type of the accumulated value.
 */
template <class T>
struct StandardAgg : Aggregator {
protected:
  T value{};   ///< Current accumulated value
  bool has = false; ///< @c true once the first non-NULL input has been seen

public:
  /** @brief Return the accumulated value, or NULL if no inputs were seen. */
  AggValue finalize() const override {
    if (has) return AggValue {value}; else return AggValue{};
  }
  /** @brief Return the value type corresponding to @c T. */
  ValueType inputType() const override {
    if constexpr (std::is_same_v<T,long>)
      return ValueType::INT;
    else if constexpr (std::is_same_v<T,double>)
      return ValueType::FLOAT;
    else if constexpr (std::is_same_v<T,bool>)
      return ValueType::BOOLEAN;
    else if constexpr (std::is_same_v<T,std::string>)
      return ValueType::STRING;
    else
      static_assert(False<T>{});
  }
};

/** @brief Aggregator implementing SUM for integer or float types. */
template <class T>
struct SumAgg : StandardAgg<T> {
  using StandardAgg<T>::value;
  using StandardAgg<T>::has;

  void add(const AggValue& x) override {
    if (x.getType() == ValueType::NONE) return;
    const T& v = std::get<T>(x.v);
    value += v;
    has = true;
  }
};

/** @brief Aggregator implementing MIN for integer or float types. */
template <class T>
struct MinAgg : StandardAgg<T> {
  using StandardAgg<T>::value;
  using StandardAgg<T>::has;

  void add(const AggValue& x) override {
    if (x.getType() == ValueType::NONE) return;
    const T& v = std::get<T>(x.v);
    if(has) {
      if(v < value) value = v;
    } else {
      value = v;
      has = true;
    }
  }
};

/** @brief Aggregator implementing MAX for integer or float types. */
template <class T>
struct MaxAgg : StandardAgg<T> {
  using StandardAgg<T>::value;
  using StandardAgg<T>::has;

  void add(const AggValue& x) override {
    if (x.getType() == ValueType::NONE) return;
    const T& v = std::get<T>(x.v);
    if(has) {
      if(v > value) value = v;
    } else {
      value = v;
      has = true;
    }
  }
};

/** @brief Aggregator implementing CHOOSE (returns the first non-NULL input). */
template <class T>
struct ChooseAgg : StandardAgg<T> {
  using StandardAgg<T>::value;
  using StandardAgg<T>::has;

  void add(const AggValue& x) override {
    if (x.getType() == ValueType::NONE) return;
    if(!has)
      value = std::get<T>(x.v);
    has = true;
  }
};

/** @brief Aggregator implementing AVG; always returns a float result. */
template <class T>
struct AvgAgg : Aggregator {
protected:
  double sum = 0;     ///< Running sum of all non-NULL input values
  unsigned count = 0; ///< Number of non-NULL inputs seen so far
  bool has = false;   ///< @c true once the first non-NULL input has been seen

public:
  void add(const AggValue& x) override {
    if (x.getType() == ValueType::NONE) return;
    const T& v = std::get<T>(x.v);
    sum += v;
    ++count;
    has = true;
  }
  AggValue finalize() const override {
    if (has) return AggValue {sum/count}; else return AggValue{};
  }
  ValueType inputType() const override {
    if constexpr (std::is_same_v<T,long>)
      return ValueType::INT;
    else if constexpr (std::is_same_v<T,double>)
      return ValueType::FLOAT;
    else
      static_assert(False<T>{});
  }
  ValueType resultType() const override {
    return ValueType::FLOAT;
  }
};

// Constructs the deterministic accumulator the Monte-Carlo sampler and the
// exhaustive subset enumerator push per-world values into.  The numeric
// aggregates (SUM / COUNT / MIN / MAX / AVG) and CHOOSE are built; the boolean
// (bool_or / bool_and) and array_agg aggregates never reach this factory: the
// m-semiring HAVING rewrite in having_semantics resolves them to a Boolean
// subcircuit before probability evaluation, so no such gate_agg survives to the
// sampler.  They are rejected explicitly rather than handled.
std::unique_ptr<Aggregator> makeAggregator(AggregationOperator op, ValueType t) {
  switch (op) {
  case AggregationOperator::COUNT:
    if (t == ValueType::INT) return std::make_unique<SumAgg<long> >();
    throw std::runtime_error("COUNT is normalized to SUM(INT)");
  case AggregationOperator::SUM:
    switch (t) {
    case ValueType::INT:  return std::make_unique<SumAgg<long> >();
    case ValueType::FLOAT: return std::make_unique<SumAgg<double> >();
    default: throw std::runtime_error("SUM not supported for this type");
    }
  case AggregationOperator::MIN:
    switch (t) {
    case ValueType::INT:  return std::make_unique<MinAgg<long> >();
    case ValueType::FLOAT: return std::make_unique<MinAgg<double> >();
    default: throw std::runtime_error("MIN not supported for this type");
    }
  case AggregationOperator::MAX:
    switch (t) {
    case ValueType::INT:  return std::make_unique<MaxAgg<long> >();
    case ValueType::FLOAT: return std::make_unique<MaxAgg<double> >();
    default: throw std::runtime_error("MAX not supported for this type");
    }
  case AggregationOperator::AVG:
    switch (t) {
    case ValueType::INT:  return std::make_unique<AvgAgg<long> >();
    case ValueType::FLOAT: return std::make_unique<AvgAgg<double> >();
    default: throw std::runtime_error("AVG not supported for this type");
    }
  case AggregationOperator::CHOOSE:
    switch(t) {
    case ValueType::BOOLEAN: return std::make_unique<ChooseAgg<bool> >();
    case ValueType::INT: return std::make_unique<ChooseAgg<long> >();
    case ValueType::FLOAT: return std::make_unique<ChooseAgg<double> >();
    case ValueType::STRING: return std::make_unique<ChooseAgg<std::string> >();
    default: throw std::runtime_error("CHOOSE not supported for this type");
    }
  case AggregationOperator::AND:
  case AggregationOperator::OR:
  case AggregationOperator::ARRAY_AGG:
  case AggregationOperator::NONE:
    // Resolved to a Boolean subcircuit by the HAVING rewrite; never sampled.
    throw std::runtime_error(
            "makeAggregator: boolean/array_agg aggregates are handled by the "
            "m-semiring HAVING rewrite, not the deterministic sampler");
  }

  throw std::logic_error("Unhandled AggregationOperator");
}
