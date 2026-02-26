#ifndef COUNTING_H
#define COUNTING_H

#include <numeric>
#include <vector>
#include <stdexcept>

#include "Semiring.h"

namespace semiring {
class Counting : public semiring::Semiring<unsigned>
{
public:
virtual value_type zero() const override {
  return 0;
}
virtual value_type one() const override {
  return 1;
}
virtual value_type plus(const std::vector<value_type> &v) const override {
  return std::accumulate(v.begin(), v.end(), 0);
}
virtual value_type times(const std::vector<value_type> &v) const override {
  return std::accumulate(v.begin(), v.end(), 1, std::multiplies<value_type>());
}
virtual value_type monus(value_type x, value_type y) const override
{
  return x<=y ? 0 : x-y;
}
virtual value_type delta(value_type x) const override
{
  return x!=0 ? 1 : 0;
}

};
}

#endif /* COUNTING_H */
