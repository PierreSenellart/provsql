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
 * Each aggregation function × value-type combination has its own
 * @c Aggregator subclass defined locally in this file (e.g.
 * @c SumAggregator<long>, @c MinAggregator<double>, @c ArrayAggregator<std::string>,
 * etc.).
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
  } else if(func_name == "and" || func_name=="every") {
    op = AggregationOperator::AND;
  } else if(func_name == "or") {
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

/** @brief Aggregator that ignores all inputs and always returns NULL. */
struct NoneAgg : Aggregator {
  void add(const AggValue& x) override {
  }
  AggValue finalize() const override {
    return AggValue{};
  }
  AggregationOperator op() const override {
    return AggregationOperator::NONE;
  }
  ValueType inputType() const override {
    return ValueType::NONE;
  }
};

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
  AggregationOperator op() const override {
    return AggregationOperator::SUM;
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
  AggregationOperator op() const override {
    return AggregationOperator::MIN;
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
  AggregationOperator op() const override {
    return AggregationOperator::MAX;
  }
};

/** @brief Aggregator implementing boolean AND (returns false if any input is false). */
struct AndAgg : StandardAgg<bool> {
  AndAgg() {
    value=true;
  }

  void add(const AggValue& x) override {
    if (x.getType() == ValueType::NONE) return;
    auto b = std::get<bool>(x.v);
    value = value && b;
    has = true;
  }
  AggregationOperator op() const override {
    return AggregationOperator::AND;
  }
};

/** @brief Aggregator implementing boolean OR (returns true if any input is true). */
struct OrAgg : StandardAgg<bool> {
  OrAgg() {
    value=false;
  }

  void add(const AggValue& x) override {
    if (x.getType() == ValueType::NONE) return;
    auto b = std::get<bool>(x.v);
    value = value || b;
    has = true;
  }
  AggregationOperator op() const override {
    return AggregationOperator::OR;
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
  AggregationOperator op() const override {
    return AggregationOperator::CHOOSE;
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
  AggregationOperator op() const override {
    return AggregationOperator::AVG;
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

/** @brief Aggregator implementing ARRAY_AGG; collects all non-NULL inputs into an array. */
template<class T>
struct ArrayAgg : StandardAgg<T> {
protected:
  std::vector<T> values; ///< Accumulated elements
  using StandardAgg<T>::has;

public:
  void add(const AggValue& x) override {
    if (x.getType() == ValueType::NONE) return;
    const T& v = std::get<T>(x.v);
    values.push_back(v);
    has = true;
  }
  AggValue finalize() const override {
    if (has) return AggValue {values}; else return AggValue{};
  }
  AggregationOperator op() const override {
    return AggregationOperator::ARRAY_AGG;
  }
  ValueType resultType() const override {
    if constexpr (std::is_same_v<T,long>)
      return ValueType::ARRAY_INT;
    else if constexpr (std::is_same_v<T,double>)
      return ValueType::ARRAY_FLOAT;
    else if constexpr (std::is_same_v<T,bool>)
      return ValueType::ARRAY_BOOLEAN;
    else if constexpr (std::is_same_v<T,std::string>)
      return ValueType::ARRAY_STRING;
    else
      static_assert(False<T>{});
  }
};

std::unique_ptr<Aggregator> makeAggregator(AggregationOperator op, ValueType t) {
  if(t==ValueType::NONE) return std::make_unique<NoneAgg>();

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
  case AggregationOperator::AND:
    if (t == ValueType::BOOLEAN) return std::make_unique<AndAgg>();
    throw std::runtime_error("AND requires BOOLEAN");
  case AggregationOperator::OR:
    if (t == ValueType::BOOLEAN) return std::make_unique<OrAgg>();
    throw std::runtime_error("OR requires BOOLEAN");
  case AggregationOperator::CHOOSE:
    switch(t) {
    case ValueType::BOOLEAN: return std::make_unique<ChooseAgg<bool> >();
    case ValueType::INT: return std::make_unique<ChooseAgg<long> >();
    case ValueType::FLOAT: return std::make_unique<ChooseAgg<double> >();
    case ValueType::STRING: return std::make_unique<ChooseAgg<std::string> >();
    default: throw std::runtime_error("CHOOSE not supported for this type");
    }
  case AggregationOperator::ARRAY_AGG:
    switch(t) {
    case ValueType::BOOLEAN: return std::make_unique<ArrayAgg<bool> >();
    case ValueType::INT: return std::make_unique<ArrayAgg<long> >();
    case ValueType::FLOAT: return std::make_unique<ArrayAgg<double> >();
    case ValueType::STRING: return std::make_unique<ArrayAgg<std::string> >();
    default: throw std::runtime_error("ARRAY_AGG not supported for this type");
    }
  case AggregationOperator::NONE:
    return std::make_unique<NoneAgg>();
  }

  throw std::logic_error("Unhandled AggregationOperator");
}
