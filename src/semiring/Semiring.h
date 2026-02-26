#ifndef SEMIRING_H
#define SEMIRING_H

#include <vector>
#include <string>

#include "../Aggregation.h"

namespace semiring {
class SemiringException : public std::exception
{
std::string message;

public:
SemiringException(const std::string &m) : message(m) {
}
virtual char const * what() const noexcept {
  return message.c_str();
}
};

template<typename V>
class Semiring
{
public:
typedef V value_type;

virtual value_type zero() const = 0;
virtual value_type one() const = 0;
virtual value_type plus(const std::vector<value_type> &v) const = 0;
virtual value_type times(const std::vector<value_type> &v) const = 0;
virtual value_type monus(value_type x, value_type y) const = 0;
virtual value_type delta(value_type x) const = 0;
virtual value_type cmp(value_type s1, ComparisonOperator op, value_type s2) const {
  throw SemiringException("This semiring does not support cmp gates.");
}
virtual value_type semimod(value_type x, value_type s) const {
  throw SemiringException("This semiring does not support semimod gates.");
}
virtual value_type agg(AggregationOperator op, const std::vector<value_type> &s) {
  throw SemiringException("This semiring does not support agg gates.");
}
virtual value_type value(const std::string &s) const {
  throw SemiringException("This semiring does not support value gates.");
}

virtual ~Semiring() = default;

virtual bool absorptive() const {
  return false;
}
};


}

#endif /* SEMIRING_H */
