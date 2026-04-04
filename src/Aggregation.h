/**
 * @file Aggregation.h
 * @brief Typed aggregation value, operator, and aggregator abstractions.
 *
 * This header provides the type system used by ProvSQL's aggregate
 * provenance evaluation:
 *
 * - @c ComparisonOperator: the six standard SQL comparison operators,
 *   used by @c gate_cmp gates in the circuit.
 * - @c AggregationOperator: the SQL aggregation functions that ProvSQL
 *   tracks provenance for (COUNT, SUM, MIN, MAX, AVG, AND, OR, …).
 * - @c ValueType: the runtime type tag for aggregate values.
 * - @c AggValue: a tagged union holding one aggregate value of any
 *   supported type, built on @c std::variant.
 * - @c Aggregator: an abstract interface for stateful incremental
 *   accumulators, one per aggregation function/type combination.
 *
 * The free functions @c getAggregationOperator() and @c makeAggregator()
 * map PostgreSQL OIDs and operator/type pairs to the corresponding C++
 * objects.
 */
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

/**
 * @brief SQL comparison operators used in @c gate_cmp circuit gates.
 */
enum class ComparisonOperator {
  EQ, ///< Equal (=)
  NE, ///< Not equal (<>)
  LE, ///< Less than or equal (<=)
  LT, ///< Less than (<)
  GE, ///< Greater than or equal (>=)
  GT  ///< Greater than (>)
};

/**
 * @brief SQL aggregation functions tracked by ProvSQL.
 */
enum class AggregationOperator {
  COUNT,     ///< COUNT(*) or COUNT(expr) → integer
  SUM,       ///< SUM → integer or float
  MIN,       ///< MIN → input type
  MAX,       ///< MAX → input type
  AVG,       ///< AVG → float
  AND,       ///< Boolean AND aggregate
  OR,        ///< Boolean OR aggregate
  CHOOSE,    ///< Arbitrary selection (pick one element)
  ARRAY_AGG, ///< Array aggregation
  NONE,      ///< No aggregation (returns NULL)
};

/**
 * @brief Runtime type tag for aggregate values.
 */
enum class ValueType {
  INT,          ///< Signed 64-bit integer
  FLOAT,        ///< Double-precision float
  BOOLEAN,      ///< Boolean
  STRING,       ///< Text string
  ARRAY_INT,    ///< Array of integers
  ARRAY_FLOAT,  ///< Array of floats
  ARRAY_BOOLEAN,///< Array of booleans
  ARRAY_STRING, ///< Array of strings
  NONE          ///< No value (NULL)
};

/**
 * @brief A dynamically-typed aggregate value.
 *
 * Wraps a @c std::variant of all supported scalar and array types.
 * The active alternative is identified by the @c ValueType tag returned
 * by @c getType().
 */
struct AggValue {
private:
  ValueType t; ///< Active type tag

public:
  /** @brief The variant holding the actual value. */
  std::variant<long, double, bool, std::string,
               std::vector<long>, std::vector<double>, std::vector<bool>, std::vector<std::string> > v;

  /** @brief Construct a NULL (NONE) value. */
  AggValue() : t(ValueType::NONE) {
  }
  /** @brief Construct an integer value. @param l Integer value. */
  AggValue(long l) : t(ValueType::INT), v(l) {
  }
  /** @brief Construct a float value. @param d Float value. */
  AggValue(double d) : t(ValueType::FLOAT), v(d) {
  }
  /** @brief Construct a boolean value. @param b Boolean value. */
  AggValue(bool b) : t(ValueType::BOOLEAN), v(b) {
  }
  /** @brief Construct a string value. @param s String value. */
  AggValue(std::string s) : t(ValueType::STRING), v(s) {
  }
  /** @brief Construct an integer-array value. @param vec Integer array. */
  AggValue(std::vector<long> vec) : t(ValueType::ARRAY_INT), v(vec) {
  }
  /** @brief Construct a float-array value. @param vec Float array. */
  AggValue(std::vector<double> vec) : t(ValueType::ARRAY_FLOAT), v(vec) {
  }
  /** @brief Construct a boolean-array value. @param vec Boolean array. */
  AggValue(std::vector<bool> vec) : t(ValueType::ARRAY_BOOLEAN), v(vec) {
  }
  /** @brief Construct a string-array value. @param vec String array. */
  AggValue(std::vector<std::string> vec) : t(ValueType::ARRAY_STRING), v(vec) {
  }

  /**
   * @brief Return the runtime type tag of this value.
   * @return The @c ValueType identifying the active alternative.
   */
  ValueType getType() const {
    return t;
  }
};

/**
 * @brief Abstract interface for an incremental aggregate accumulator.
 *
 * Each concrete subclass implements one aggregation function for one
 * input type (e.g., SUM over integers, MAX over floats).  Instances are
 * created by @c makeAggregator().
 */
struct Aggregator {
  virtual ~Aggregator() = default;

  /**
   * @brief Incorporate one input value into the running aggregate.
   * @param x  Input value to add.
   */
  virtual void add(const AggValue& x) = 0;

  /**
   * @brief Return the final aggregate result.
   * @return The accumulated aggregate as an @c AggValue.
   */
  virtual AggValue finalize() const = 0;

  /**
   * @brief Return the aggregation operator this accumulator implements.
   * @return The @c AggregationOperator enum value for this accumulator.
   */
  virtual AggregationOperator op() const = 0;

  /**
   * @brief Return the type of the input values accepted by @c add().
   * @return The @c ValueType of values passed to @c add().
   */
  virtual ValueType inputType() const = 0;

  /**
   * @brief Return the type of the value returned by @c finalize().
   *
   * Defaults to @c inputType(); override when the result type differs
   * (e.g., AVG returns FLOAT regardless of the input type).
   * @return The @c ValueType of the value returned by @c finalize().
   */
  virtual ValueType resultType() const {
    return inputType();
  }
};

/**
 * @brief Map a PostgreSQL aggregate function OID to an @c AggregationOperator.
 *
 * @param oid  OID of the aggregate function (e.g. @c F_COUNT_ANY, @c F_SUM_INT4).
 * @return     The corresponding @c AggregationOperator.
 */
AggregationOperator getAggregationOperator(Oid oid);

/**
 * @brief Create a concrete @c Aggregator for the given operator and value type.
 *
 * @param op  The aggregation function to implement.
 * @param t   The type of input values that will be accumulated.
 * @return    A heap-allocated @c Aggregator, or @c nullptr if the combination
 *            is not supported.
 */
std::unique_ptr<Aggregator> makeAggregator(AggregationOperator op, ValueType t);

#endif /* AGGREGATION_H */
