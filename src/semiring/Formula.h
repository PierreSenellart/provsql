#ifndef FORMULA_H
#define FORMULA_H

#include <numeric>
#include <vector>
#include <string>
#include <sstream>
#include <iterator>

#include "Semiring.h"

template <typename Range, typename Value = typename Range::value_type>
static std::string join(Range const& elements, const char *const delimiter) {
  std::ostringstream os;
  auto b = begin(elements), e = end(elements);

  if (b != e) {
    std::copy(b, prev(e), std::ostream_iterator<Value>(os, delimiter));
    b = prev(e);
  }
  if (b != e) {
    os << *b;
  }

  return os.str();
}

namespace semiring {
class Formula : public semiring::Semiring<std::string>
{
public:
virtual value_type zero() const {
  return "𝟘";
}
virtual value_type one() const {
  return "𝟙";
}
virtual value_type plus(const std::vector<value_type> &v) const {
  if(v.size()==0)
    return zero();
  else if(v.size()==1)
    return v[0];
  else
    return "("+join(v, " ⊕ ")+")";
}
virtual value_type times(const std::vector<value_type> &v) const {
  if(v.size()==0)
    return one();
  else if(v.size()==1)
    return v[0];
  else
    return "("+join(v, " ⊗ ")+")";
}
virtual value_type monus(value_type x, value_type y) const
{
  return "("+x+" ⊖ "+y+")";
}
virtual value_type delta(value_type x) const
{
  if(x[0]=='(')
    return "δ"+x;
  else
    return "δ("+x+")";
}
};
}

#endif /* FORMULA_H */
