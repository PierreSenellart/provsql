#ifndef BOOLEAN_H
#define BOOLEAN_H

#include <algorithm>
#include <vector>

#include "Semiring.h"

namespace semiring {
class Boolean : public semiring::Semiring<bool>
{
public:
virtual value_type zero() const {
  return false;
}
virtual value_type one() const {
  return true;
}
virtual value_type plus(const std::vector<value_type> &v) const {
  return std::any_of(v.begin(), v.end(), [](bool x) {
        return x;
      });
}
virtual value_type times(const std::vector<value_type> &v) const {
  return std::all_of(v.begin(), v.end(), [](bool x) {
        return x;
      });
}
virtual value_type monus(value_type x, value_type y) const
{
  return x & !y;
}
virtual value_type delta(value_type x) const
{
  return x;
}
};
}

#endif /* BOOLEAN_H */
