#ifndef BOOLEAN_H
#define BOOLEAN_H

#include <algorithm>
#include <vector>

#include "Semiring.h"

namespace semiring {
class Boolean : public semiring::Semiring<bool>
{
public:
virtual value_type zero() const override {
  return false;
}
virtual value_type one() const override {
  return true;
}
virtual value_type plus(const std::vector<value_type> &v) const override {
  return std::any_of(v.begin(), v.end(), [](bool x) {
        return x;
      });
}
virtual value_type times(const std::vector<value_type> &v) const override {
  return std::all_of(v.begin(), v.end(), [](bool x) {
        return x;
      });
}
virtual value_type monus(value_type x, value_type y) const override
{
  return x & !y;
}
virtual value_type delta(value_type x) const override
{
  return x;
}
virtual bool absorptive() const override {
  return true;
}
};
}

#endif /* BOOLEAN_H */
