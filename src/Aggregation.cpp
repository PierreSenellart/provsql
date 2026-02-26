#include "Aggregation.h"

#include <string>
#include <stdexcept>

extern "C" {
#include "utils/lsyscache.h"
}

AggregationOperator getAggregationOperator(Oid oid)
{
  char *fname = get_func_name(oid);

  if(fname == nullptr)
    elog(ERROR, "Invalid OID for aggregation function: %d", oid);

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
    elog(ERROR, "Aggregation operator %s not supported", func_name.c_str());
  }

  return op;
}

struct NoneAgg : Aggregator {
  void add(const Value& x) override {
  }
  Value finalize() const override {
    return Value{};
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

template <class T>
struct StandardAgg : Aggregator {
protected:
  T value{};
  bool has = false;

public:
  Value finalize() const override {
    if (has) return Value {value}; else return Value{};
  }
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

template <class T>
struct SumAgg : StandardAgg<T> {
  using StandardAgg<T>::value;
  using StandardAgg<T>::has;

  void add(const Value& x) override {
    if (x.getType() == ValueType::NONE) return;
    const T& v = std::get<T>(x.v);
    value += v;
    has = true;
  }
  AggregationOperator op() const override {
    return AggregationOperator::SUM;
  }
};

template <class T>
struct MinAgg : StandardAgg<T> {
  using StandardAgg<T>::value;
  using StandardAgg<T>::has;

  void add(const Value& x) override {
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

template <class T>
struct MaxAgg : StandardAgg<T> {
  using StandardAgg<T>::value;
  using StandardAgg<T>::has;

  void add(const Value& x) override {
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

struct AndAgg : StandardAgg<bool> {
  AndAgg() {
    value=true;
  }

  void add(const Value& x) override {
    if (x.getType() == ValueType::NONE) return;
    auto b = std::get<bool>(x.v);
    value = value && b;
    has = true;
  }
  AggregationOperator op() const override {
    return AggregationOperator::AND;
  }
};

struct OrAgg : StandardAgg<bool> {
  OrAgg() {
    value=false;
  }

  void add(const Value& x) override {
    if (x.getType() == ValueType::NONE) return;
    auto b = std::get<bool>(x.v);
    value = value || b;
    has = true;
  }
  AggregationOperator op() const override {
    return AggregationOperator::OR;
  }
};

template <class T>
struct ChooseAgg : StandardAgg<T> {
  using StandardAgg<T>::value;
  using StandardAgg<T>::has;

  void add(const Value& x) override {
    if (x.getType() == ValueType::NONE) return;
    if(!has)
      value = std::get<T>(x.v);
    has = true;
  }
  AggregationOperator op() const override {
    return AggregationOperator::CHOOSE;
  }
};

template <class T>
struct AvgAgg : Aggregator {
protected:
  double sum = 0;
  unsigned count = 0;
  bool has = false;

public:
  void add(const Value& x) override {
    if (x.getType() == ValueType::NONE) return;
    const T& v = std::get<T>(x.v);
    sum += v;
    ++count;
    has = true;
  }
  AggregationOperator op() const override {
    return AggregationOperator::AVG;
  }
  Value finalize() const override {
    if (has) return Value {sum/count}; else return Value{};
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

template<class T>
struct ArrayAgg : StandardAgg<T> {
protected:
  std::vector<T> values;
  using StandardAgg<T>::has;

public:
  void add(const Value& x) override {
    if (x.getType() == ValueType::NONE) return;
    const T& v = std::get<T>(x.v);
    values.push_back(v);
    has = true;
  }
  Value finalize() const override {
    if (has) return Value {values}; else return Value{};
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
