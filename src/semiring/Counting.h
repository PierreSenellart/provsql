#ifndef COUNTING_H
#define COUNTING_H

#include <numeric>
#include <vector>

#include "Semiring.h"

namespace semiring {
class Counting : public semiring::Semiring<unsigned>
{
public:
virtual value_type zero() const {
  return 0;
}
virtual value_type one() const {
  return 1;
}
virtual value_type plus(const std::vector<value_type> &v) const {
  return std::accumulate(v.begin(), v.end(), 0);
}
virtual value_type times(const std::vector<value_type> &v) const {
  return std::accumulate(v.begin(), v.end(), 1, std::multiplies<value_type>());
}
virtual value_type monus(value_type x, value_type y) const
{
  return x<=y ? 0 : x-y;
}
virtual value_type delta(value_type x) const
{
  return x!=0 ? 1 : 0;
}

virtual value_type semimod(value_type x, value_type y) const override
{
  return x * y;
}

virtual value_type cmp(value_type x, ComparisonOperator op, value_type y) const override {
  bool b;
  switch(op) {
  case ComparisonOperator::EQUAL:      b = (x == y); break;
  case ComparisonOperator::NE:         b = (x != y); break;
  case ComparisonOperator::LT:         b = (x <  y); break;
  case ComparisonOperator::LE:         b = (x <= y); break;
  case ComparisonOperator::GT:         b = (x >  y); break;
  case ComparisonOperator::GE:         b = (x >= y); break;
  default:                             b = false;     break;
  }
  return b ? 1 : 0;
}

virtual value_type agg(AggregationOperator op, const std::vector<value_type> &v) override {
  if (v.empty()) {
    switch (op) {
    case AggregationOperator::SUM:  return zero();
    case AggregationOperator::PROD: return one();
    case AggregationOperator::MIN:  return std::numeric_limits<value_type>::max();
    case AggregationOperator::MAX:  return std::numeric_limits<value_type>::min();
    case AggregationOperator::CHOOSE: return zero();
    }
  }
  switch (op) {
  case AggregationOperator::SUM:
    return plus(v);
  case AggregationOperator::PROD:
    return times(v);
  case AggregationOperator::MIN:
    return *std::min_element(v.begin(), v.end());
  case AggregationOperator::MAX:
    return *std::max_element(v.begin(), v.end());
  case AggregationOperator::CHOOSE:
    return v[0];
  }
  return zero();
}


virtual value_type value(const std::string &s) const override {
  return static_cast<value_type>(std::stoul(s));
}

};
}

#endif /* COUNTING_H */
