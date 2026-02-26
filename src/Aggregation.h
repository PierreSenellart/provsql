#ifndef AGGREGATION_H
#define AGGREGATION_H

extern "C" {
#include "postgres.h"
}

#include <variant>
#include <string>
#include <vector>
#include <cassert>
#include <memory>

enum class ComparisonOperator {
  EQ,
  NE,
  LE,
  LT,
  GE,
  GT
};

enum class AggregationOperator {
  // INT
  COUNT,
  // INT OR FLOAT
  SUM,
  MIN,
  MAX,
  AVG,
  // BOOLEAN
  AND,
  OR,
  // ARBITRARY TYPE
  CHOOSE,
  ARRAY_AGG,
  // Returns NULL
  NONE,
};

enum class ValueType {
  INT,
  FLOAT,
  BOOLEAN,
  STRING,
  ARRAY_INT,
  ARRAY_FLOAT,
  ARRAY_BOOLEAN,
  ARRAY_STRING,
  NONE
};

struct Value {
private:
  ValueType t;

public:
  std::variant<long, double, bool, std::string,
               std::vector<long>, std::vector<double>, std::vector<bool>, std::vector<std::string> > v;

  Value() : t(ValueType::NONE) {
  }
  Value(long l) : t(ValueType::INT), v(l) {
  }
  Value(double d) : t(ValueType::FLOAT), v(d) {
  }
  Value(bool b) : t(ValueType::BOOLEAN), v(b) {
  }
  Value(std::string s) : t(ValueType::STRING), v(s) {
  }
  Value(std::vector<long> vec) : t(ValueType::ARRAY_INT), v(vec) {
  }
  Value(std::vector<double> vec) : t(ValueType::ARRAY_FLOAT), v(vec) {
  }
  Value(std::vector<bool> vec) : t(ValueType::ARRAY_BOOLEAN), v(vec) {
  }
  Value(std::vector<std::string> vec) : t(ValueType::ARRAY_STRING), v(vec) {
  }

  ValueType getType() const {
    return t;
  }
};

struct Aggregator {
  virtual ~Aggregator() = default;
  virtual void add(const Value& x) = 0;
  virtual Value finalize() const = 0;
  virtual AggregationOperator op() const = 0;
  virtual ValueType inputType() const = 0;
  virtual ValueType resultType() const {
    return inputType();
  }
};

AggregationOperator getAggregationOperator(Oid oid);
std::unique_ptr<Aggregator> makeAggregator(AggregationOperator op, ValueType t);

#endif /* AGGREGATION_H */
